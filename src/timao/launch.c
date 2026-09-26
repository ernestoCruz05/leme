#include "timao/launch.h"
#include "control/memory.h"
#include "public/value-internal.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

struct notice {
  uint32_t kind;
  int32_t error, pid;
};

_Static_assert(sizeof(struct notice) == 12 && sizeof(struct notice) <= PIPE_BUF,
               "launch notice must be one padding-free atomic pipe write");

struct timao_launch {
  pid_t helper;
  int fd, error;
  uint64_t deadline;
  enum timao_launch_state state;
  struct notice input;
  size_t used;
  bool detached, failed, eof, helper_failed;
};

static void *allocate(struct leme_public_budget *account, size_t bytes,
                      enum leme_public_status *status) {
  const size_t overhead = leme_control_allocation_overhead();
  if (bytes > SIZE_MAX - overhead) {
    *status = LEME_PUBLIC_LIMIT;
    return NULL;
  }
  *status = leme_public_budget_reserve(account, bytes + overhead);
  if (*status != LEME_PUBLIC_OK)
    return NULL;
  leme_public_budget_release(account, bytes + overhead);
  void *value = leme_control_alloc(account, bytes);
  if (value == NULL)
    *status = LEME_PUBLIC_OOM;
  else
    memset(value, 0, bytes);
  return value;
}

static void free_arguments(char **argv, size_t count) {
  if (argv == NULL)
    return;
  for (size_t i = 0; i < count; ++i)
    leme_control_free(argv[i]);
  leme_control_free((void *)argv);
}

static enum leme_public_status
copy_arguments(struct leme_public_budget *account,
               const struct leme_public_value *value, char ***out) {
  *out = NULL;
  const size_t count = leme_public_length(value);
  if (leme_public_kind(value) != LEME_PUBLIC_ARRAY || !value->owner->sealed ||
      count == 0)
    return LEME_PUBLIC_INVALID;
  if (count >= SIZE_MAX / sizeof(char *))
    return LEME_PUBLIC_LIMIT;
  enum leme_public_status status = LEME_PUBLIC_OK;
  char **argv =
      (char **)allocate(account, (count + 1) * sizeof(*argv), &status);
  if (argv == NULL)
    return status;
  for (size_t i = 0; i < count; ++i) {
    struct leme_public_text text = {0};
    if (leme_public_as_text(leme_public_at(value, i), &text) !=
            LEME_PUBLIC_OK ||
        (i == 0 && text.length == 0) ||
        memchr(text.data, '\0', text.length) != NULL) {
      status = LEME_PUBLIC_INVALID;
      goto fail;
    }
    if (text.length == SIZE_MAX) {
      status = LEME_PUBLIC_LIMIT;
      goto fail;
    }
    argv[i] = allocate(account, text.length + 1, &status);
    if (argv[i] == NULL)
      goto fail;
    memcpy(argv[i], text.data, text.length);
  }
  *out = argv;
  return LEME_PUBLIC_OK;
fail:
  free_arguments(argv, count);
  return status;
}

static enum leme_public_status prepare_paths(struct leme_public_budget *account,
                                             const char *executable,
                                             char ***out, size_t *count) {
  *out = NULL;
  *count = 1;
  const bool direct = strchr(executable, '/') != NULL;
  const char *path = direct ? "" : getenv("PATH");
  if (path == NULL)
    path = "/bin:/usr/bin";
  const size_t length = strnlen(path, 67108864);
  if (length == 67108864)
    return LEME_PUBLIC_LIMIT;
  for (size_t i = 0; i < length; ++i)
    if (path[i] == ':')
      ++*count;
  if (*count >= SIZE_MAX / sizeof(char *))
    return LEME_PUBLIC_LIMIT;
  enum leme_public_status status = LEME_PUBLIC_OK;
  char **paths =
      (char **)allocate(account, (*count + 1) * sizeof(*paths), &status);
  if (paths == NULL)
    return status;
  const size_t name = strlen(executable);
  size_t start = 0;
  for (size_t i = 0; i < *count; ++i) {
    size_t end = start;
    while (end < length && path[end] != ':')
      ++end;
    const size_t prefix = end - start;
    if (name > SIZE_MAX - 2 || prefix > SIZE_MAX - name - 2) {
      status = LEME_PUBLIC_LIMIT;
      goto fail;
    }
    const size_t offset = prefix != 0 ? prefix + 1 : 0;
    paths[i] = allocate(account, offset + name + 1, &status);
    if (paths[i] == NULL)
      goto fail;
    if (prefix != 0) {
      memcpy(paths[i], path + start, prefix);
      paths[i][prefix] = '/';
    }
    memcpy(paths[i] + offset, executable, name);
    start = end + 1;
  }
  *out = paths;
  return LEME_PUBLIC_OK;
fail:
  free_arguments(paths, *count);
  return status;
}

static bool send_notice(int fd, struct notice notice) {
  ssize_t count = 0;
  do {
    count = write(fd, &notice, sizeof(notice));
  } while (count < 0 && errno == EINTR);
  return count == (ssize_t)sizeof(notice);
}

static _Noreturn void child_error(int fd, int error) {
  if (!send_notice(fd, (struct notice){.kind = 2, .error = error}))
    _exit(126);
  _exit(127);
}

static _Noreturn void child(int fd, char *const *argv, char *const *paths,
                            size_t count, const sigset_t *mask) {
  const int null = open("/dev/null", O_RDWR | O_CLOEXEC);
  if (null < 0)
    child_error(fd, errno);
  for (int destination = 0; destination < 3; ++destination)
    if (dup2(null, destination) < 0 || fcntl(destination, F_SETFD, 0) < 0)
      child_error(fd, errno);
  if (fd > 3 && close_range(3, (unsigned)fd - 1, 0) < 0)
    child_error(fd, errno);
  if (close_range((unsigned)fd + 1, UINT_MAX, 0) < 0 || setsid() < 0)
    child_error(fd, errno);
  struct sigaction action = {.sa_handler = SIG_DFL};
  if (sigemptyset(&action.sa_mask) < 0)
    child_error(fd, errno);
  for (int number = 1; number < NSIG; ++number) {
    if (number == SIGKILL || number == SIGSTOP)
      continue;
    if (sigaction(number, &action, NULL) < 0 && errno != EINVAL)
      child_error(fd, errno);
  }
  if (sigprocmask(SIG_SETMASK, mask, NULL) < 0)
    child_error(fd, errno);
  const pid_t pid = fork();
  if (pid < 0)
    child_error(fd, errno);
  if (pid != 0) {
    if (!send_notice(fd, (struct notice){.kind = 1, .pid = pid}))
      _exit(126);
    _exit(0);
  }
  bool denied = false;
  for (size_t i = 0; i < count; ++i) {
    execve(paths[i], argv, environ);
    const int error = errno;
    if (error == EACCES)
      denied = true;
    else if (error != ENOENT && error != ENOTDIR)
      child_error(fd, error);
  }
  child_error(fd, denied ? EACCES : ENOENT);
}

static bool move_fd(int *fd) {
  if (*fd >= 3)
    return true;
  const int replacement = fcntl(*fd, F_DUPFD_CLOEXEC, 3);
  if (replacement < 0)
    return false;
  const int closed = close(*fd);
  *fd = replacement;
  return closed == 0;
}

enum leme_public_status timao_launch_start(
    struct leme_public_budget *account, const struct leme_public_value *argv,
    const struct timao_launch_options *options, struct timao_launch **out) {
  if (out == NULL)
    return LEME_PUBLIC_INVALID;
  *out = NULL;
  if (account == NULL || options == NULL)
    return LEME_PUBLIC_INVALID;
  char **copied = NULL;
  enum leme_public_status status = copy_arguments(account, argv, &copied);
  if (status != LEME_PUBLIC_OK)
    return status;
  char **paths = NULL;
  size_t path_count = 0;
  struct timao_launch *launch = NULL;
  int descriptors[2] = {-1, -1};
  status = prepare_paths(account, copied[0], &paths, &path_count);
  if (status != LEME_PUBLIC_OK)
    goto fail;
  launch = allocate(account, sizeof(*launch), &status);
  if (launch == NULL)
    goto fail;
  launch->fd = -1;
  sigset_t mask = {0};
  if (options->child_mask != NULL)
    mask = *options->child_mask;
  else if (sigprocmask(SIG_SETMASK, NULL, &mask) < 0 ||
           sigdelset(&mask, SIGINT) < 0 || sigdelset(&mask, SIGTERM) < 0 ||
           sigdelset(&mask, SIGPIPE) < 0) {
    status = LEME_PUBLIC_UNAVAILABLE;
    goto fail;
  }
  if (pipe2(descriptors, O_CLOEXEC | O_NONBLOCK) < 0 ||
      !move_fd(&descriptors[0]) || !move_fd(&descriptors[1])) {
    status = LEME_PUBLIC_UNAVAILABLE;
    goto fail;
  }
  launch->deadline =
      options->now_ms > UINT64_MAX - 5000 ? UINT64_MAX : options->now_ms + 5000;
  launch->helper = fork();
  if (launch->helper < 0) {
    status = LEME_PUBLIC_UNAVAILABLE;
    goto fail;
  }
  if (launch->helper == 0)
    child(descriptors[1], copied, paths, path_count, &mask);
  launch->fd = descriptors[0];
  if (close(descriptors[1]) < 0) {
    launch->state = TIMAO_LAUNCH_UNKNOWN;
    launch->error = errno;
  }
  free_arguments(paths, path_count);
  free_arguments(copied, leme_public_length(argv));
  *out = launch;
  return LEME_PUBLIC_OK;
fail:
  for (size_t i = 0; i < 2; ++i)
    if (descriptors[i] >= 0 && close(descriptors[i]) < 0)
      status = LEME_PUBLIC_UNAVAILABLE;
  leme_control_free(launch);
  free_arguments(paths, path_count);
  free_arguments(copied, leme_public_length(argv));
  return status;
}

int timao_launch_fd(const struct timao_launch *launch) {
  return launch != NULL ? launch->fd : -1;
}
pid_t timao_launch_helper(const struct timao_launch *launch) {
  return launch != NULL ? launch->helper : 0;
}
int timao_launch_error(const struct timao_launch *launch) {
  return launch != NULL ? launch->error : EINVAL;
}

static void reap(struct timao_launch *launch) {
  if (launch->helper <= 0)
    return;
  int status = 0;
  const pid_t result = waitpid(launch->helper, &status, WNOHANG);
  if (result == launch->helper) {
    launch->helper = 0;
    launch->helper_failed = !WIFEXITED(status) || WEXITSTATUS(status) != 0;
  } else if (result < 0 && errno == ECHILD)
    launch->helper = 0;
  else if (result < 0 && errno != EINTR) {
    launch->state = TIMAO_LAUNCH_UNKNOWN;
    launch->error = errno;
  }
}

static void lose(struct timao_launch *launch, int error) {
  if (launch->state == TIMAO_LAUNCH_PENDING) {
    launch->state = TIMAO_LAUNCH_UNKNOWN;
    launch->error = error;
  }
}

static void receive(struct timao_launch *launch) {
  for (size_t turn = 0; launch->fd >= 0 && turn < 4; ++turn) {
    unsigned char *bytes = (unsigned char *)&launch->input;
    const ssize_t count = read(launch->fd, bytes + launch->used,
                               sizeof(launch->input) - launch->used);
    if (count < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        lose(launch, errno);
      return;
    }
    if (count == 0) {
      launch->eof = true;
      if (launch->used != 0)
        lose(launch, EPROTO);
      if (close(launch->fd) < 0)
        lose(launch, errno);
      launch->fd = -1;
      return;
    }
    launch->used += (size_t)count;
    if (launch->used != sizeof(launch->input))
      continue;
    const struct notice notice = launch->input;
    launch->used = 0;
    launch->input = (struct notice){0};
    if (notice.kind == 1 && notice.pid > 0 && notice.error == 0 &&
        !launch->detached)
      launch->detached = true;
    else if (notice.kind == 2 && notice.error > 0 && notice.pid == 0 &&
             !launch->failed) {
      launch->failed = true;
      if (launch->state == TIMAO_LAUNCH_PENDING)
        launch->error = notice.error;
    } else
      lose(launch, EPROTO);
  }
}

enum timao_launch_state timao_launch_step(struct timao_launch *launch,
                                          uint64_t now_ms, bool cancelled) {
  if (launch == NULL)
    return TIMAO_LAUNCH_FAILED;
  receive(launch);
  reap(launch);
  if (launch->state != TIMAO_LAUNCH_PENDING)
    return launch->state;
  if (launch->failed)
    launch->state = TIMAO_LAUNCH_FAILED;
  else if (launch->eof && launch->helper == 0) {
    if (launch->detached && !launch->helper_failed)
      launch->state = TIMAO_LAUNCH_ACCEPTED;
    else
      lose(launch, EIO);
  } else if (cancelled || now_ms >= launch->deadline)
    lose(launch, cancelled ? ECANCELED : ETIMEDOUT);
  return launch->state;
}

bool timao_launch_destroy(struct timao_launch *launch) {
  if (launch == NULL)
    return true;
  reap(launch);
  if (launch->helper != 0) {
    errno = EBUSY;
    return false;
  }
  if (launch->fd >= 0 && close(launch->fd) < 0)
    launch->error = errno;
  leme_control_free(launch);
  return true;
}
