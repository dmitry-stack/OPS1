#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "Использование: %s <файл>\n", argv[0]);
    return EXIT_FAILURE;
  }

  int fd = open(argv[1], O_RDWR);
  if (fd == -1) {
    perror("open");
    return EXIT_FAILURE;
  }

  // 1. Узнаем размер файла
  off_t file_size = lseek(fd, 0, SEEK_END);
  if (file_size == -1) {
    perror("lseek SEEK_END");
    close(fd);
    return EXIT_FAILURE;
  }
  printf("Размер: %ld байт\n", (long)file_size);

  if (file_size < 5) {
    fprintf(stderr, "Файл слишком мал для теста (меньше 5 байт)\n");
    close(fd);
    return EXIT_FAILURE;
  }

  // 2. Первые 5 байт (возвращаем каретку в начало)
  if (lseek(fd, 0, SEEK_SET) == -1) {
    perror("lseek SEEK_SET");
    close(fd);
    return EXIT_FAILURE;
  }

  char first_buf[6];
  ssize_t bytes_read = read(fd, first_buf, 5);
  if (bytes_read == -1) {
    perror("read first");
    close(fd);
    return EXIT_FAILURE;
  }
  first_buf[bytes_read] = '\0'; // Обязательно закрываем строку
  printf("Начало: %s\n", first_buf);

  // 3. Последние 5 байт (отступаем на -5 от конца)
  if (lseek(fd, -5, SEEK_END) == -1) {
    perror("lseek -5 SEEK_END");
    close(fd);
    return EXIT_FAILURE;
  }

  char last_buf[6];
  bytes_read = read(fd, last_buf, 5);
  if (bytes_read == -1) {
    perror("read last");
    close(fd);
    return EXIT_FAILURE;
  }
  last_buf[bytes_read] = '\0';
  printf("Конец: %s\n", last_buf);

  // 4. Перезаписываем первый символ
  if (lseek(fd, 0, SEEK_SET) == -1) {
    perror("lseek rewrite");
    close(fd);
    return EXIT_FAILURE;
  }

  char ex = '!';
  if (write(fd, &ex, 1) == -1) {
    perror("write");
    close(fd);
    return EXIT_FAILURE;
  }

  close(fd);
  return EXIT_SUCCESS;
}

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "Использование: %s <файл>\n", argv[0]);
    return EXIT_FAILURE;
  }

  int fd = open(argv[1], O_RDWR);
  if (fd == -1) {
    perror("open");
    return EXIT_FAILURE;
  }

  // 1. Узнаем размер через fstat:
  struct stat sb;
  fstat(fd, &sb);
  size_t length = sb.st_size;

  // 2. Отображаем в память:
  char *data = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (data == MAP_FAILED) {
    perror("mmap");
    close(fd);
    return EXIT_FAILURE;
  }

  // 3. data теперь ведет себя как обычный массив:
  // data[i] = '?'

  for (int i = 0; i < length; i++) {
    if (data[i] == '!') {
      data[i] = '?';
    }
  }

  // 4. Синхронизация и очистка:
  msync(data, length, MS_SYNC);
  munmap(data, length);
  close(fd);
}

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "Использование: %s <файл>\n", argv[0]);
    return EXIT_FAILURE;
  }
  int fd = open(argv[1], O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd == -1) {
    perror("open");
    return EXIT_FAILURE;
  }

  struct iovec iov[3];
  iov[0].iov_base = "[LOG] ";
  iov[0].iov_len = strlen("[LOG] ");

  iov[1].iov_base = argv[2];
  iov[1].iov_len = strlen(argv[2]);

  iov[2].iov_base = "\n";
  iov[2].iov_len = 1;

  ssize_t written = writev(fd, iov, 3);
  if (written == -1) {
    perror("writev");
    return EXIT_FAILURE;
  }
  close(fd);
}

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FIFO_NAME "chat_fifo"
#define BUF_SIZE 256

int main(void) {
  // 1. Создаем именованный канал
  if (mkfifo(FIFO_NAME, 0666) == -1 && errno != EEXIST) {
    perror("mkfifo");
    return EXIT_FAILURE;
  }

  // 2. Открываем FIFO в режиме O_RDWR | O_NONBLOCK
  // O_RDWR держит канал открытым, даже когда внешний писатель закрывает свою
  // сторону
  int fifo_fd = open(FIFO_NAME, O_RDWR | O_NONBLOCK);
  if (fifo_fd == -1) {
    perror("open fifo");
    unlink(FIFO_NAME);
    return EXIT_FAILURE;
  }

  // 3. Конфигурируем дескрипторы для poll
  struct pollfd fds[2];

  // fds[0]: ввод с клавиатуры
  fds[0].fd = STDIN_FILENO;
  fds[0].events = POLLIN;

  // fds[1]: наш именованный канал
  fds[1].fd = fifo_fd;
  fds[1].events = POLLIN;

  printf("Часовой запущен. Введите 'q' для выхода.\n");
  printf("Для отправки сообщения откройте другой терминал:\n  echo \"Привет\" "
         "> %s\n\n",
         FIFO_NAME);

  int running = 1;
  while (running) {
    fds[0].revents = 0;
    fds[1].revents = 0;

    // Ждем события максимум 2000 мс (2 секунды)
    int ret = poll(fds, 2, 2000);

    if (ret == -1) {
      if (errno == EINTR) // Прерывание системным сигналом
        continue;
      perror("poll");
      break;
    }

    // 4.1. Сработал таймаут (ret == 0)
    if (ret == 0) {
      printf("Тишина уже 2 секунды...\n");
      continue;
    }

    // 4.2. Сработал STDIN (клавиатура)
    if (fds[0].revents & POLLIN) {
      char input_buf[BUF_SIZE];
      ssize_t n = read(STDIN_FILENO, input_buf, sizeof(input_buf) - 1);
      if (n > 0) {
        input_buf[n] = '\0';
        if (input_buf[0] == 'q') {
          printf("Выход по команде 'q'...\n");
          running = 0;
        }
      }
    }

    // 4.3. Сработал FIFO
    if (fds[1].revents & POLLIN) {
      char fifo_buf[BUF_SIZE];
      ssize_t bytes_read;

      // Вычитываем все доступные байты до тех пор, пока буфер ядра не опустеет
      while ((bytes_read = read(fifo_fd, fifo_buf, sizeof(fifo_buf) - 1)) > 0) {
        fifo_buf[bytes_read] = '\0';
        printf("[FIFO]: %s", fifo_buf);
        if (fifo_buf[bytes_read - 1] != '\n')
          printf("\n");
      }

      // В неблокирующем режиме read возвращает -1 с EAGAIN/EWOULDBLOCK, когда
      // данных больше нет
      if (bytes_read == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
        perror("read fifo");
      }
    }
  }

  // 5. Очистка ресурсов
  close(fifo_fd);
  unlink(FIFO_NAME);

  printf("Канал удален, программа завершена.\n");
  return EXIT_SUCCESS;
}