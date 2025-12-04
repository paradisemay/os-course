#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <vtsh.h>

#define MAX_ARGS 128

static int tokenize(char *line, char **argv) {
  int argc = 0;
  char *saveptr = NULL;
  char *token = strtok_r(line, " \t", &saveptr);

  while (token != NULL && argc < MAX_ARGS - 1) {
    argv[argc++] = token;
    token = strtok_r(NULL, " \t", &saveptr);
  }

  argv[argc] = NULL;
  return argc;
}

static int is_whitespace_only(const char *line) {
  for (const unsigned char *p = (const unsigned char *)line; *p != '\0'; ++p) {
    if (!isspace(*p)) {
      return 0;
    }
  }
  return 1;
}

static int resolve_executable(const char *command, char *buffer, size_t buffer_size) {
  if (command == NULL || buffer_size == 0) {
    return 0;
  }

  if (strchr(command, '/')) {
    if (access(command, X_OK) == 0) {
      strncpy(buffer, command, buffer_size - 1);
      buffer[buffer_size - 1] = '\0';
      return 1;
    }
    return 0;
  }

  const char *path_env = getenv("PATH");
  if (path_env == NULL || *path_env == '\0') {
    path_env = "/bin:/usr/bin";
  }

  char *paths = strdup(path_env);
  if (paths == NULL) {
    return 0;
  }

  int found = 0;
  char *saveptr = NULL;
  char *dir = strtok_r(paths, ":", &saveptr);
  while (dir != NULL) {
    if (*dir != '\0') {
      int written = snprintf(buffer, buffer_size, "%s/%s", dir, command);
      if (written > 0 && (size_t)written < buffer_size && access(buffer, X_OK) == 0) {
        found = 1;
        break;
      }
    }
    dir = strtok_r(NULL, ":", &saveptr);
  }

  free(paths);
  return found;
}

int main() {
  setvbuf(stdin, NULL, _IONBF, 0);
  setvbuf(stdout, NULL, _IONBF, 0);

  char *line = NULL;
  size_t line_cap = 0;
  ssize_t line_len = 0;

  while (1) {
    printf("%s", vtsh_prompt());

    line_len = getline(&line, &line_cap, stdin);
    if (line_len == -1) {
      break;
    }

    while (line_len > 0 && (line[line_len - 1] == '\n' || line[line_len - 1] == '\r')) {
      line[--line_len] = '\0';
    }

    if (is_whitespace_only(line)) {
      continue;
    }

    char *argv[MAX_ARGS];
    int argc = tokenize(line, argv);
    if (argc == 0) {
      continue;
    }

    char exec_path[PATH_MAX];
    if (!resolve_executable(argv[0], exec_path, sizeof(exec_path))) {
      printf("Command not found\n");
      continue;
    }

    argv[0] = exec_path;

    pid_t pid = fork();
    if (pid == -1) {
      continue;
    }

    if (pid == 0) {
      execv(exec_path, argv);
      if (errno == ENOENT) {
        printf("Command not found\n");
      }
      _exit(0);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {
    }
  }

  free(line);
  return 0;
}
