
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define ERR(source)                                                            \
  (perror(source), fprintf(stderr, "%s:%d\n", __FILE__, __LINE__),             \
   exit(EXIT_FAILURE))

int has_extension(const char *filename, const char *extension) {
  const char *dot = strrchr(filename, '.');

  if (dot == NULL || dot == filename)
    return 0;

  return strcmp(dot + 1, extension) == 0;
}

void scan_dir(char *path, char *extension, int depth, FILE *out) {
  DIR *dirp;
  struct dirent *dp;
  struct stat filestat;
  char filepath[4096];

  dirp = opendir(path);
  if (dirp == NULL)
    ERR("opendir");

  fprintf(out, "path: %s\n", path);

  do {
    errno = 0;
    dp = readdir(dirp);

    if (dp != NULL) {
      if (strcmp(dp->d_name, ".") == 0 || strcmp(dp->d_name, "..") == 0)
        continue;

      int n = snprintf(filepath, sizeof(filepath), "%s/%s", path, dp->d_name);

      if (n < 0 || (size_t)n >= sizeof(filepath)) {
        fprintf(stderr, "Path too long\n");
        exit(EXIT_FAILURE);
      }

      if (lstat(filepath, &filestat) == -1)
        ERR("lstat");

      if (S_ISLNK(filestat.st_mode))
        continue;

      if (S_ISDIR(filestat.st_mode)) {
        if (depth > 1)
          scan_dir(filepath, extension, depth - 1, out);

        continue;
      }

      if (!S_ISREG(filestat.st_mode))
        continue;

      if (extension == NULL || has_extension(dp->d_name, extension)) {
        fprintf(out, "%s %jd\n", dp->d_name, (intmax_t)filestat.st_size);
      }
    }
  } while (dp != NULL);

  if (errno != 0)
    ERR("readdir");

  if (closedir(dirp) == -1)
    ERR("closedir");
}

int main(int argc, char **argv) {
  char *extension = NULL;
  int depth = 1;
  int use_output_file = 0;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-d") == 0) {
      if (i + 1 >= argc)
        ERR("missing depth");

      depth = atoi(argv[++i]);

      if (depth < 1) {
        fprintf(stderr, "Depth must be positive\n");
        return EXIT_FAILURE;
      }
    } else if (strcmp(argv[i], "-e") == 0) {
      if (i + 1 >= argc)
        ERR("missing extension");

      extension = argv[++i];
    } else if (strcmp(argv[i], "-o") == 0) {
      use_output_file = 1;
    }
  }

  FILE *out = stdout;

  if (use_output_file) {
    char *output_path = getenv("L1_OUTPUTFILE");

    if (output_path != NULL && output_path[0] != '\0') {
      out = fopen(output_path, "w");

      if (out == NULL)
        ERR("fopen");
    }
  }

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-p") == 0) {
      if (i + 1 >= argc)
        ERR("missing path");

      scan_dir(argv[++i], extension, depth, out);
    }
  }

  if (out != stdout) {
    if (fclose(out) == EOF)
      ERR("fclose");
  }

  return EXIT_SUCCESS;
}
