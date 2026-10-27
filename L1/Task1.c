
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ERR(source) (perror(source), exit(EXIT_FAILURE))

long long scan_dir(FILE *file) {
  DIR *dirp;
  struct dirent *dp;
  struct stat filestat;
  long long total = 0;

  dirp = opendir(".");
  if (dirp == NULL) {
    fprintf(file, "Error opening directory: %s\n", strerror(errno));
    return -1;
  }

  while (1) {
    errno = 0;
    dp = readdir(dirp);

    if (dp == NULL) {
      if (errno != 0) {
        fprintf(file, "Error reading directory: %s\n", strerror(errno));
        closedir(dirp);
        return -1;
      }
      break;
    }

    if (strcmp(dp->d_name, ".") == 0 || strcmp(dp->d_name, "..") == 0)
      continue;

    if (lstat(dp->d_name, &filestat) == -1) {
      fprintf(file, "lstat failed for %s: %s\n", dp->d_name, strerror(errno));
      continue;
    }

    if (S_ISREG(filestat.st_mode))
      total += (long long)filestat.st_size;
  }

  if (closedir(dirp) == -1) {
    fprintf(file, "Error closing directory: %s\n", strerror(errno));
    return -1;
  }

  return total;
}

int main(int argc, char **argv) {
  FILE *file;
  char path[PATH_MAX];

  if (argc < 2 || (argc - 1) % 2 != 0) {
    fprintf(stderr, "Usage: %s path limit [path limit ...]\n", argv[0]);
    return EXIT_FAILURE;
  }

  file = fopen("out.txt", "w");
  if (file == NULL) {
    perror("fopen");
    return EXIT_FAILURE;
  }

  if (getcwd(path, sizeof(path)) == NULL)
    ERR("getcwd");

  for (int i = 1; i < argc; i += 2) {
    char *end;
    errno = 0;

    long long limit = strtoll(argv[i + 1], &end, 10);

    if (errno != 0 || *end != '\0' || end == argv[i + 1]) {
      fprintf(file, "Invalid size limit: %s\n", argv[i + 1]);
      continue;
    }

    if (chdir(argv[i]) == -1) {
      fprintf(file, "Cannot access %s: %s\n", argv[i], strerror(errno));
      continue;
    }

    long long total = scan_dir(file);

    if (total != -1 && total > limit)
      fprintf(file, "%s\n", argv[i]);

    if (chdir(path) == -1)
      ERR("chdir");
  }

  if (fclose(file) == EOF) {
    perror("fclose");
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
