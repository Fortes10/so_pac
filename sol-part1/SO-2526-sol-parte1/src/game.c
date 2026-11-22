#include "board.h"
#include "parser.h"
#include "protocol.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>

#define PATH_MAX 4096

typedef struct {
    int request_fd;
    int notification_fd;
    board_t *board;

    int stop_level;
    int stop_session; 
    int level_finished; 

    int session_index;
} session_info_t;

typedef struct {
    board_t *board;
    int ghost_index;
    session_info_t *session_info;
} ghost_thread_args_t;

typedef struct {
    char request_pipe[MAX_PIPE_PATH_LENGTH];
    char notification_pipe[MAX_PIPE_PATH_LENGTH];
} session_request_t;

typedef struct {
    session_request_t *buffer;
    int capacity;
    int head;
    int tail;
    pthread_mutex_t mutex;
    sem_t available_slots; // empty slots
    sem_t available_items; // queued items
} session_request_queue_t;

typedef struct {
    char levels_dir[PATH_MAX];
    session_request_queue_t *queue;
    int session_index;
} session_args_t;

typedef struct {
    int active;
    int client_id;   
    int score;       
} active_client_t;

active_client_t *active_clients = NULL;
int max_games = 0;
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

// flag só para o sinal
int sigusr1_received = 0;

// Extrai o id do cliente a partir de /tmp/<id>_request
int client_id(char *request_pipe_path) {
    int id = 0;
    for (int i = 5; request_pipe_path[i] >= '0' && request_pipe_path[i] <= '9'; i++) {
        id = id * 10 + (request_pipe_path[i] - '0');
    }
    return id;
}

// Marca cliente como conectado
void set_client_connected(int session_index, int client_id) {
    pthread_mutex_lock(&lock);
    active_clients[session_index].active = 1;
    active_clients[session_index].client_id = client_id;
    active_clients[session_index].score = 0;
    pthread_mutex_unlock(&lock);
}

// Atualiza a pontuação do cliente
void update_client_score(int session_index, int score) {
    pthread_mutex_lock(&lock);
    if (active_clients[session_index].active) {
        active_clients[session_index].score = score;
    }
    pthread_mutex_unlock(&lock);
}

// Marca cliente como desconectado
void set_client_disconnected(int session_index) {
    pthread_mutex_lock(&lock);
    active_clients[session_index].active = 0;
    active_clients[session_index].client_id = -1;
    active_clients[session_index].score = 0;
    pthread_mutex_unlock(&lock);
}

int cmp_score_desc(const void *a, const void *b) {
    active_client_t *x = (active_client_t *)a;
    active_client_t *y = (active_client_t *)b;
    return (y->score - x->score);
}

// Cria ficheiro top-5
void write_top5() {
    pthread_mutex_lock(&lock);

    active_client_t *clients_copy = malloc((size_t)max_games * sizeof(*clients_copy));
    if (!clients_copy) {
        pthread_mutex_unlock(&lock);
        return;
    }

    int count = 0;
    for (int i = 0; i < max_games; i++) {
        if (active_clients[i].active) {
            clients_copy[count++] = active_clients[i];
        }
    }

    pthread_mutex_unlock(&lock);

    qsort(clients_copy, (size_t)count, sizeof(*clients_copy), cmp_score_desc);

    FILE *fp = fopen("top5_clients.txt", "w");
    if (fp) {
        int limit = (count < 5) ? count : 5;
        for (int i = 0; i < limit; i++) {
            fprintf(fp, "client_id=%d score=%d\n",
                        clients_copy[i].client_id, clients_copy[i].score);
        }
        if (limit == 0) 
            fprintf(fp, "nenhum cliente ligado\n");
        fclose(fp);
    }

    free(clients_copy);
}


// sends the entire buffer, retrying as needed
int send_msg(int fd, void *str, size_t n) {
  size_t written = 0;

  while (written < n) {
    ssize_t ret = write(fd, str + written, n - written);
    if (ret < 0) return -1;

    written += ret;
  }
  return 0;
}

// reads exactly n bytes into buf, retrying as needed
int read_msg(int fd, void *buf, size_t n) {
  size_t seen = 0;

  while (seen < n) {
    ssize_t ret = read(fd, buf + seen, n - seen);
    if (ret == 0) return 0;   // EOF
    if (ret < 0) return -1;

    seen += ret;
  }
  return 1;
}

// Ghost thread
void *ghost_thread(void *arg) {
    ghost_thread_args_t *ghost_args = (ghost_thread_args_t *)arg;
    board_t *board = ghost_args->board;
    ghost_t *ghost = &board->ghosts[ghost_args->ghost_index];
    session_info_t *session_info = ghost_args->session_info;

    while (!session_info->stop_level) {
        sleep_ms(board->tempo * (1 + ghost->passo));

        pthread_rwlock_rdlock(&board->state_lock);
        if (!session_info->stop_level && ghost->n_moves > 0) 
            move_ghost(board, ghost_args->ghost_index, &ghost->moves[ghost->current_move % ghost->n_moves]);
        pthread_rwlock_unlock(&board->state_lock);
    }

    free(ghost_args);
    return NULL;
}

// Pacman request thread
void *pacman_thread(void *arg) {
    session_info_t *session_info = (session_info_t *)arg;
    board_t *board = session_info->board;

    while (!session_info->stop_level) {
        // Read opcode
        char opcode;
        int read_result = read_msg(session_info->request_fd, &opcode, 1);
        if (read_result <= 0) {
            session_info->stop_session = 1;
            session_info->stop_level = 1;
            break;
        }

        if (opcode == OP_CODE_DISCONNECT) {
            session_info->stop_session = 1;
            session_info->stop_level = 1;
            break;
        }

        if (opcode != OP_CODE_PLAY) continue;

        // Read command
        char command;
        if (read_msg(session_info->request_fd, &command, 1) <= 0) {
            session_info->stop_session = 1;
            session_info->stop_level = 1;
            break;
        }
        if (command == 'G') continue;

        command_t pacman_cmd = {.command = command, .turns = 1, .turns_left = 1};

        sleep_ms(board->tempo * (1 + board->pacmans[0].passo));

        pthread_rwlock_rdlock(&board->state_lock);
        int result = move_pacman(board, 0, &pacman_cmd);
        pthread_rwlock_unlock(&board->state_lock);
        
        if (result == DEAD_PACMAN) {
            session_info->stop_session = 1;
            session_info->stop_level = 1;
            break;
        }
        
        if (result == REACHED_PORTAL) {
            session_info->level_finished = 1;
            session_info->stop_level = 1;
            break;
        }
    }
    return NULL;
}


// Notifier thread (periodically sends board updates)
void *board_notifier_thread(void *arg) {
    session_info_t *session_info = (session_info_t *)arg;
    board_t *board = session_info->board;

    while (1) {
        sleep_ms(board->tempo);

        pthread_rwlock_rdlock(&board->state_lock);

        int width = board->width;
        int height = board->height;
        int tempo = board->tempo;

        int victory = session_info->level_finished ? 1 : 0;
        int game_over = session_info->stop_session ? 1 : 0;
        int points = board->pacmans[0].points;

        char *data = malloc(board->width * board->height);
        if (!data) {
            pthread_rwlock_unlock(&board->state_lock);
            session_info->stop_session = 1;
            session_info->stop_level = 1;
            break;
        }


        // Fill board data
        for (int y = 0; y < board->height; y++) {
            for (int x = 0; x < board->width; x++) {
                int index = y * board->width + x;
                char content = board->board[index].content;

                if (content == 'W') data[index] = '#';
                else if (content == 'P') data[index] = 'C';
                else if (content == 'M') data[index] = 'M';
                else if (board->board[index].has_portal) data[index] = '@';
                else if (board->board[index].has_dot) data[index] = '.';
                else data[index] = ' ';
            }
        }

        pthread_rwlock_unlock(&board->state_lock);

        update_client_score(session_info->session_index, points);

        // Send board update
        char opcode = OP_CODE_BOARD;
        size_t board_size = width * height;

        if (send_msg(session_info->notification_fd, &opcode, 1) != 0 ||
            send_msg(session_info->notification_fd, &width, sizeof(int)) != 0 ||
            send_msg(session_info->notification_fd, &height, sizeof(int)) != 0 ||
            send_msg(session_info->notification_fd, &tempo, sizeof(int)) != 0 ||
            send_msg(session_info->notification_fd, &victory, sizeof(int)) != 0 ||
            send_msg(session_info->notification_fd, &game_over, sizeof(int)) != 0 ||
            send_msg(session_info->notification_fd, &points, sizeof(int)) != 0 ||
            send_msg(session_info->notification_fd, data, board_size) != 0) {
            free(data);
            session_info->stop_session = 1;
            session_info->stop_level = 1;
            break;
        }

        free(data);

        if (session_info->stop_level) break;
        if (session_info->stop_session) break;
    }

    return NULL;
}

// Runs a game session for a client
int run_session(char const *levels_dir, char const *request_pipe_path,
                    char const *notification_pipe_path,int session_index) {

    // Open request and notification pipes


    int notification_fd = open(notification_pipe_path, O_WRONLY);
    if (notification_fd < 0) {
        perror("[server] open(notification_pipe)");
        return -1;
    }

    // Send connect acknowledgment
    char connect_ack[2];
    connect_ack[0] = OP_CODE_CONNECT;
    connect_ack[1] = 0;
    if (send_msg(notification_fd, &connect_ack, sizeof(connect_ack)) != 0) {
        perror("[server] write(connect_ack)");
        close(notification_fd);
        return -1;
    }

    int request_fd = open(request_pipe_path, O_RDONLY);
    if (request_fd < 0) {
        close(notification_fd);
        perror("[server] open(request_pipe)");
        return -1;
    }

    // Open levels directory
    DIR *levels_directory = opendir(levels_dir);
    if (!levels_directory) {
        perror("[server] opendir(levels_dir)");
        close(request_fd);
        close(notification_fd);
        return -1;
    }

    srand((unsigned)time(NULL));

    int accumulated_points = 0;
    bool end_session = false;

    // Iterate through levels
    struct dirent *entry;
    while (!end_session && (entry = readdir(levels_directory)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char *dot = strrchr(entry->d_name, '.');
        if (!dot || strcmp(dot, ".lvl") != 0) continue;

        board_t game_board;
        if (load_level(&game_board, entry->d_name, (char *)levels_dir, accumulated_points) != 0) {
            debug("[server] load_level failed for %s\n", entry->d_name);
            end_session = true;
            break;
        }

        session_info_t session_info = {
            .request_fd = request_fd,
            .notification_fd = notification_fd,
            .board = &game_board,
            .stop_level = 0,
            .stop_session = 0,
            .level_finished = 0,
            .session_index = session_index,
        };

        // Create threads
        pthread_t pacman_tid;
        pthread_t notifier_tid;

        pthread_t *ghost_tids = calloc(game_board.n_ghosts, sizeof(pthread_t));

        if (!ghost_tids) {
            free(ghost_tids);
            unload_level(&game_board);
            end_session = true;
            break;
        }

        if (pthread_create(&pacman_tid, NULL, pacman_thread, &session_info) != 0) {
            free(ghost_tids);
            unload_level(&game_board);
            end_session = true;
            break;
        }

        if (pthread_create(&notifier_tid, NULL, board_notifier_thread, &session_info) != 0) {
            session_info.stop_level = 1;
            pthread_join(pacman_tid, NULL);
            free(ghost_tids);
            unload_level(&game_board);
            end_session = true;
            break;
        }

        for (int i = 0; i < game_board.n_ghosts; i++) {
            ghost_thread_args_t *ghost_args = malloc(sizeof(*ghost_args));

            ghost_args->board = &game_board;
            ghost_args->ghost_index = i;
            ghost_args->session_info = &session_info;

            if (pthread_create(&ghost_tids[i], NULL, ghost_thread, ghost_args) != 0) {
                session_info.stop_level = 1;
                pthread_join(pacman_tid, NULL);
                free(ghost_tids);
                unload_level(&game_board);
                end_session = true;
                break;
            }
        }

        // Wait for threads to finish
        pthread_join(pacman_tid, NULL);
        session_info.stop_level = 1;

        pthread_join(notifier_tid, NULL);
        for (int i = 0; i < game_board.n_ghosts; i++) {
            pthread_join(ghost_tids[i], NULL);
        }

        free(ghost_tids);

        accumulated_points = game_board.pacmans[0].points;

        if (session_info.stop_session) 
            end_session = true;

        unload_level(&game_board);
    }

    closedir(levels_directory);
    close(request_fd);
    close(notification_fd);
    return 0;
}

// Producer-consumer implementation
void session_queue_init(session_request_queue_t *queue, int capacity) {
    queue->buffer = calloc(capacity, sizeof(session_request_t));
    queue->capacity = capacity;
    queue->head = 0;
    queue->tail = 0;

    pthread_mutex_init(&queue->mutex, NULL);
    sem_init(&queue->available_slots, 0, capacity);
    sem_init(&queue->available_items, 0, 0);
}

void session_producer(session_request_queue_t *queue, session_request_t const *request) {
    sem_wait(&queue->available_slots);
    pthread_mutex_lock(&queue->mutex);

    queue->buffer[queue->head] = *request;
    queue->head = (queue->head + 1) % queue->capacity;

    pthread_mutex_unlock(&queue->mutex);
    sem_post(&queue->available_items);
}

void session_consumer(session_request_queue_t *queue, session_request_t *out_request) {
    sem_wait(&queue->available_items);
    pthread_mutex_lock(&queue->mutex);

    *out_request = queue->buffer[queue->tail];
    queue->tail = (queue->tail + 1) % queue->capacity;

    pthread_mutex_unlock(&queue->mutex);
    sem_post(&queue->available_slots);
}

// Session thread function
void *session_thread(void *arg) {
    session_args_t *worker_args = (session_args_t *)arg;

    while (1) {
        session_request_t session_request;
        session_consumer(worker_args->queue, &session_request);

        int id = client_id(session_request.request_pipe);
        set_client_connected(worker_args->session_index, id);

        run_session(worker_args->levels_dir, session_request.request_pipe,
                session_request.notification_pipe, worker_args->session_index);

        set_client_disconnected(worker_args->session_index);
    }

    return NULL;
}

// Signal handler for SIGUSR1
static void sigusr1_handler() {
    sigusr1_received = 1;
    signal(SIGUSR1, sigusr1_handler);
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <levels_dir> <max_games> <register_fifo>\n", argv[0]);
        return 1;
    }

    // Prevent server from dying when writing to a closed FIFO
    signal(SIGPIPE, SIG_IGN);

    char const *levels_dir = argv[1];
    max_games = atoi(argv[2]);

    char const *register_fifo_path = argv[3];

    open_debug_file("server-debug.log");

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    // Setup SIGUSR1 handler
    if (signal(SIGUSR1, sigusr1_handler) == SIG_ERR) {
        perror("signal(SIGUSR1)");
        return 1;
    }

    active_clients = calloc(max_games, sizeof(active_client_t));
    if (!active_clients) {
        perror("[server] calloc(active_clients)");
        return 1;
    }
    for (int i = 0; i < max_games; i++) {
        active_clients[i].active = 0;
        active_clients[i].client_id = -1;
        active_clients[i].score = 0;
    }

    // Create register FIFO
    if (unlink(register_fifo_path) != 0 && errno != ENOENT) {
        perror("[server] unlink(register_fifo)");
        return 1;
    }

    if (mkfifo(register_fifo_path, 0640) != 0 && errno != EEXIST) {
        perror("[server] mkfifo(register_fifo)");
        return 1;
    }

    // Producer-consumer queue
    int queue_capacity = max_games * 5;
    if (queue_capacity < 5) queue_capacity = 5;

    session_request_queue_t request_queue;
    session_queue_init(&request_queue, queue_capacity);

    // Create workers at startup
    pthread_t *worker_threads = malloc(max_games * sizeof(pthread_t));
    session_args_t *worker_args = calloc(max_games, sizeof(session_args_t));
    if (!worker_threads || !worker_args) {
        perror("[server] malloc/calloc(worker args)");
        return 1;
    }

    for (int i = 0; i < max_games; i++) {
        worker_args[i].queue = &request_queue;
        worker_args[i].session_index = i;

        strncpy(worker_args[i].levels_dir, levels_dir, sizeof(worker_args[i].levels_dir) - 1);
        worker_args[i].levels_dir[sizeof(worker_args[i].levels_dir) - 1] = '\0';

        pthread_create(&worker_threads[i], NULL, session_thread, &worker_args[i]);
    }

    pthread_sigmask(SIG_UNBLOCK, &set, NULL);

    // open register FIFO 
    int register_fd = open(register_fifo_path, O_RDONLY);
    if (register_fd < 0) {
        perror("[server] open(register_fifo, RDONLY)");
        return 1;
    }

    while (1) {
        // se recebeu SIGUSR1, cria ficheiro top-5
        if (sigusr1_received) {
            sigusr1_received = 0;
            write_top5();
        }

        // Read CONNECT request
        char connect_msg[1 + 2 * MAX_PIPE_PATH_LENGTH];
        int read_result = read_msg(register_fd, &connect_msg, sizeof(connect_msg));
        if (read_result == 0) continue;
        if (read_result < 0) {
            perror("[server] read(register_fifo)");
            continue;
        }
        char opcode = connect_msg[0];
        if (opcode != OP_CODE_CONNECT) continue;

        char request_pipe[MAX_PIPE_PATH_LENGTH];
        char notification_pipe[MAX_PIPE_PATH_LENGTH];

        memcpy(request_pipe, connect_msg + 1, MAX_PIPE_PATH_LENGTH);
        request_pipe[MAX_PIPE_PATH_LENGTH - 1] = '\0';

        memcpy(notification_pipe, connect_msg + 1 + MAX_PIPE_PATH_LENGTH, MAX_PIPE_PATH_LENGTH);
        notification_pipe[MAX_PIPE_PATH_LENGTH - 1] = '\0';

        session_request_t session_request;
        strncpy(session_request.request_pipe, request_pipe, MAX_PIPE_PATH_LENGTH - 1);
        session_request.request_pipe[MAX_PIPE_PATH_LENGTH - 1] = '\0';

        strncpy(session_request.notification_pipe, notification_pipe, MAX_PIPE_PATH_LENGTH - 1);
        session_request.notification_pipe[MAX_PIPE_PATH_LENGTH - 1] = '\0';

        session_producer(&request_queue, &session_request);
    }

    close(register_fd);
    return 0;
}
