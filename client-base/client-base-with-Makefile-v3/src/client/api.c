#include "api.h"
#include "protocol.h"
#include "debug.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <errno.h>
#include <stdbool.h>

#define BUFFER_SIZE (128)

struct Session {
  int id;
  int req_pipe;
  int notif_pipe;
  char req_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
  char notif_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
};

static struct Session session = {.id = -1};

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

// sends the entire buffer, retrying as needed
int send_msg(int fd, char const *str, size_t n) {
  size_t written = 0;

  while (written < n) {
    ssize_t ret = write(fd, str + written, n - written);
    if (ret < 0) return -1;

    written += ret;
  }
  return 0;
}

int pacman_connect(char const *req_pipe_path, char const *notif_pipe_path, 
  char const *server_pipe_path) {

  strcpy(session.req_pipe_path, req_pipe_path);
  session.req_pipe_path[MAX_PIPE_PATH_LENGTH] = '\0';
  strcpy(session.notif_pipe_path, notif_pipe_path);
  session.notif_pipe_path[MAX_PIPE_PATH_LENGTH] = '\0';  
  
  // Remove any existing named pipes
  if(unlink(session.req_pipe_path) != 0 && errno != ENOENT) {
    perror("[ERR]: unlink failed");
    return 1;
  }
  if(unlink(session.notif_pipe_path) != 0 && errno != ENOENT) {
    perror("[ERR]: unlink failed");
    return 1;
  }

  // Create the named pipes
  if (mkfifo(session.req_pipe_path, 0640) != 0 && errno != EEXIST){
    perror("[ERR]: mkfifo failed");
    return 1;
  } 
  if (mkfifo(session.notif_pipe_path, 0640) != 0 && errno != EEXIST) {
    perror("[ERR]: mkfifo failed");
    return 1;
  }

  // Open the server pipe for writing
  int fd_reg_pipe = open(server_pipe_path, O_WRONLY);
  if (fd_reg_pipe == -1) {
    perror("[ERR]: open failed");
    return 1;
  }

  // build and send CONNECT message
  char msg[1 + 2 * MAX_PIPE_PATH_LENGTH];
  memset(msg, 0, sizeof(msg));
  msg[0] = OP_CODE_CONNECT;
  strncpy(msg + 1, req_pipe_path, MAX_PIPE_PATH_LENGTH - 1);
  strncpy(msg + 1 + MAX_PIPE_PATH_LENGTH, notif_pipe_path, MAX_PIPE_PATH_LENGTH - 1);

  if (send_msg(fd_reg_pipe, msg, sizeof(msg)) != 0) {
    close(fd_reg_pipe);
    return 1;
  }
  close(fd_reg_pipe);

  // Open notification pipe for reading
  session.notif_pipe = open(session.notif_pipe_path, O_RDONLY);
  if (session.notif_pipe < 0) {
    perror("[ERR]: open failed");
    return 1;
  }

  char resp[2];
  int rr = read_msg(session.notif_pipe, resp, sizeof(resp));
  if (rr <= 0) return 1;
  if (resp[0] != OP_CODE_CONNECT) return 1;
  if (resp[1] != 0) return 1;

  // Open request pipe for writing
  session.req_pipe = open(session.req_pipe_path, O_WRONLY);
  if (session.req_pipe < 0) {
    perror("[ERR]: open failed");
    return 1;
  }

  return 0;
}


void pacman_play(char command) {
  if (session.req_pipe < 0) {
    perror("[ERR]: open failed");
    exit(EXIT_FAILURE);
  }

  // Send play command
  char msg[2];
  msg[0] = OP_CODE_PLAY;
  msg[1] = command;

  // sem resposta :contentReference[oaicite:11]{index=11}
  if (send_msg(session.req_pipe, msg, sizeof(msg)) != 0) {
    // se falhar, não crashamos o cliente; ele acabará por terminar via receiver thread
    debug("pacman_play: write failed\n");
  }
}

int pacman_disconnect() {
  if (session.req_pipe >= 0) {
    char op = OP_CODE_DISCONNECT;
    (void)send_msg(session.req_pipe, &op, 1);
  }

  // Close and remove pipes
  if (session.req_pipe >= 0) 
    close(session.req_pipe);
  if (session.notif_pipe >= 0) 
    close(session.notif_pipe);

  session.id = -1;

  if (unlink(session.req_pipe_path)) {
    perror("[ERR]: unlink failed");
    return 1;
  }
  if (unlink(session.notif_pipe_path)) {
    perror("[ERR]: unlink failed");
    return 1;
  }

  return 0;
}

Board receive_board_update(void) {
  Board board = {0};

  if (session.notif_pipe < 0) {
    board.game_over = 1;
    return board;
  }

  char op;
  int width, height, tempo, victory, game_over, points;

  // Read operation code
  if (read_msg(session.notif_pipe, &op, 1) <= 0) {
    board.game_over = 1;
    return board;
  }
  if (op != OP_CODE_BOARD) {
    board.game_over = 1;
    return board;
  }

  // Read board parameters
  if (read_msg(session.notif_pipe, &width, sizeof(int)) <= 0 ||
      read_msg(session.notif_pipe, &height, sizeof(int)) <= 0 ||
      read_msg(session.notif_pipe, &tempo, sizeof(int)) <= 0 ||
      read_msg(session.notif_pipe, &victory, sizeof(int)) <= 0 ||
      read_msg(session.notif_pipe, &game_over, sizeof(int)) <= 0 ||
      read_msg(session.notif_pipe, &points, sizeof(int)) <= 0) {
    board.game_over = 1;
    return board;
  }

  // Allocate board data
  size_t data_size = (size_t)(width * height);
  char *data = (char*)malloc(data_size + 1);
  if (!data) {
    board.game_over = 1;
    return board;
  }

  // Read board data
  if (read_msg(session.notif_pipe, data, data_size) <= 0) {
    board.game_over = 1;
    free(data);
    return board;
  }

  data[data_size] = '\0';

  // Fill board structure
  board.width = width;
  board.height = height;
  board.tempo = tempo;
  board.victory = victory;
  board.game_over = game_over;
  board.accumulated_points = points;
  board.data = data;

  return board;
}