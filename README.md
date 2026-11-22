# Pacmanist — Concurrent Client–Server Game

Pacmanist is a Pac-Man-inspired terminal game developed in C for the 2025/26 Operating Systems course at Instituto Superior Técnico. Multiple players can connect to the same server and play independent games, each with its own board and score.

## Features

- **Concurrent game sessions.** The server uses a configurable pool of POSIX worker threads. Each worker runs one game session at a time and takes the next queued request when it becomes available.
- **Client admission.** Connection requests enter a bounded FIFO queue. Two counting semaphores track pending requests and free slots, while a mutex protects access to the queue.
- **Independent ghost movement.** Each ghost runs its movement logic in a dedicated POSIX thread. Separate threads within each session handle player commands and send board updates.
- **Interprocess communication.** Clients register through a shared named pipe. Each session then uses two dedicated named pipes, one for player commands and one for board updates. The client renders the board in the terminal using ncurses.
- **Score reporting.** When the server processes a `SIGUSR1` request, it writes the scores of up to five active clients to `top5_clients.txt`, ordered from highest to lowest.

## How to Run

The project requires Linux, GCC, GNU Make, and the ncurses development headers (`libncurses-dev` on Debian or Ubuntu). Run the following commands from the repository root.

### Build the server and client

```bash
make -C sol-part1/SO-2526-sol-parte1 clean
make -C sol-part1/SO-2526-sol-parte1
make -C client-base/client-base-with-Makefile-v3 clean
make -C client-base/client-base-with-Makefile-v3
```

### Start the server

The included sample level is in the levels/ directory. The `2` sets the maximum number of simultaneous game sessions.

```bash
./sol-part1/SO-2526-sol-parte1/bin/Pacmanist ./levels 2 /tmp/pacmanist-register
```

### Connect clients

Run each command in a separate terminal. Give each client a unique numeric ID and use the same registration pipe.

```bash
./client-base/client-base-with-Makefile-v3/bin/client 1 /tmp/pacmanist-register
```

```bash
./client-base/client-base-with-Makefile-v3/bin/client 2 /tmp/pacmanist-register
```

Use **W**, **A**, **S**, and **D** to move and **Q** to quit. To play using a command file, pass its path as the optional third argument.

## Attribution

Built on the Pacmanist starter code provided for the Operating Systems course at Instituto Superior Técnico.