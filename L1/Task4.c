
#define _XOPEN_SOURCE 700

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#define ERR(source) (perror(source), exit(EXIT_FAILURE))

#define TITLE_SIZE 64
#define DATABASE_RECORD_SIZE 68

struct Metadata {
  char *author;
  char *title;
  char *genre;
};

struct DatabaseEntry {
  unsigned int size;
  char title[TITLE_SIZE];
};

_Static_assert(sizeof(unsigned int) == 4, "unsigned int must be 4 bytes");
_Static_assert(sizeof(struct DatabaseEntry) == DATABASE_RECORD_SIZE,
               "Database record must be 68 bytes");

char *before_colon(char *buf) {
  char *colon = strchr(buf, ':');

  if (colon == NULL)
    return NULL;

  *colon = '\0';
  return buf;
}

char *after_colon(char *buf) {
  char *colon = strchr(buf, ':');

  if (colon == NULL)
    return NULL;

  return colon + 1;
}

void read_metadata(const char *filename, struct Metadata *metadata) {
  FILE *f = fopen(filename, "r");

  if (f == NULL)
    ERR("fopen");

  metadata->author = NULL;
  metadata->title = NULL;
  metadata->genre = NULL;

  char *buf = NULL;
  size_t capacity = 0;
  ssize_t length;

  while ((length = getline(&buf, &capacity, f)) != -1) {
    (void)length;

    char *value = after_colon(buf);

    if (value == NULL)
      continue;

    char *key = before_colon(buf);

    value[strcspn(value, "\r\n")] = '\0';

    char **field = NULL;

    if (strcmp(key, "author") == 0)
      field = &metadata->author;
    else if (strcmp(key, "title") == 0)
      field = &metadata->title;
    else if (strcmp(key, "genre") == 0)
      field = &metadata->genre;

    if (field != NULL) {
      char *copy = strdup(value);

      if (copy == NULL)
        ERR("strdup");

      free(*field);
      *field = copy;
    }
  }

  if (ferror(f))
    ERR("getline");

  free(buf);

  if (fclose(f) == EOF)
    ERR("fclose");
}

void free_metadata(struct Metadata *metadata) {
  free(metadata->author);
  free(metadata->title);
  free(metadata->genre);
}

char *join_paths(const char *path1, const char *path2) {
  size_t len = strlen(path1) + strlen(path2) + 2;
  char *result = malloc(len);

  if (result == NULL)
    ERR("malloc");

  snprintf(result, len, "%s/%s", path1, path2);

  return result;
}

void read_dir(const char *path) {
  DIR *dirp = opendir(path);

  if (dirp == NULL)
    ERR("opendir");

  struct dirent *dp;

  while (1) {
    errno = 0;
    dp = readdir(dirp);

    if (dp == NULL) {
      if (errno != 0)
        ERR("readdir");
      break;
    }

    if (strcmp(dp->d_name, ".") == 0 || strcmp(dp->d_name, "..") == 0)
      continue;

    char *filepath = join_paths(path, dp->d_name);

    struct stat filestat;

    if (lstat(filepath, &filestat) == -1)
      ERR("lstat");

    if (S_ISLNK(filestat.st_mode)) {
      free(filepath);
      continue;
    }

    if (S_ISDIR(filestat.st_mode)) {
      read_dir(filepath);
      free(filepath);
      continue;
    }

    if (!S_ISREG(filestat.st_mode)) {
      free(filepath);
      continue;
    }

    struct Metadata metadata;
    read_metadata(filepath, &metadata);

    char *linkpath_visible = join_paths("index/by-visible-title", dp->d_name);

    char *target_visible = join_paths("../../", filepath);

    if (symlink(target_visible, linkpath_visible) == -1)
      ERR("symlink by-visible-title");

    free(linkpath_visible);
    free(target_visible);

    if (metadata.title != NULL) {

      if (strlen(metadata.title) > TITLE_SIZE)
        metadata.title[TITLE_SIZE] = '\0';

      char *linkpath_title = join_paths("index/by-title", metadata.title);

      char *target_title = join_paths("../../", filepath);

      if (symlink(target_title, linkpath_title) == -1)
        ERR("symlink by-title");

      free(linkpath_title);
      free(target_title);

      if (metadata.genre != NULL) {

        if (strlen(metadata.genre) > TITLE_SIZE)
          metadata.genre[TITLE_SIZE] = '\0';

        char *genre_dir = join_paths("index/by-genre", metadata.genre);

        if (mkdir(genre_dir, 0755) == -1 && errno != EEXIST)
          ERR("mkdir genre");

        char *linkpath_genre = join_paths(genre_dir, metadata.title);

        char *target_genre = join_paths("../../../", filepath);

        if (symlink(target_genre, linkpath_genre) == -1)
          ERR("symlink by-genre");

        free(genre_dir);
        free(linkpath_genre);
        free(target_genre);
      }
    }

    free_metadata(&metadata);
    free(filepath);
  }

  if (closedir(dirp) == -1)
    ERR("closedir");
}

/* ---------- DATABASE ---------- */

/*
 * Returns:
 *   1  - one complete record was read
 *   0  - end of file
 *  -1  - read error or incomplete record
 */
int read_record(int fd, struct DatabaseEntry *entry) {
  char *buffer = (char *)entry;
  size_t total = 0;

  while (total < sizeof(*entry)) {
    struct iovec iov;

    iov.iov_base = buffer + total;
    iov.iov_len = sizeof(*entry) - total;

    ssize_t n = readv(fd, &iov, 1);

    if (n == -1) {
      if (errno == EINTR)
        continue;

      return -1;
    }

    if (n == 0) {
      if (total == 0)
        return 0;

      errno = EINVAL;
      return -1;
    }

    total += (size_t)n;
  }

  return 1;
}

void check_database(const char *database_path) {
  int fd = open(database_path, O_RDONLY);

  if (fd == -1)
    ERR("open database");

  unsigned long database_correct = 0;
  unsigned long database_missing = 0;
  unsigned long database_size_mismatch = 0;

  struct DatabaseEntry entry;
  int result;

  while ((result = read_record(fd, &entry)) == 1) {
    /*
     * The database title may contain no terminating '\0',
     * so copy it into a larger, terminated string.
     */
    char title[TITLE_SIZE + 1];

    memcpy(title, entry.title, TITLE_SIZE);
    title[TITLE_SIZE] = '\0';

    /*
     * Remove padding zeros from the database title.
     */
    size_t title_length = strnlen(title, TITLE_SIZE);
    title[title_length] = '\0';

    char *linkpath = join_paths("index/by-title", title);
    struct stat st;

    /* Is there an entry with this title? */
    if (lstat(linkpath, &st) == -1) {
      if (errno == ENOENT) {
        database_missing++;
        printf("Missing: %s\n", title);
        free(linkpath);
        continue;
      }

      free(linkpath);
      ERR("lstat database entry");
    }

    /* The index entry should be a symbolic link. */
    if (!S_ISLNK(st.st_mode)) {
      database_missing++;
      printf("Missing: %s\n", title);
      free(linkpath);
      continue;
    }

    /* Follow the link to inspect the actual file. */
    if (stat(linkpath, &st) == -1) {
      if (errno == ENOENT) {
        database_missing++;
        printf("Missing: %s (broken link)\n", title);
        free(linkpath);
        continue;
      }

      free(linkpath);
      ERR("stat database entry");
    }

    if (!S_ISREG(st.st_mode)) {
      database_missing++;
      printf("Missing: %s\n", title);
    } else if ((unsigned long long)st.st_size ==
               (unsigned long long)entry.size) {
      database_correct++;
    } else {
      database_size_mismatch++;

      printf("Size mismatch: %s "
             "(database: %u, actual: %lld)\n",
             title, entry.size, (long long)st.st_size);
    }

    free(linkpath);
  }

  if (result == -1)
    ERR("readv database");

  if (close(fd) == -1)
    ERR("close database");

  printf("\n");
  printf("database_correct: %lu\n", database_correct);
  printf("database_missing: %lu\n", database_missing);
  printf("database_size_mismatch: %lu\n", database_size_mismatch);
}

int main(int argc, char **argv) {
  if (argc > 2) {
    fprintf(stderr, "Usage: %s [database_file]\n", argv[0]);
    return EXIT_FAILURE;
  }

  if (mkdir("index", 0755) == -1)
    ERR("mkdir index");

  if (mkdir("index/by-visible-title", 0755) == -1)
    ERR("mkdir by-visible-title");

  if (mkdir("index/by-title", 0755) == -1)
    ERR("mkdir by-title");

  if (mkdir("index/by-genre", 0755) == -1)
    ERR("mkdir by-genre");

  read_dir("library");

  if (argc == 2)
    check_database(argv[1]);

  return EXIT_SUCCESS;
}
