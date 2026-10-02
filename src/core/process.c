#include "core/process.h"

#include "config/config.h"
#include "core/server.h"
#include "shell/xwayland.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wlr/util/log.h>

extern char **environ;

static bool leme_process_close(int fd) { return close(fd) == 0; }

static bool leme_process_set_cloexec(int fd) {
  int flags = 0;

  do {
    flags = fcntl(fd, F_GETFD);
  } while (flags < 0 && errno == EINTR);
  if (flags < 0) {
    return false;
  }
  do {
    flags = fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
  } while (flags < 0 && errno == EINTR);
  return flags == 0;
}

static void leme_process_report_error(int fd, int error) {
  ssize_t written = -1;

  do {
    written = write(fd, &error, sizeof(error));
  } while (written < 0 && errno == EINTR);
  (void)written;
  (void)leme_process_close(fd);
}

static void leme_process_exit_error(int fd, int error) {
  leme_process_report_error(fd, error);
  _exit(EXIT_FAILURE);
}

static bool leme_process_env_named(const char *entry, const char *name) {
  const size_t length = strlen(name);

  return strncmp(entry, name, length) == 0 && entry[length] == '=';
}

static const char *leme_process_env_value(char *const *env, const char *name) {
  for (size_t index = 0; env[index] != NULL; index++) {
    if (leme_process_env_named(env[index], name)) {
      return env[index] + strlen(name) + 1;
    }
  }
  return NULL;
}

static bool leme_process_env_configured_later(const struct leme_config *config,
                                              size_t index) {
  for (size_t later = index + 1; later < config->environment_count; later++) {
    if (strcmp(config->environment[later].name,
               config->environment[index].name) == 0) {
      return true;
    }
  }
  return false;
}

static bool leme_process_env_replaced(const struct leme_server *server,
                                      const char *entry) {
  if (leme_process_env_named(entry, "DISPLAY") ||
      (server->socket != NULL &&
       leme_process_env_named(entry, "WAYLAND_DISPLAY"))) {
    return true;
  }
  for (size_t index = 0;
       server->config != NULL && index < server->config->environment_count;
       index++) {
    if (leme_process_env_named(entry,
                               server->config->environment[index].name)) {
      return true;
    }
  }
  return false;
}

static char *leme_process_env_entry(const char *name, const char *value) {
  const size_t size = strlen(name) + strlen(value) + 2;
  char *entry = malloc(size);

  if (entry != NULL) {
    (void)snprintf(entry, size, "%s=%s", name, value);
  }
  return entry;
}

static void leme_process_free_env(char **env) {
  for (size_t index = 0; env[index] != NULL; index++) {
    free(env[index]);
  }
  free((void *)env);
}

static char **leme_process_build_env(const struct leme_server *server) {
  const struct leme_config *config = server->config;
  const char *display = leme_xwayland_display(server);
  const size_t configured = config != NULL ? config->environment_count : 0;
  size_t inherited = 0;
  size_t used = 0;
  char **env;

  while (environ[inherited] != NULL) {
    inherited++;
  }
  env = (char **)calloc(inherited + configured + 3, sizeof(*env));
  if (env == NULL) {
    return NULL;
  }
  for (size_t index = 0; index < inherited; index++) {
    if (leme_process_env_replaced(server, environ[index])) {
      continue;
    }
    env[used] = strdup(environ[index]);
    if (env[used++] == NULL) {
      goto fail;
    }
  }
  for (size_t index = 0; index < configured; index++) {
    if (leme_process_env_configured_later(config, index)) {
      continue;
    }
    env[used] = leme_process_env_entry(config->environment[index].name,
                                       config->environment[index].value);
    if (env[used++] == NULL) {
      goto fail;
    }
  }
  if (server->socket != NULL) {
    env[used] = leme_process_env_entry("WAYLAND_DISPLAY", server->socket);
    if (env[used++] == NULL) {
      goto fail;
    }
  }
  if (display != NULL) {
    env[used] = leme_process_env_entry("DISPLAY", display);
    if (env[used++] == NULL) {
      goto fail;
    }
  }
  return env;

fail:
  leme_process_free_env(env);
  return NULL;
}

static int leme_process_exec_path(char *const *argv, char *const *env) {
  static const char default_path[] = "/bin:/usr/bin";
  const char *path = leme_process_env_value(env, "PATH");
  const char *entry;
  const char *next;
  const char *executable = argv[0];
  bool saw_eacces = false;

  if (strchr(executable, '/') != NULL) {
    execve(executable, argv, env);
    return errno;
  }
  if (path == NULL) {
    path = default_path;
  }
  entry = path;
  for (;;) {
    char candidate[PATH_MAX] = {0};
    size_t directory_length;
    int error;
    int written;

    next = strchr(entry, ':');
    directory_length = next == NULL ? strlen(entry) : (size_t)(next - entry);
    if (directory_length > (size_t)INT_MAX) {
      return ENAMETOOLONG;
    }
    if (directory_length == 0) {
      // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
      written = snprintf(candidate, sizeof(candidate), "%s", executable);
    } else {
      // NOLINTNEXTLINE(clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
      written = snprintf(candidate, sizeof(candidate), "%.*s/%s",
                         (int)directory_length, entry, executable);
    }
    if (written < 0 || (size_t)written >= sizeof(candidate)) {
      return ENAMETOOLONG;
    }
    // NOLINTNEXTLINE(clang-analyzer-optin.taint.GenericTaint)
    execve(candidate, argv, env);
    error = errno;
    if (error == EACCES) {
      saw_eacces = true;
    } else if (error != ENOENT && error != ENOTDIR) {
      return error;
    }
    if (next == NULL) {
      break;
    }
    entry = next + 1;
  }
  return saw_eacces ? EACCES : ENOENT;
}

static void leme_process_setup_and_exec(char *const *argv, char *const *env,
                                        int error_fd) {
  sigset_t unblocked;

  if (sigemptyset(&unblocked) < 0 ||
      sigprocmask(SIG_SETMASK, &unblocked, NULL) < 0) {
    leme_process_exit_error(error_fd, errno);
  }
  if (setsid() < 0) {
    leme_process_exit_error(error_fd, errno);
  }
  leme_process_exit_error(error_fd, leme_process_exec_path(argv, env));
}

static bool leme_process_wait(pid_t process) {
  int status = 0;

  while (waitpid(process, &status, 0) < 0) {
    if (errno != EINTR) {
      return false;
    }
  }
  return WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS;
}

static bool leme_process_read_error(int fd, int *error) {
  ssize_t count = -1;

  do {
    count = read(fd, error, sizeof(*error));
  } while (count < 0 && errno == EINTR);
  return count == 0;
}

static bool leme_process_spawn_env(char *const *argv, char *const *env) {
  int error_pipe[2] = {-1, -1};
  pid_t process = -1;
  bool waited = false;
  bool read_success = false;
  bool close_success = false;
  int child_error = 0;
  int error = 0;

  if (pipe(error_pipe) < 0) {
    wlr_log(WLR_ERROR, "leme: pipe failed: %s", strerror(errno));
    return false;
  }
  if (!leme_process_set_cloexec(error_pipe[1])) {
    error = errno;
    (void)leme_process_close(error_pipe[0]);
    (void)leme_process_close(error_pipe[1]);
    wlr_log(WLR_ERROR, "leme: cannot configure spawn pipe: %s",
            strerror(error));
    return false;
  }
  process = fork();
  if (process < 0) {
    error = errno;
    (void)leme_process_close(error_pipe[0]);
    (void)leme_process_close(error_pipe[1]);
    wlr_log(WLR_ERROR, "leme: fork failed: %s", strerror(error));
    return false;
  }
  if (process == 0) {
    pid_t child = -1;

    if (!leme_process_close(error_pipe[0])) {
      leme_process_exit_error(error_pipe[1], errno);
    }
    child = fork();
    if (child < 0) {
      leme_process_exit_error(error_pipe[1], errno);
    }
    if (child > 0) {
      (void)leme_process_close(error_pipe[1]);
      _exit(EXIT_SUCCESS);
    }
    leme_process_setup_and_exec(argv, env, error_pipe[1]);
  }
  close_success = leme_process_close(error_pipe[1]);
  waited = leme_process_wait(process);
  read_success = leme_process_read_error(error_pipe[0], &child_error);
  if (!leme_process_close(error_pipe[0])) {
    close_success = false;
  }
  if (!close_success || !waited || !read_success) {
    if (!read_success && child_error != 0) {
      wlr_log(WLR_ERROR, "leme: failed to spawn %s: %s", argv[0],
              strerror(child_error));
    } else {
      wlr_log(WLR_ERROR, "leme: failed to spawn %s", argv[0]);
    }
    return false;
  }
  wlr_log(WLR_INFO, "leme: spawned %s", argv[0]);
  return true;
}

bool leme_process_spawn_detached(struct leme_server *server,
                                 char *const *argv) {
  char **env;
  bool spawned;

  if (server == NULL || argv == NULL || argv[0] == NULL) {
    wlr_log(WLR_ERROR, "%s", "leme: spawn requires an argv vector");
    return false;
  }
  env = leme_process_build_env(server);
  if (env == NULL) {
    wlr_log(WLR_ERROR, "leme: cannot build the environment for %s", argv[0]);
    return false;
  }
  spawned = leme_process_spawn_env(argv, env);
  leme_process_free_env(env);
  return spawned;
}
