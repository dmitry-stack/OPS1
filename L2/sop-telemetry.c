#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ERR(source)                                                            \
  (perror(source), fprintf(stderr, "%s:%d\n", __FILE__, __LINE__),             \
   exit(EXIT_FAILURE))

#define LINE_LENGTH 64
#define NUM_SLOTS 16
#define LOG_SIZE (NUM_SLOTS * LINE_LENGTH) // 1024 байта
#define MAX_SENSORS 4
#define POLL_TIMEOUT_MS 2500

typedef struct {
  int fd;
  const char *path;
  char buf[256];
  size_t buf_len;
} Sensor;

void usage(const char *const pname) {
  fprintf(stderr, "USAGE: %s <log_file> <sensor1> [sensor2 ... sensor4]\n",
          pname);
  exit(EXIT_FAILURE);
}

void process_packet(int log_fd, const char *line) {
  // 1. Ищем разделитель ':'
  const char *colon = strchr(line, ':');
  if (colon == NULL || colon == line || *(colon + 1) == '\0') {
    fprintf(stderr, "Corrupted packet: %s\n", line);
    return;
  }

  // 2. Проверяем, что до ':' идут строго цифры
  for (const char *p = line; p < colon; p++) {
    if (!isdigit((unsigned char)*p)) {
      fprintf(stderr, "Corrupted packet: %s\n", line);
      return;
    }
  }

  // 3. Парсим ID и проверяем диапазон 0..15
  char *endptr;
  long id = strtol(line, &endptr, 10);
  if (endptr != colon || id < 0 || id >= NUM_SLOTS) {
    fprintf(stderr, "Corrupted packet: %s\n", line);
    return;
  }

  // 4. Формируем строку на 64 байта (63 символа с пробелами + '\n')
  const char *payload = colon + 1;
  char formatted[LINE_LENGTH];
  snprintf(formatted, sizeof(formatted), "%-63.63s", payload);
  formatted[LINE_LENGTH - 1] = '\n';

  // 5. Записываем в нужную строку файла log.bin
  if (lseek(log_fd, id * LINE_LENGTH, SEEK_SET) == -1)
    ERR("lseek");

  if (write(log_fd, formatted, LINE_LENGTH) != LINE_LENGTH)
    ERR("write");

  printf("Sensor %02ld updated\n", id);
}

int main(int argc, char **argv) {
  if (argc < 3 || argc > 2 + MAX_SENSORS)
    usage(argv[0]);

  // --- ЭТАП 1: Инициализация или проверка файла журнала ---
  int log_fd = open(argv[1], O_RDWR | O_CREAT | O_EXCL, 0644);
  if (log_fd == -1 && errno != EEXIST) {
    ERR("open");
  } else if (log_fd == -1 && errno == EEXIST) {
    log_fd = open(argv[1], O_RDWR);
    if (log_fd == -1)
      ERR("open existing");

    struct stat s;
    if (fstat(log_fd, &s) < 0)
      ERR("fstat");

    if (s.st_size != LOG_SIZE) {
      fprintf(stderr, "Invalid log file size!\n");
      close(log_fd);
      exit(EXIT_FAILURE);
    }
  } else {
    char line[LINE_LENGTH];
    memset(line, ' ', LINE_LENGTH - 1);
    line[LINE_LENGTH - 1] = '\n';

    for (int i = 0; i < NUM_SLOTS; i++) {
      if (write(log_fd, line, LINE_LENGTH) != LINE_LENGTH)
        ERR("write");
    }
  }

  // --- ЭТАП 2: Проверка и открытие каналов датчиков ---
  int sensor_count = argc - 2;
  Sensor sensors[MAX_SENSORS];

  for (int i = 0; i < sensor_count; i++) {
    const char *path = argv[i + 2];
    struct stat s;

    if (stat(path, &s) == -1 || !S_ISFIFO(s.st_mode)) {
      fprintf(stderr, "Sensor %s is offline!\n", path);
      for (int j = 0; j < i; j++)
        close(sensors[j].fd);
      close(log_fd);
      exit(EXIT_FAILURE);
    }

    int s_fd = open(path, O_RDONLY | O_NONBLOCK);
    if (s_fd == -1) {
      fprintf(stderr, "Sensor %s is offline!\n", path);
      for (int j = 0; j < i; j++)
        close(sensors[j].fd);
      close(log_fd);
      exit(EXIT_FAILURE);
    }

    sensors[i].fd = s_fd;
    sensors[i].path = path;
    sensors[i].buf_len = 0;
  }

  // --- ЭТАП 3: Сбор телеметрии через poll ---
  struct pollfd fds[MAX_SENSORS];
  for (int i = 0; i < sensor_count; i++) {
    fds[i].fd = sensors[i].fd;
    fds[i].events = POLLIN;
    fds[i].revents = 0;
  }

  int active_sensors = sensor_count;
  while (active_sensors > 0) {
    for (int i = 0; i < sensor_count; i++)
      fds[i].revents = 0;

    int ret = poll(fds, sensor_count, POLL_TIMEOUT_MS);
    if (ret < 0) {
      if (errno == EINTR)
        continue;
      ERR("poll");
    }

    if (ret == 0) {
      printf("Waiting for telemetry...\n");
      continue;
    }

    for (int i = 0; i < sensor_count; i++) {
      if (fds[i].fd < 0)
        continue;

      if (fds[i].revents & POLLIN) {
        char ch;
        ssize_t n;
        while ((n = read(fds[i].fd, &ch, 1)) > 0) {
          if (ch == '\n') {
            sensors[i].buf[sensors[i].buf_len] = '\0';
            process_packet(log_fd, sensors[i].buf);
            sensors[i].buf_len = 0;
          } else if (sensors[i].buf_len < sizeof(sensors[i].buf) - 1) {
            sensors[i].buf[sensors[i].buf_len++] = ch;
          }
        }

        if (n == 0) {
          printf("Sensor %s disconnected\n", sensors[i].path);
          close(fds[i].fd);
          fds[i].fd = -1;
          active_sensors--;
          continue;
        } else if (n == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
          ERR("read");
        }
      }

      if (fds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) {
        printf("Sensor %s disconnected\n", sensors[i].path);
        close(fds[i].fd);
        fds[i].fd = -1;
        active_sensors--;
      }
    }
  }

  char *buff =
      mmap(NULL, LOG_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, log_fd, 0);
  if (buff == MAP_FAILED)
    ERR("mmap");

  int patched = 0;
  for (int i = 0; i <= LOG_SIZE - 3; i++) {
    if (memcmp(buff + i, "ERR", 3) == 0) {
      memcpy(buff + i, "OK!", 3);
      patched++;
      i += 2;
    }
  }
  printf("Errors patched: %d\n", patched);

  int empty_count = 0;
  for (int i = 0; i < NUM_SLOTS; i++) {
    char *row = buff + i * LINE_LENGTH;
    int len = LINE_LENGTH - 1;

    while (len > 0 && row[len - 1] == ' ')
      len--;

    if (len == 0) {
      empty_count++;
    } else {
      printf("%02d: %.*s\n", i, len, row);
    }
  }

  if (empty_count == 0) {
    printf("Telemetry complete!\n");
  } else {
    printf("Empty slots: %d\n", empty_count);
  }

  if (msync(buff, LOG_SIZE, MS_SYNC) == -1)
    ERR("msync");

  if (munmap(buff, LOG_SIZE) == -1)
    ERR("munmap");

  if (close(log_fd) == -1)
    ERR("close log_fd");

  return EXIT_SUCCESS;
}