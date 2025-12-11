#include "board.h"
#include "display.h"
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>


#define CONTINUE_PLAY 0
#define NEXT_LEVEL 1
#define QUIT_GAME 2
#define LOAD_BACKUP 3
#define CREATE_BACKUP 4

int game_state = CONTINUE_PLAY;
pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t board_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    int ghost_index;
    board_t * game_board;
} ghost_arg_t;


static int get_game_state(void) {
    pthread_mutex_lock(&state_mutex);
    int state = game_state;
    pthread_mutex_unlock(&state_mutex);
    return state;
}

static void set_game_state(int state) {
    pthread_mutex_lock(&state_mutex);
    game_state = state;
    pthread_mutex_unlock(&state_mutex);
}

void screen_refresh(board_t * game_board, int mode) {
    debug("REFRESH\n");
    draw_board(game_board, mode);
    refresh_screen();
    if(game_board->tempo != 0)
        sleep_ms(game_board->tempo);       
}

int play_pacman(board_t * game_board) {
    pacman_t* pacman = &game_board->pacmans[0];
    command_t* play;
    command_t c; 
    if (pacman->n_moves == 0) { // if is user input
        pthread_mutex_lock(&board_mutex);
        c.command = get_input();
        pthread_mutex_unlock(&board_mutex);
        
        if(c.command == '\0')
            return CONTINUE_PLAY;

        if(c.command == 'G')
            return CREATE_BACKUP;
        
        c.turns = 1;
        play = &c;
    }
    else { // else if the moves are pre-defined in the file
        play = &pacman->moves[pacman->current_move%pacman->n_moves];
    }

    debug("KEY %c\n", play->command);

    if (play->command == 'Q') {
        return QUIT_GAME;
    }
    
    pthread_mutex_lock(&board_mutex);
    int result = move_pacman(game_board, 0, play);
    pthread_mutex_unlock(&board_mutex);

    if (result == REACHED_PORTAL) {
        return NEXT_LEVEL;
    }

    if(result == DEAD_PACMAN) {
        return QUIT_GAME;
    }

    return CONTINUE_PLAY;
}


void *pacman_thread (void *arg){
    board_t * game_board = (board_t *) arg;
    set_game_state(CONTINUE_PLAY);

    while(get_game_state() == CONTINUE_PLAY){
        int state = play_pacman(game_board);
        if (state != CONTINUE_PLAY) {
            set_game_state(state);
            break;
        }
        sleep_ms(80); 
    }

    return NULL;
}

int play_ghosts(board_t *game_board, int ghost_index) {
    ghost_t* ghost = &game_board->ghosts[ghost_index];
    
    pthread_mutex_lock(&board_mutex);
    move_ghost(game_board, ghost_index, &ghost->moves[ghost->current_move % ghost->n_moves]);
    pthread_mutex_unlock(&board_mutex);

    if (!game_board->pacmans[0].alive) {
        return QUIT_GAME;
    }      

    return CONTINUE_PLAY;
}

void *ghost_thread (void *arg){
    ghost_arg_t * ghost_arg = (ghost_arg_t *) arg;
    board_t * game_board = ghost_arg->game_board;
    int ghost_index = ghost_arg->ghost_index;

    while(get_game_state() == CONTINUE_PLAY){
        int state = play_ghosts(game_board, ghost_index);
        if (state != CONTINUE_PLAY) {
            set_game_state(state);
            break;
        }
        sleep_ms(200);
    }
    return NULL;
}

void *ncurses_thread (void *arg){
    while (get_game_state() == CONTINUE_PLAY) {
        pthread_mutex_lock(&board_mutex);
        screen_refresh((board_t *) arg, DRAW_MENU);
        pthread_mutex_unlock(&board_mutex);
        sleep_ms(100); // Adjust refresh rate as needed
    }
    return NULL;
}

int is_lvl_file(char *filename) {
    int len = strlen(filename);
    if (len < 4) return 0;
    return strcmp(filename + (len - 4), ".lvl") == 0;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        printf("Usage: %s <level_directory>\n", argv[0]);
        return EXIT_FAILURE;
    }
    
    DIR * dir = opendir(argv[1]);
    if (!dir) {
        perror("opendir");
        return EXIT_FAILURE;
    }

    // Random seed for any random movements
    srand((unsigned int)time(NULL));

    open_debug_file("debug.log");

    terminal_init();
    
    int accumulated_points = 0;
    bool end_game = false;
    board_t game_board;
    
    char level_files[MAX_LEVELS][MAX_FILENAME];
    int n_levels = 0;
    int has_backup = 0;
    
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {
        if (!is_lvl_file(entry->d_name))
            continue;
            
        strncpy(level_files[n_levels], entry->d_name, MAX_FILENAME - 1);
        level_files[n_levels][MAX_FILENAME - 1] = '\0';
        n_levels++;

        if (n_levels >= MAX_LEVELS)
            break;
    }

    closedir(dir);

    int lvl_index = 0;
    while (lvl_index < n_levels && !end_game) {
        char filepath[BIG_BUFFER_SIZE];

        snprintf(filepath, sizeof(filepath), "%s/%s", argv[1], level_files[lvl_index]);
        if (load_level(&game_board, accumulated_points, filepath, argv[1]) != 0) {
            perror("Failed to load level");
            return EXIT_FAILURE;
        }

        int level_finished = 0;

        while (!level_finished && !end_game) {
            set_game_state(CONTINUE_PLAY);

            //Criação das threads
            pthread_t ncurses_tid;
            if (pthread_create(&ncurses_tid, NULL, ncurses_thread, (void *)&game_board) != 0) {
                perror("pthread_create ncurses");
                end_game = true;
                break;
            }

            pthread_t pac_thread;
            if (pthread_create(&pac_thread, NULL, pacman_thread, (void *)&game_board) != 0) {
                perror("pthread_create pacman");
                pthread_join(ncurses_tid, NULL);
                end_game = true;
                break;
            }

            pthread_t ghost_threads[MAX_GHOSTS];
            ghost_arg_t ghost_args[MAX_GHOSTS];
            for (int i = 0; i < game_board.n_ghosts; i++) {
                ghost_args[i].ghost_index = i;
                ghost_args[i].game_board  = &game_board;
                if (pthread_create(&ghost_threads[i], NULL, ghost_thread, (void *)&ghost_args[i]) != 0) {
                    perror("pthread_create ghost");
                    end_game = true;
                    break;
                }
            }

            //Espera pelo fim das threads
            pthread_join(pac_thread, NULL);
            pthread_join(ncurses_tid, NULL);
            for (int i = 0; i < game_board.n_ghosts; i++) {
                pthread_join(ghost_threads[i], NULL);
            }

            int state = get_game_state();

            if (state == CREATE_BACKUP) {
                if(has_backup == 1){
                    set_game_state(CONTINUE_PLAY);
                    continue;
                }
                pid_t pid = fork();
                if (pid < 0) {
                    perror("fork");
                    has_backup = 0;
                    set_game_state(CONTINUE_PLAY);
                    continue;
                } else if (pid == 0) {
                    has_backup = 1;
                    set_game_state(CONTINUE_PLAY);
                    continue;
                } else {
                    int status;
                    has_backup = 1;
                    if (wait(&status) < 0) {
                        perror("wait");
                        end_game = true;
                        level_finished = 1;
                        break;
                    }

                    if (WEXITSTATUS(status) == 1) {
                        has_backup = 0;
                        set_game_state(CONTINUE_PLAY);
                        continue;
                    }

                    end_game = true;
                    level_finished = 1;
                    break;
                }
            }

            if (state == NEXT_LEVEL) {
                screen_refresh(&game_board, DRAW_WIN);
                sleep_ms(game_board.tempo);
                accumulated_points = game_board.pacmans[0].points;
                print_board(&game_board);
                level_finished = 1;
                continue;
            }

            if (state == QUIT_GAME) {
                if (has_backup == 1 && game_board.pacmans[0].alive == 0) {
                    exit(1);
                } else if (has_backup == 1) {
                    exit(0);
                } else {
                    screen_refresh(&game_board, DRAW_GAME_OVER);
                    sleep_ms(game_board.tempo);
                    end_game = true;
                    level_finished = 1;
                    break;
                }
            }

            screen_refresh(&game_board, DRAW_MENU);
            accumulated_points = game_board.pacmans[0].points;
            print_board(&game_board);
        }

        if (level_finished) {
            lvl_index++;
        }
        unload_level(&game_board);

    }
            
    terminal_cleanup();

    close_debug_file();

    return 0;
}
