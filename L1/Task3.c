#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define PROGRAM "sop-venv"

enum Operation { OP_NONE, OP_CREATE, OP_INSTALL, OP_REMOVE };

static void usage(const char *program) {
  fprintf(stderr,
          "usage:\n"
          "  %s -c ENVIRONMENT\n"
          "  %s -c -v ENVIRONMENT\n"
          "  %s -v ENVIRONMENT [-v ENVIRONMENT ...] "
          "-i PACKAGE==VERSION\n"
          "  %s -v ENVIRONMENT [-v ENVIRONMENT ...] "
          "-r PACKAGE\n",
          program, program, program, program);
}

static void error_message(const char *message) {
  fprintf(stderr, "%s: %s\n", PROGRAM, message);
}

static void error_errno(const char *message) {
  int saved_errno = errno;
  fprintf(stderr, "%s: %s: %s\n", PROGRAM, message, strerror(saved_errno));
}

static char *join_paths(const char *a, const char *b) {
  size_t len = strlen(a) + strlen(b) + 2;
  char *result = malloc(len);

  if (result == NULL) {
    error_errno("malloc");
    return NULL;
  }

  snprintf(result, len, "%s/%s", a, b);
  return result;
}

static int valid_component(const char *name) {
  if (name == NULL || name[0] == '\0')
    return 0;

  if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
    return 0;

  for (const unsigned char *p = (const unsigned char *)name; *p != '\0'; p++) {
    if (*p == '/' || isspace(*p))
      return 0;
  }

  return 1;
}

static int create_environment(const char *name) {
  if (!valid_component(name)) {
    error_message("invalid environment name");
    return -1;
  }

  if (mkdir(name, 0755) == -1) {
    error_errno("cannot create environment");
    return -1;
  }

  char *requirements = join_paths(name, "requirements");
  if (requirements == NULL) {
    rmdir(name);
    return -1;
  }

  int fd = open(requirements, O_WRONLY | O_CREAT | O_EXCL, 0644);

  if (fd == -1) {
    error_errno("cannot create requirements");
    free(requirements);
    rmdir(name);
    return -1;
  }

  if (close(fd) == -1) {
    error_errno("cannot close requirements");
    free(requirements);
    unlink(requirements);
    rmdir(name);
    return -1;
  }

  free(requirements);
  return 0;
}

/* Return 1 if the package name exists, 0 if it does not, -1 on error. */
static int package_exists(const char *requirements, const char *package) {
  FILE *file = fopen(requirements, "r");

  if (file == NULL) {
    error_errno("cannot open requirements");
    return -1;
  }

  char *line = NULL;
  size_t capacity = 0;
  ssize_t length;
  int found = 0;

  while ((length = getline(&line, &capacity, file)) != -1) {
    (void)length;

    char *end = line;

    while (*end != '\0' && !isspace((unsigned char)*end))
      end++;

    if ((size_t)(end - line) == strlen(package) &&
        strncmp(line, package, strlen(package)) == 0) {
      found = 1;
      break;
    }
  }

  if (ferror(file)) {
    error_errno("cannot read requirements");
    free(line);
    fclose(file);
    return -1;
  }

  free(line);

  if (fclose(file) == EOF) {
    error_errno("cannot close requirements");
    return -1;
  }

  return found;
}

static int check_environment(const char *environment, char **requirements_out) {
  struct stat st;

  if (stat(environment, &st) == -1 || !S_ISDIR(st.st_mode)) {
    error_message("the environment does not exist");
    return -1;
  }

  char *requirements = join_paths(environment, "requirements");
  if (requirements == NULL)
    return -1;

  if (stat(requirements, &st) == -1 || !S_ISREG(st.st_mode)) {
    error_message("requirements file does not exist");
    free(requirements);
    return -1;
  }

  *requirements_out = requirements;
  return 0;
}

static int write_all(int fd, const void *buffer, size_t size) {
  const char *p = buffer;
  size_t written = 0;

  while (written < size) {
    ssize_t n = write(fd, p + written, size - written);

    if (n == -1) {
      if (errno == EINTR)
        continue;
      return -1;
    }

    if (n == 0) {
      errno = EIO;
      return -1;
    }

    written += (size_t)n;
  }

  return 0;
}

static int install_package(const char *environment, const char *package,
                           const char *version) {
  if (!valid_component(package) || strchr(package, '=') != NULL ||
      version == NULL || version[0] == '\0' ||
      strpbrk(version, " \t\r\n") != NULL) {
    error_message("invalid package name or version");
    return -1;
  }

  char *requirements = NULL;
  if (check_environment(environment, &requirements) == -1)
    return -1;

  int exists = package_exists(requirements, package);

  if (exists == -1) {
    free(requirements);
    return -1;
  }

  if (exists) {
    error_message("package is already installed");
    free(requirements);
    return -1;
  }

  char *package_path = join_paths(environment, package);
  if (package_path == NULL) {
    free(requirements);
    return -1;
  }

  /* Create the package file exclusively. */
  int fd = open(package_path, O_WRONLY | O_CREAT | O_EXCL, 0600);

  if (fd == -1) {
    error_errno("cannot create package file");
    free(package_path);
    free(requirements);
    return -1;
  }

  /* Fill the file with random bytes from /dev/urandom. */
  unsigned char content[32];
  int random_fd = open("/dev/urandom", O_RDONLY);

  if (random_fd == -1) {
    error_errno("cannot open /dev/urandom");
    close(fd);
    unlink(package_path);
    free(package_path);
    free(requirements);
    return -1;
  }

  size_t received = 0;

  while (received < sizeof(content)) {
    ssize_t n = read(random_fd, content + received, sizeof(content) - received);

    if (n == -1) {
      if (errno == EINTR)
        continue;

      error_errno("cannot read random data");
      close(random_fd);
      close(fd);
      unlink(package_path);
      free(package_path);
      free(requirements);
      return -1;
    }

    if (n == 0) {
      errno = EIO;
      error_errno("cannot read random data");
      close(random_fd);
      close(fd);
      unlink(package_path);
      free(package_path);
      free(requirements);
      return -1;
    }

    received += (size_t)n;
  }

  if (close(random_fd) == -1) {
    error_errno("cannot close /dev/urandom");
    close(fd);
    unlink(package_path);
    free(package_path);
    free(requirements);
    return -1;
  }

  if (write_all(fd, content, sizeof(content)) == -1) {
    error_errno("cannot write package file");
    close(fd);
    unlink(package_path);
    free(package_path);
    free(requirements);
    return -1;
  }

  /* All user classes get read-only permission. */
  if (fchmod(fd, 0444) == -1) {
    error_errno("cannot set package permissions");
    close(fd);
    unlink(package_path);
    free(package_path);
    free(requirements);
    return -1;
  }

  if (close(fd) == -1) {
    error_errno("cannot close package file");
    unlink(package_path);
    free(package_path);
    free(requirements);
    return -1;
  }

  FILE *file = fopen(requirements, "a");

  if (file == NULL) {
    error_errno("cannot open requirements for appending");
    unlink(package_path);
    free(package_path);
    free(requirements);
    return -1;
  }

  int failed = 0;

  if (fprintf(file, "%s %s\n", package, version) < 0) {
    error_errno("cannot update requirements");
    failed = 1;
  }

  if (fclose(file) == EOF) {
    error_errno("cannot close requirements");
    failed = 1;
  }

  if (failed)
    unlink(package_path);

  free(package_path);
  free(requirements);

  return failed ? -1 : 0;
}

static int uninstall_package(const char *environment, const char *package) {
  if (!valid_component(package) || strchr(package, '=') != NULL) {
    error_message("invalid package name");
    return -1;
  }

  char *requirements = NULL;
  if (check_environment(environment, &requirements) == -1)
    return -1;

  int exists = package_exists(requirements, package);

  if (exists == -1) {
    free(requirements);
    return -1;
  }

  if (!exists) {
    error_message("package is not installed");
    free(requirements);
    return -1;
  }

  char *package_path = join_paths(environment, package);
  if (package_path == NULL) {
    free(requirements);
    return -1;
  }

  /*
   * Build a temporary requirements file without the package line.
   */
  char temp_name[64];
  snprintf(temp_name, sizeof(temp_name), ".requirements.tmp.%ld",
           (long)getpid());

  char *temp_path = join_paths(environment, temp_name);

  if (temp_path == NULL) {
    free(package_path);
    free(requirements);
    return -1;
  }

  int temp_fd = open(temp_path, O_WRONLY | O_CREAT | O_EXCL, 0600);

  if (temp_fd == -1) {
    error_errno("cannot create temporary requirements");
    free(temp_path);
    free(package_path);
    free(requirements);
    return -1;
  }

  FILE *input = fopen(requirements, "r");
  FILE *output = fdopen(temp_fd, "w");

  if (input == NULL || output == NULL) {
    error_errno("cannot open requirements files");

    if (input != NULL)
      fclose(input);

    if (output != NULL)
      fclose(output);
    else
      close(temp_fd);

    unlink(temp_path);
    free(temp_path);
    free(package_path);
    free(requirements);
    return -1;
  }

  char *line = NULL;
  size_t capacity = 0;
  ssize_t length;
  int failed = 0;

  while ((length = getline(&line, &capacity, input)) != -1) {
    char *end = line;

    while (*end != '\0' && !isspace((unsigned char)*end))
      end++;

    int match = (size_t)(end - line) == strlen(package) &&
                strncmp(line, package, strlen(package)) == 0;

    if (!match && fwrite(line, 1, (size_t)length, output) != (size_t)length) {
      error_errno("cannot write temporary requirements");
      failed = 1;
      break;
    }
  }

  if (ferror(input)) {
    error_errno("cannot read requirements");
    failed = 1;
  }

  free(line);

  if (fclose(input) == EOF) {
    error_errno("cannot close requirements");
    failed = 1;
  }

  if (fclose(output) == EOF) {
    error_errno("cannot close temporary requirements");
    failed = 1;
  }

  if (failed) {
    unlink(temp_path);
    free(temp_path);
    free(package_path);
    free(requirements);
    return -1;
  }

  struct stat st;

  if (lstat(package_path, &st) == -1) {
    error_errno("package file does not exist");
    unlink(temp_path);
    free(temp_path);
    free(package_path);
    free(requirements);
    return -1;
  }

  if (unlink(package_path) == -1) {
    error_errno("cannot remove package file");
    unlink(temp_path);
    free(temp_path);
    free(package_path);
    free(requirements);
    return -1;
  }

  if (rename(temp_path, requirements) == -1) {
    error_errno("cannot replace requirements file");
    unlink(temp_path);
    free(temp_path);
    free(package_path);
    free(requirements);
    return -1;
  }

  free(temp_path);
  free(package_path);
  free(requirements);

  return 0;
}

int main(int argc, char **argv) {
  enum Operation operation = OP_NONE;
  char *package = NULL;
  char *version = NULL;
  char *create_name = NULL;

  char **environments = calloc((size_t)argc, sizeof(*environments));
  if (environments == NULL) {
    error_errno("calloc");
    return EXIT_FAILURE;
  }

  int environment_count = 0;
  int syntax_error = 0;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-c") == 0) {
      if (operation != OP_NONE) {
        syntax_error = 1;
        break;
      }

      operation = OP_CREATE;

      /*
       * Support both:
       *   -c ENV
       *   -c -v ENV
       */
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        create_name = argv[++i];
      }
    } else if (strcmp(argv[i], "-v") == 0) {
      if (i + 1 >= argc || argv[i + 1][0] == '\0') {
        syntax_error = 1;
        break;
      }

      environments[environment_count++] = argv[++i];
    } else if (strcmp(argv[i], "-i") == 0) {
      if (operation != OP_NONE || i + 1 >= argc) {
        syntax_error = 1;
        break;
      }

      operation = OP_INSTALL;
      package = argv[++i];

      char *separator = strstr(package, "==");

      if (separator == NULL || separator == package || separator[2] == '\0') {
        syntax_error = 1;
        break;
      }

      *separator = '\0';
      version = separator + 2;
    } else if (strcmp(argv[i], "-r") == 0) {
      if (operation != OP_NONE || i + 1 >= argc) {
        syntax_error = 1;
        break;
      }

      operation = OP_REMOVE;
      package = argv[++i];
    } else {
      syntax_error = 1;
      break;
    }
  }

  if (operation == OP_NONE)
    syntax_error = 1;

  if (operation == OP_CREATE) {
    if (create_name != NULL) {
      if (environment_count != 0)
        syntax_error = 1;
    } else if (environment_count != 1) {
      syntax_error = 1;
    }
  } else {
    if (environment_count == 0)
      syntax_error = 1;
  }

  if (syntax_error) {
    usage(argv[0]);
    free(environments);
    return EXIT_FAILURE;
  }

  int failed = 0;

  if (operation == OP_CREATE) {
    const char *name = create_name != NULL ? create_name : environments[0];

    if (create_environment(name) == -1)
      failed = 1;
  } else {
    /*
     * Keep processing other environments even if one fails.
     */
    for (int i = 0; i < environment_count; i++) {
      int result;

      if (operation == OP_INSTALL) {
        result = install_package(environments[i], package, version);
      } else {
        result = uninstall_package(environments[i], package);
      }

      if (result == -1)
        failed = 1;
    }
  }

  free(environments);

  return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
