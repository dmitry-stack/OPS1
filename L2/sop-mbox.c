#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#define MAX_SMBOX_MSGS 25
#define BATCH_BYTES 64
#define MAX_FIFOS 4

#define ERR(source)                                                            \
  (perror(source), fprintf(stderr, "%s:%d\n", __FILE__, __LINE__),             \
   exit(EXIT_FAILURE))

static void usage(char *name) {
  fprintf(stderr, "USAGE: %s mailbox_file [fifo ...]\n", name);
  exit(EXIT_FAILURE);
}

void set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL);
  if (flags == -1)
    ERR("fcntl");
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
    ERR("fcntl");
}

typedef struct {
  unsigned int header_size;
  unsigned int text_size;
  char header[256];
  char text[256];
} DataBlock;

size_t parse_smbox(DataBlock blocks[MAX_SMBOX_MSGS], const char *path) {
  const int fd = open(path, O_RDONLY);
  if (fd == -1)
    ERR("open");

  struct stat sb;
  if (fstat(fd, &sb) == -1)
    ERR("fstat");

  size_t length = sb.st_size;
  if (length == 0) {
    if (close(fd) == -1)
      ERR("close");
    return 0;
  }

  uint8_t *buff = mmap(NULL, length, PROT_READ, MAP_PRIVATE, fd, 0);
  if (buff == MAP_FAILED)
    ERR("mmap");

  size_t offset = 0;
  size_t count = 0;

  while (offset < length) {
    if (count >= MAX_SMBOX_MSGS || offset + 2 > length) {
      fprintf(stderr, "Invalid smbox format\n");
      exit(EXIT_FAILURE);
    }

    unsigned int h_len = buff[offset++];
    unsigned int t_len = buff[offset++];

    if (h_len < 2 || t_len < 2 || offset + h_len + t_len > length) {
      fprintf(stderr, "Invalid smbox block size\n");
      exit(EXIT_FAILURE);
    }

    memcpy(blocks[count].header, buff + offset, h_len);
    if (blocks[count].header[h_len - 1] != '\0') {
      fprintf(stderr, "Header is not null-terminated\n");
      exit(EXIT_FAILURE);
    }
    blocks[count].header_size = h_len;
    offset += h_len;

    memcpy(blocks[count].text, buff + offset, t_len);
    if (blocks[count].text[t_len - 1] != '\0') {
      fprintf(stderr, "Text is not null-terminated\n");
      exit(EXIT_FAILURE);
    }
    blocks[count].text_size = t_len;
    offset += t_len;

    count++;
  }

  if (munmap(buff, length) == -1)
    ERR("munmap");
  if (close(fd) == -1)
    ERR("close");

  return count;
}

void print_headers(const DataBlock *blocks, size_t msg_count, size_t selected) {
  fflush(stdout);
  for (size_t i = 0; i < msg_count; i++) {
    struct iovec iov[3];
    int idx = 0;

    if (i == selected) {
      iov[idx].iov_base = "-> ";
      iov[idx].iov_len = 3;
      idx++;
    }

    iov[idx].iov_base = (void *)blocks[i].header;
    iov[idx].iov_len = blocks[i].header_size - 1;
    idx++;

    iov[idx].iov_base = "\n";
    iov[idx].iov_len = 1;
    idx++;

    if (writev(STDOUT_FILENO, iov, idx) < 0)
      ERR("writev");
  }
}

void print_message(const DataBlock *blocks, size_t msg_count, size_t selected) {
  if (msg_count == 0)
    return;

  struct iovec iov[4];

  iov[0].iov_base = (void *)blocks[selected].header;
  iov[0].iov_len = blocks[selected].header_size - 1;

  iov[1].iov_base = "\n";
  iov[1].iov_len = 1;

  iov[2].iov_base = (void *)blocks[selected].text;
  iov[2].iov_len = blocks[selected].text_size - 1;

  iov[3].iov_base = "\n";
  iov[3].iov_len = 1;

  fflush(stdout);
  if (writev(STDOUT_FILENO, iov, 4) < 0)
    ERR("writev");
}

void dump_file(const char *path) {
  printf("Dumping the file...\n");
  const int fd = open(path, O_RDONLY);
  if (fd == -1)
    ERR("open");

  struct stat sb;
  if (fstat(fd, &sb) == -1)
    ERR("fstat");

  size_t length = sb.st_size;
  if (length > 0) {
    char *buff = mmap(NULL, length, PROT_READ, MAP_PRIVATE, fd, 0);
    if (buff == MAP_FAILED)
      ERR("mmap");

    fflush(stdout);
    if (write(STDOUT_FILENO, buff, length) != (ssize_t)length)
      ERR("write");

    if (munmap(buff, length) == -1)
      ERR("munmap");
  }

  if (close(fd) == -1)
    ERR("close");
  printf("\n");
}

bool process_commands(const char *buf, ssize_t n, const char *path,
                      const DataBlock *blocks, size_t msg_count,
                      size_t *selected) {
  for (ssize_t i = 0; i < n; i++) {
    char cmd = buf[i];
    if (cmd == '\n') {
      continue;
    } else if (cmd == 'd') {
      dump_file(path);
    } else if (cmd == 'h') {
      print_headers(blocks, msg_count, *selected);
    } else if (cmd == 'm') {
      print_message(blocks, msg_count, *selected);
    } else if (cmd == 'w') {
      if (*selected > 0)
        (*selected)--;
      print_headers(blocks, msg_count, *selected);
    } else if (cmd == 's') {
      if (*selected + 1 < msg_count)
        (*selected)++;
      print_headers(blocks, msg_count, *selected);
    } else if (cmd == 'q') {
      return true;
    } else {
      printf("Invalid input\n");
    }
  }
  return false;
}

int main(int argc, char **argv) {
  if (argc < 2 || argc > 2 + MAX_FIFOS)
    usage(argv[0]);

  const char *path = argv[1];
  int fifo_n = argc - 2;
  int nfds = 1 + fifo_n;

  DataBlock blocks[MAX_SMBOX_MSGS];
  size_t msg_count = parse_smbox(blocks, path);
  size_t selected = 0;

  struct pollfd fds[1 + MAX_FIFOS];
  const char *fifo_paths[1 + MAX_FIFOS];

  set_nonblocking(STDIN_FILENO);
  fds[0].fd = STDIN_FILENO;
  fds[0].events = POLLIN;
  fds[0].revents = 0;
  fifo_paths[0] = NULL;

  for (int i = 0; i < fifo_n; i++) {
    const char *fpath = argv[2 + i];
    struct stat sb;
    if (stat(fpath, &sb) == -1 || !S_ISFIFO(sb.st_mode)) {
      fprintf(stderr, "%s is not a FIFO\n", fpath);
      for (int j = 1; j <= i; j++)
        close(fds[j].fd);
      exit(EXIT_FAILURE);
    }

    int fd = open(fpath, O_RDONLY | O_NONBLOCK);
    if (fd == -1)
      ERR("open fifo");

    fds[1 + i].fd = fd;
    fds[1 + i].events = POLLIN;
    fds[1 + i].revents = 0;
    fifo_paths[1 + i] = fpath;
  }

  char buf[BATCH_BYTES];
  bool running = true;

  while (running) {
    for (int i = 0; i < nfds; i++)
      fds[i].revents = 0;

    int ret = poll(fds, nfds, -1);
    if (ret < 0) {
      if (errno == EINTR)
        continue;
      ERR("poll");
    }

    for (int i = 0; i < nfds && running; i++) {
      if (fds[i].fd < 0)
        continue;

      bool reopen_fifo = false;

      if (fds[i].revents & POLLIN) {
        ssize_t n;
        while ((n = read(fds[i].fd, buf, BATCH_BYTES)) > 0) {
          if (process_commands(buf, n, path, blocks, msg_count, &selected)) {
            running = false;
            break;
          }
        }

        if (running) {
          if (n == 0) {
            if (i == 0) {
              fds[0].fd = -1;
            } else {
              reopen_fifo = true;
            }
          } else if (n == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
            ERR("read");
          }
        }
      }

      if (running && fds[i].fd >= 0 &&
          (reopen_fifo || (fds[i].revents & (POLLHUP | POLLERR | POLLNVAL)))) {
        if (i == 0) {
          fds[0].fd = -1;
        } else {
          if (close(fds[i].fd) == -1)
            ERR("close fifo");
          fds[i].fd = open(fifo_paths[i], O_RDONLY | O_NONBLOCK);
          if (fds[i].fd == -1)
            ERR("reopen fifo");
        }
      }
    }
  }

  for (int i = 1; i < nfds; i++) {
    if (fds[i].fd >= 0 && close(fds[i].fd) == -1)
      ERR("close fifo");
  }

  return EXIT_SUCCESS;
}