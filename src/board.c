#include "board.h"
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h> 

FILE * debugfile;

// Helper private function to find and kill pacman at specific position
static int find_and_kill_pacman(board_t* board, int new_x, int new_y) {
    for (int p = 0; p < board->n_pacmans; p++) {
        pacman_t* pac = &board->pacmans[p];
        if (pac->pos_x == new_x && pac->pos_y == new_y && pac->alive) {
            pac->alive = 0;
            kill_pacman(board, p);
            return DEAD_PACMAN;
        }
    }
    return VALID_MOVE;
}

// Helper private function for getting board position index
static inline int get_board_index(board_t* board, int x, int y) {
    return y * board->width + x;
}

// Helper private function for checking valid position
static inline int is_valid_position(board_t* board, int x, int y) {
    return (x >= 0 && x < board->width) && (y >= 0 && y < board->height); // Inside of the board boundaries
}

void sleep_ms(int milliseconds) {
    struct timespec ts;
    ts.tv_sec = milliseconds / 1000;
    ts.tv_nsec = (milliseconds % 1000) * 1000000;
    nanosleep(&ts, NULL);
}

int move_pacman(board_t* board, int pacman_index, command_t* command) {
    if (pacman_index < 0 || !board->pacmans[pacman_index].alive) {
        return DEAD_PACMAN; // Invalid or dead pacman
    }

    pacman_t* pac = &board->pacmans[pacman_index];
    int new_x = pac->pos_x;
    int new_y = pac->pos_y;

    // check passo
    if (pac->waiting > 0) {
        pac->waiting -= 1;
        return VALID_MOVE;        
    }
    pac->waiting = pac->passo;

    char direction = command->command;

    if (direction == 'R') {
        char directions[] = {'W', 'S', 'A', 'D'};
        direction = directions[rand() % 4];
    }

    // Calculate new position based on direction
    switch (direction) {
        case 'W': // Up
            new_y--;
            break;
        case 'S': // Down
            new_y++;
            break;
        case 'A': // Left
            new_x--;
            break;
        case 'D': // Right
            new_x++;
            break;
        case 'T': // Wait
            if (command->turns_left == 1) {
                pac->current_move += 1; // move on
                command->turns_left = command->turns;
            }
            else command->turns_left -= 1;
            return VALID_MOVE;
        default:
            return INVALID_MOVE; // Invalid direction
    }

    // Logic for the WASD movement
    pac->current_move+=1;

    // Check boundaries
    if (!is_valid_position(board, new_x, new_y)) {
        return INVALID_MOVE;
    }

    int new_index = get_board_index(board, new_x, new_y);
    int old_index = get_board_index(board, pac->pos_x, pac->pos_y);
    char target_content = board->board[new_index].content;

    if (board->board[new_index].has_portal) {
        if (board->board[new_index].has_dot) {
            pac->points++;
            board->board[new_index].has_dot = 0;
        }
        board->board[old_index].content = ' ';
        pac->pos_x = new_x;
        pac->pos_y = new_y;
        board->board[new_index].content = 'P';
        return REACHED_PORTAL;
    }

    // Check for walls
    if (target_content == 'W') {
        return INVALID_MOVE;
    }

    // Check for ghosts
    if (target_content == 'M') {
        kill_pacman(board, pacman_index);
        return DEAD_PACMAN;
    }

    // Collect points
    if (board->board[new_index].has_dot) {
        pac->points++;
        board->board[new_index].has_dot = 0;
    }

    board->board[old_index].content = ' ';
    pac->pos_x = new_x;
    pac->pos_y = new_y;
    board->board[new_index].content = 'P';

    return VALID_MOVE;
}

// Helper private function for charged ghost movement in one direction
static int move_ghost_charged_direction(board_t* board, ghost_t* ghost, char direction, int* new_x, int* new_y) {
    int x = ghost->pos_x;
    int y = ghost->pos_y;
    *new_x = x;
    *new_y = y;
    
    switch (direction) {
        case 'W': // Up
            if (y == 0) return INVALID_MOVE;
            *new_y = 0; // In case there is no colision
            for (int i = y - 1; i >= 0; i--) {
                char target_content = board->board[get_board_index(board, x, i)].content;
                if (target_content == 'W' || target_content == 'M') {
                    *new_y = i + 1; // stop before colision
                    return VALID_MOVE;
                }
                else if (target_content == 'P') {
                    *new_y = i;
                    return find_and_kill_pacman(board, *new_x, *new_y);
                }
            }
            break;

        case 'S': // Down
            if (y == board->height - 1) return INVALID_MOVE;
            *new_y = board->height - 1; // In case there is no colision
            for (int i = y + 1; i < board->height; i++) {
                char target_content = board->board[get_board_index(board, x, i)].content;
                if (target_content == 'W' || target_content == 'M') {
                    *new_y = i - 1; // stop before colision
                    return VALID_MOVE;
                }
                if (target_content == 'P') {
                    *new_y = i;
                    return find_and_kill_pacman(board, *new_x, *new_y);
                }
            }
            break;

        case 'A': // Left
            if (x == 0) return INVALID_MOVE;
            *new_x = 0; // In case there is no colision
            for (int j = x - 1; j >= 0; j--) {
                char target_content = board->board[get_board_index(board, j, y)].content;
                if (target_content == 'W' || target_content == 'M') {
                    *new_x = j + 1; // stop before colision
                    return VALID_MOVE;
                }
                if (target_content == 'P') {
                    *new_x = j;
                    return find_and_kill_pacman(board, *new_x, *new_y);
                }
            }
            break;

        case 'D': // Right
            if (x == board->width - 1) return INVALID_MOVE;
            *new_x = board->width - 1; // In case there is no colision
            for (int j = x + 1; j < board->width; j++) {
                char target_content = board->board[get_board_index(board, j, y)].content;
                if (target_content == 'W' || target_content == 'M') {
                    *new_x = j - 1; // stop before colision
                    return VALID_MOVE;
                }
                if (target_content == 'P') {
                    *new_x = j;
                    return find_and_kill_pacman(board, *new_x, *new_y);
                }
            }
            break;
        default:
            debug("DEFAULT CHARGED MOVE - direction = %c\n", direction);
            return INVALID_MOVE;
    }
    return VALID_MOVE;
}   

int move_ghost_charged(board_t* board, int ghost_index, char direction) {
    ghost_t* ghost = &board->ghosts[ghost_index];
    int x = ghost->pos_x;
    int y = ghost->pos_y;
    int new_x = x;
    int new_y = y;

    ghost->charged = 0; //uncharge
    int result = move_ghost_charged_direction(board, ghost, direction, &new_x, &new_y);
    if (result == INVALID_MOVE) {   
        debug("DEFAULT CHARGED MOVE - direction = %c\n", direction);
        return INVALID_MOVE;
    }

    // Get board indices
    int old_index = get_board_index(board, ghost->pos_x, ghost->pos_y);
    int new_index = get_board_index(board, new_x, new_y);

    // Update board - clear old position (restore what was there)
    board->board[old_index].content = ' '; // Or restore the dot if ghost was on one
    // Update ghost position
    ghost->pos_x = new_x;
    ghost->pos_y = new_y;
    // Update board - set new position
    board->board[new_index].content = 'M';
    return result;
}

int move_ghost(board_t* board, int ghost_index, command_t* command) {
    ghost_t* ghost = &board->ghosts[ghost_index];
    int new_x = ghost->pos_x;
    int new_y = ghost->pos_y;

    // check passo
    if (ghost->waiting > 0) {
        ghost->waiting -= 1;
        return VALID_MOVE;
    }
    ghost->waiting = ghost->passo;

    char direction = command->command;
    
    if (direction == 'R') {
        char directions[] = {'W', 'S', 'A', 'D'};
        direction = directions[rand() % 4];
    }

    // Calculate new position based on direction
    switch (direction) {
        case 'W': // Up
            new_y--;
            break;
        case 'S': // Down
            new_y++;
            break;
        case 'A': // Left
            new_x--;
            break;
        case 'D': // Right
            new_x++;
            break;
        case 'C': // Charge
            ghost->current_move += 1;
            ghost->charged = 1;
            return VALID_MOVE;
        case 'T': // Wait
            if (command->turns_left == 1) {
                ghost->current_move += 1; // move on
                command->turns_left = command->turns;
            }
            else command->turns_left -= 1;
            return VALID_MOVE;
        default:
            return INVALID_MOVE; // Invalid direction
    }

    // Logic for the WASD movement
    ghost->current_move++;
    if (ghost->charged)
        return move_ghost_charged(board, ghost_index, direction);

    // Check boundaries
    if (!is_valid_position(board, new_x, new_y)) {
        return INVALID_MOVE;
    }

    // Check board position
    int new_index = get_board_index(board, new_x, new_y);
    int old_index = get_board_index(board, ghost->pos_x, ghost->pos_y);
    char target_content = board->board[new_index].content;

    // Check for walls and ghosts
    if (target_content == 'W' || target_content == 'M') {
        return INVALID_MOVE;
    }

    int result = VALID_MOVE;
    // Check for pacman
    if (target_content == 'P') {
        result = find_and_kill_pacman(board, new_x, new_y);
    }

    // Update board - clear old position (restore what was there)
    board->board[old_index].content = ' '; // Or restore the dot if ghost was on one

    // Update ghost position
    ghost->pos_x = new_x;
    ghost->pos_y = new_y;

    // Update board - set new position
    board->board[new_index].content = 'M';
    return result;
}

void kill_pacman(board_t* board, int pacman_index) {
    debug("Killing %d pacman\n\n", pacman_index);
    pacman_t* pac = &board->pacmans[pacman_index];
    int index = pac->pos_y * board->width + pac->pos_x;

    // Remove pacman from the board
    board->board[index].content = ' ';

    // Mark pacman as dead
    pac->alive = 0;
}

void parse_pacman_line(board_t *board, pacman_t *pac, char *linha){
    if (linha[0] == '#')
        return;

    //PASSO N
    else if (strncmp(linha, "PASSO", 5) == 0) {
        sscanf(linha, "PASSO %d", &pac->passo);
        return;
    }

    //POS X Y
    else if (strncmp(linha, "POS", 3) == 0) {
        sscanf(linha, "POS %d %d", &pac->pos_y, &pac->pos_x) ;
        board->board[get_board_index(board, pac->pos_x, pac->pos_y)].content = 'P';
        return;
    }

    if (pac->n_moves >= MAX_MOVES) {
        return;
    }

    else if (linha[0] == 'T') {
        //T n : espera n jogadas
        int time = 0;
        sscanf(linha, "T %d", &time);
        pac->moves[pac->n_moves].command = 'T';
        pac->moves[pac->n_moves].turns = time;
        pac->moves[pac->n_moves].turns_left = time;
    } else {
        //Comandos simples: A,D,W,S,R,C
        pac->moves[pac->n_moves].command = linha[0];
        pac->moves[pac->n_moves].turns = 1;
        pac->moves[pac->n_moves].turns_left = 1;
    }

    pac->n_moves++;
}

int load_pacman(board_t* board, int points, char *filepath) {
    int fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        perror("open");
        return -1;
    }

    pacman_t *pac = &board->pacmans[0];

    // estado inicial
    pac->alive = 1;
    pac->points = points;
    pac->current_move = 0;
    pac->waiting = 0;
    pac->n_moves = 0;
    pac->passo = 0;

    char linha[Line_SIZE + 1];
    char buffer[BUF_SIZE + 1];
    int line_index = 0;
    ssize_t bytes;

    //Lê o ficheiro linha a linha
    while ((bytes = read(fd, buffer, BUF_SIZE)) > 0) {
        for (ssize_t i = 0; i < bytes; i++) {
            char c = buffer[i];

            if (c != '\n' && c != '\r') {
                linha[line_index++] = c;
            } 
            
            else {
                linha[line_index] = '\0';
                line_index = 0;

                //Processa a linha lida
                parse_pacman_line(board, pac, linha);
            }
        }
    }

    //Processa a última linha caso não tenha '\n'
    if (line_index > 0) {
        linha[line_index] = '\0';
        parse_pacman_line(board, pac, linha);
    }
    close(fd);

    return 0;
}

void parse_ghost_line(board_t *board, ghost_t *ghost, char *linha){
    if (linha[0] == '#')
        return;

    //PASSO N
    else if (strncmp(linha, "PASSO", 5) == 0) {
        sscanf(linha, "PASSO %d", &ghost->passo);
        return;
    }

    //POS X Y
    else if (strncmp(linha, "POS", 3) == 0) {
        sscanf(linha, "POS %d %d", &ghost->pos_y, &ghost->pos_x);
        board->board[get_board_index(board, ghost->pos_x, ghost->pos_y)].content = 'M';
        return;
    }

    else if (ghost->n_moves >= MAX_MOVES) {
        return;
    }

    if (linha[0] == 'T') {
        //T n : espera n jogadas
        int time = 0;
        sscanf(linha, "T %d", &time);
        ghost->moves[ghost->n_moves].command = 'T';
        ghost->moves[ghost->n_moves].turns = time;
        ghost->moves[ghost->n_moves].turns_left = time;
    } else {
        //Comandos simples: A,D,W,S,R,C
        ghost->moves[ghost->n_moves].command = linha[0];
        ghost->moves[ghost->n_moves].turns = 1;
        ghost->moves[ghost->n_moves].turns_left = 1;
    }

    ghost->n_moves++;
}


int load_ghost(board_t* board, int n_ghosts, char ghosts_files[MAX_GHOSTS][MAX_FILENAME]) {
    if (n_ghosts > MAX_GHOSTS)
        n_ghosts = MAX_GHOSTS;

    for (int i = 0; i < n_ghosts; i++) {
        ghost_t *ghost = &board->ghosts[i];

        ghost->pos_x = 0;
        ghost->pos_y = 0;
        ghost->passo = 0;
        ghost->waiting = 0;
        ghost->current_move = 0;
        ghost->n_moves = 0;
        ghost->charged = 0;

        int fd = open(ghosts_files[i], O_RDONLY);
        if (fd < 0) {
            perror("open");
            return -1;
        }

        char linha[Line_SIZE + 1];
        int line_index = 0;

        char buffer[BUF_SIZE + 1];
        ssize_t bytes;

        //Lê o ficheiro linha a linha
        while ((bytes = read(fd, buffer, BUF_SIZE)) > 0) {
            for (ssize_t i = 0; i < bytes; i++) {
                char c = buffer[i];

                if (c != '\n' && c != '\r') 
                    linha[line_index++] = c;

                else {
                    linha[line_index] = '\0';
                    line_index = 0;

                    //Processa a linha lida
                    parse_ghost_line(board, ghost, linha);
                }
            }
        }

        //Processa a última linha caso não tenha '\n'
        if (line_index > 0) {
            linha[line_index] = '\0';
            parse_ghost_line(board, ghost, linha);
        }

        close(fd);
    } 

    return 0;
}

void parse_level_line(board_t *board, char *linha, int *row){
    if (linha[0] == '#')
        return;

    //DIM H W
    else if (strncmp(linha, "DIM", 3) == 0)
        sscanf(linha, "DIM %d %d", &board->height, &board->width);

    //TEMPO t
    else if (strncmp(linha, "TEMPO", 5) == 0)
        sscanf(linha, "TEMPO %d", &board->tempo);

    //PAC nome
    else if (strncmp(linha, "PAC", 3) == 0)
        sscanf(linha, "PAC %s", board->pacman_file);

    //MON nomes
    else if (strncmp(linha, "MON", 3) == 0) {
        char *token = strtok(linha, " ");
        token = strtok(NULL, " ");
        while (token && board->n_ghosts < MAX_GHOSTS) {
            strncpy(board->ghosts_files[board->n_ghosts], token, MAX_FILENAME - 1);
            board->ghosts_files[board->n_ghosts][MAX_FILENAME - 1] = '\0';
            board->n_ghosts++;
            token = strtok(NULL, " ");
        }
    }
    else {
        //Linhas que representam o conteúdo da matriz do tabuleiro
        if(!board->board)
            board->board = calloc(board->width * board->height, sizeof(board_pos_t));
            
        if (*row >= board->height) {
            return; 
        }

        for (int j = 0; j < board->width && linha[j] != '\0'; j++) {
            char ch = linha[j];

            switch (ch) {
                case 'X':
                    board->board[get_board_index(board, j, *row)].content = 'W';
                    break;
                case 'o':
                    board->board[get_board_index(board, j, *row)].content = ' ';
                    board->board[get_board_index(board, j, *row)].has_dot = 1;
                    break;
                case '@':
                    board->board[get_board_index(board, j, *row)].content = ' ';
                    board->board[get_board_index(board, j, *row)].has_portal = 1;
                    break;
            }
        }
        (*row)++; 
    }
}

int load_level(board_t *board, int points, char *filepath, char *directory) {
    board->width = 0;
    board->height = 0;
    board->tempo = 0;
    
    board->n_pacmans = 1;
    board->n_ghosts = 0;
    
    board->pacman_file[0] = '\0';
    for (int i = 0; i < MAX_GHOSTS; i++) {
        board->ghosts_files[i][0] = '\0';
    }

    board->board = NULL;
    board->pacmans = NULL;
    board->ghosts = NULL;

    int fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        perror("open");
        return -1;
    }
    
    char linha[Line_SIZE + 1];
    int line_index = 0;

    char buffer[BUF_SIZE + 1];
    ssize_t bytes;

    int row = 0;

    //Lê o ficheiro linha a linha
    while ((bytes = read(fd, buffer, BUF_SIZE)) > 0) {
        for (ssize_t i = 0; i < bytes; i++) {
            char c = buffer[i];

            if (c != '\n' && c != '\r') 
                linha[line_index++] = c;

            else {
                linha[line_index] = '\0';
                line_index = 0;
                
                //Processa a linha lida
                parse_level_line(board, linha, &row);
            }
        }
    }
    
    //Processa a última linha caso não tenha '\n'
    if (line_index > 0) {
        linha[line_index] = '\0';
        parse_level_line(board, linha, &row);
    }

    close(fd);

    board->pacmans = calloc(board->n_pacmans, sizeof(pacman_t));
    char fullpath[BIG_BUFFER_SIZE];
    
    //Constrói path completo para o ficheiro .p
    if (board->pacman_file[0] != '\0') {
        fullpath[0] = '\0';
        snprintf(fullpath, sizeof(fullpath), "%s/%s", directory, board->pacman_file);

        load_pacman(board, points, fullpath); 
    } else {
        // Sem ficheiro PAC
        pacman_t *pac = &board->pacmans[0];
        pac->alive = 1;
        pac->points = points;
        pac->current_move = 0;
        pac->waiting = 0;
        pac->n_moves = 0;
        pac->passo = 0;

        int start_x = 1;
        int start_y = 1;

        int found = 0;
        if (board->board[get_board_index(board, start_x, start_y)].content == ' ' && 
                board->board[get_board_index(board, start_x, start_y)].has_dot == 1) {
            found = 1;
        } else {
            //Se a posição (1,1) estiver ocupada, procura a primeira posição livre
            for (int y = 0; y < board->height && !found; y++) {
                for (int x = 0; x < board->width && !found; x++) {
                    if (board->board[get_board_index(board, x, y)].content == ' ' && 
                            board->board[get_board_index(board, x, y)].has_dot == 1) {
                        start_x = x;
                        start_y = y;
                        found = 1;
                    }
                }
            }
        }
        pac->pos_x = start_x;
        pac->pos_y = start_y;
        board->board[get_board_index(board, pac->pos_x, pac->pos_y)].content = 'P';
    }

    board->ghosts = calloc(board->n_ghosts, sizeof(ghost_t));
    //Constrói paths completos para cada ficheiro .m
    for (int i = 0; i < board->n_ghosts; i++) {
        char fullpath[BIG_BUFFER_SIZE];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", directory, board->ghosts_files[i]);
        strncpy(board->ghosts_files[i], fullpath, MAX_FILENAME - 1);
        board->ghosts_files[i][MAX_FILENAME - 1] = '\0';
    }
    load_ghost(board, board->n_ghosts, board->ghosts_files);
    

    return 0;
}

void unload_level(board_t * board) {
    free(board->board);
    free(board->pacmans);
    free(board->ghosts);
}

void open_debug_file(char *filename) {
    debugfile = fopen(filename, "w");
}

void close_debug_file() {
    fclose(debugfile);
}

void debug(const char * format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(debugfile, format, args);
    va_end(args);

    fflush(debugfile);
}

void print_board(board_t *board) {
    if (!board || !board->board) {
        debug("[%d] Board is empty or not initialized.\n", getpid());
        return;
    }

    // Large buffer to accumulate the whole output
    char buffer[BIG_BUFFER_SIZE];
    size_t offset = 0;

    offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                       "=== [%d] LEVEL INFO ===\n"
                       "Dimensions: %d x %d\n"
                       "Tempo: %d\n"
                       "Pacman file: %s\n",
                       getpid(), board->height, board->width, board->tempo, board->pacman_file);

    offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                       "Monster files (%d):\n", board->n_ghosts);

    for (int i = 0; i < board->n_ghosts; i++) {
        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                           "  - %s\n", board->ghosts_files[i]);
    }

    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "\n=== BOARD ===\n");

    for (int y = 0; y < board->height; y++) {
        for (int x = 0; x < board->width; x++) {
            int idx = y * board->width + x;
            if (offset < sizeof(buffer) - 2) {
                buffer[offset++] = board->board[idx].content;
            }
        }
        if (offset < sizeof(buffer) - 2) {
            buffer[offset++] = '\n';
        }
    }

    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "==================\n");

    buffer[offset] = '\0';

    debug("%s", buffer);
}
