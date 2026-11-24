#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define ERR(source)                                                            \
  (perror(source), fprintf(stderr, "%s:%d\n", __FILE__, __LINE__),             \
   exit(EXIT_FAILURE))

#define LINE_LENGTH 64
#define NUM_STEPS 32
#define RECIPE_SIZE (NUM_STEPS * LINE_LENGTH)
#define MAX_TOWERS 5
#define MAX_MSG_LEN 128
#define POLL_TIMEOUT_MS 3000

typedef struct {
  int fd;
  const char *path;
  char buf[MAX_MSG_LEN + 1];
  size_t buf_len;
  int corrupted;
} Tower;

static void usage(const char *pname) {
  fprintf(stderr, "USAGE: %s <recipe> <tower1> [tower2 ... tower5]\n", pname);
  exit(EXIT_FAILURE);
}

static void process_message(int recipe_fd, const char *msg) {
  const char *colon = strchr(msg, ':');
  if (colon == NULL || colon == msg || *(colon + 1) == '\0') {
    fprintf(stderr, "Granny got carried away: %s\n", msg);
    return;
  }

  for (const char *p = msg; p < colon; p++) {
    if (!isdigit((unsigned char)*p)) {
      fprintf(stderr, "Granny got carried away: %s\n", msg);
      return;
    }
  }

  char *endptr;
  long step = strtol(msg, &endptr, 10);
  if (endptr != colon || step < 0 || step >= NUM_STEPS) {
    fprintf(stderr, "Granny got carried away: %s\n", msg);
    return;
  }

  const char *content = colon + 1;
  char line[LINE_LENGTH];
  memset(line, ' ', LINE_LENGTH - 1);
  line[LINE_LENGTH - 1] = '\n';

  size_t content_len = strlen(content);
  if (content_len > LINE_LENGTH - 1)
    content_len = LINE_LENGTH - 1;
  memcpy(line, content, content_len);

  if (lseek(recipe_fd, step * LINE_LENGTH, SEEK_SET) == -1)
    ERR("lseek");

  if (write(recipe_fd, line, LINE_LENGTH) != LINE_LENGTH)
    ERR("write");

  printf("Step %02ld saved\n", step);
}

int main(int argc, char **argv) {
  if (argc < 3 || argc > 2 + MAX_TOWERS)
    usage(argv[0]);

  const char *recipe_path = argv[1];
  int tower_count = argc - 2;

  // --- STAGE 1: Open or create the recipe file (2048 bytes) ---
  int recipe_fd = open(recipe_path, O_RDWR | O_CREAT | O_EXCL, 0644);
  if (recipe_fd == -1) {
    if (errno != EEXIST)
      ERR("open");

    recipe_fd = open(recipe_path, O_RDWR);
    if (recipe_fd == -1)
      ERR("open existing");

    off_t size = lseek(recipe_fd, 0, SEEK_END);
    if (size == -1)
      ERR("lseek");

    if (size != RECIPE_SIZE) {
      fprintf(stderr, "This book is not a recipe!\n");
      close(recipe_fd);
      exit(EXIT_FAILURE);
    }
  } else {
    char empty_line[LINE_LENGTH];
    memset(empty_line, ' ', LINE_LENGTH - 1);
    empty_line[LINE_LENGTH - 1] = '\n';

    for (int i = 0; i < NUM_STEPS; i++) {
      if (write(recipe_fd, empty_line, LINE_LENGTH) != LINE_LENGTH)
        ERR("write");
    }
  }

  // --- STAGE 2: Validate and open named pipes (FIFOs) ---
  Tower towers[MAX_TOWERS];
  struct pollfd fds[MAX_TOWERS];

  for (int i = 0; i < tower_count; i++) {
    const char *path = argv[i + 2];
    struct stat sb;

    if (stat(path, &sb) == -1 || !S_ISFIFO(sb.st_mode)) {
      fprintf(stderr, "There is no granny in %s\n", path);
      for (int j = 0; j < i; j++)
        close(towers[j].fd);
      close(recipe_fd);
      exit(EXIT_FAILURE);
    }

    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd == -1) {
      fprintf(stderr, "There is no granny in %s\n", path);
      for (int j = 0; j < i; j++)
        close(towers[j].fd);
      close(recipe_fd);
      exit(EXIT_FAILURE);
    }

    towers[i].fd = fd;
    towers[i].path = path;
    towers[i].buf_len = 0;
    towers[i].corrupted = 0;

    fds[i].fd = fd;
    fds[i].events = POLLIN;
    fds[i].revents = 0;
  }

  // --- STAGE 3: Collect steps from all towers using poll ---
  int active_towers = tower_count;

  while (active_towers > 0) {
    for (int i = 0; i < tower_count; i++)
      fds[i].revents = 0;

    int ret = poll(fds, tower_count, POLL_TIMEOUT_MS);
    if (ret < 0) {
      if (errno == EINTR)
        continue;
      ERR("poll");
    }

    if (ret == 0) {
      printf("Grannies are pouring more tea...\n");
      continue;
    }

    for (int i = 0; i < tower_count; i++) {
      if (fds[i].fd < 0)
        continue;

      if (fds[i].revents & POLLIN) {
        char ch;
        ssize_t n;
        while ((n = read(fds[i].fd, &ch, 1)) > 0) {
          if (ch == '\n') {
            towers[i].buf[towers[i].buf_len] = '\0';
            if (towers[i].corrupted) {
              fprintf(stderr, "Granny got carried away: %s\n", towers[i].buf);
            } else {
              process_message(recipe_fd, towers[i].buf);
            }
            towers[i].buf_len = 0;
            towers[i].corrupted = 0;
          } else {
            if (towers[i].buf_len < MAX_MSG_LEN) {
              towers[i].buf[towers[i].buf_len++] = ch;
            } else {
              towers[i].corrupted = 1;
            }
          }
        }

        if (n == 0) {
          printf("Granny from %s finished dictating\n", towers[i].path);
          close(fds[i].fd);
          fds[i].fd = -1;
          active_towers--;
          continue;
        } else if (n == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
          ERR("read");
        }
      }

      if (fds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) {
        printf("Granny from %s finished dictating\n", towers[i].path);
        close(fds[i].fd);
        fds[i].fd = -1;
        active_towers--;
      }
    }
  }

  // --- STAGE 4: Clean stains via mmap and print the final recipe ---
  char *map =
      mmap(NULL, RECIPE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, recipe_fd, 0);
  if (map == MAP_FAILED)
    ERR("mmap");

  const char *bad_word = "raw~berries";
  const char *good_word = "raspberries";
  const size_t word_len = 11;

  int stains_cleaned = 0;
  for (size_t i = 0; i <= RECIPE_SIZE - word_len; i++) {
    if (memcmp(map + i, bad_word, word_len) == 0) {
      memcpy(map + i, good_word, word_len);
      stains_cleaned++;
      i += word_len - 1;
    }
  }

  printf("Princess Joanna's stains cleaned: %d\n", stains_cleaned);

  int missing_steps = 0;
  for (int i = 0; i < NUM_STEPS; i++) {
    char *row = map + i * LINE_LENGTH;
    int len = LINE_LENGTH - 1;

    while (len > 0 && row[len - 1] == ' ')
      len--;

    if (len == 0) {
      missing_steps++;
    } else {
      printf("%02d: %.*s\n", i, len, row);
    }
  }

  if (missing_steps == 0) {
    printf("Cake ready!\n");
  } else {
    printf("Missing %d steps, the cake will collapse!\n", missing_steps);
  }

  if (msync(map, RECIPE_SIZE, MS_SYNC) == -1)
    ERR("msync");
  if (munmap(map, RECIPE_SIZE) == -1)
    ERR("munmap");
  if (close(recipe_fd) == -1)
    ERR("close");

  return EXIT_SUCCESS;
}