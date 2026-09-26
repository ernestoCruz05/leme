#include "timao/runtime-signal.h"
#include "control/memory.h"

#include <errno.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>

struct timao_runtime_signal {
  sigset_t original;
  int fd, received;
};

static bool signal_owner = false;

enum leme_public_status
timao_runtime_signal_create(struct leme_public_budget *account,
                            struct timao_runtime_signal **out) {
  if (out == NULL)
    return LEME_PUBLIC_INVALID;
  *out = NULL;
  if (account == NULL)
    return LEME_PUBLIC_INVALID;
  if (signal_owner)
    return LEME_PUBLIC_UNAVAILABLE;
  struct timao_runtime_signal *signals =
      leme_control_alloc(account, sizeof(*signals));
  if (signals == NULL)
    return errno == ENOSPC || errno == EOVERFLOW ? LEME_PUBLIC_LIMIT
                                                 : LEME_PUBLIC_OOM;
  *signals = (struct timao_runtime_signal){.fd = -1};
  sigset_t mask = {0};
  bool masked = false;
  if (sigemptyset(&mask) < 0 || sigaddset(&mask, SIGINT) < 0 ||
      sigaddset(&mask, SIGTERM) < 0)
    goto failed;
  if (sigprocmask(SIG_BLOCK, &mask, &signals->original) < 0)
    goto failed;
  masked = true;
  signals->fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
  if (signals->fd < 0)
    goto failed;
  signal_owner = true;
  *out = signals;
  return LEME_PUBLIC_OK;
failed: {
  int error = errno;
  if (masked && sigprocmask(SIG_SETMASK, &signals->original, NULL) < 0)
    error = errno;
  leme_control_free(signals);
  errno = error;
  return error == ENOMEM                      ? LEME_PUBLIC_OOM
         : error == EMFILE || error == ENFILE ? LEME_PUBLIC_LIMIT
                                              : LEME_PUBLIC_UNAVAILABLE;
}
}

int timao_runtime_signal_read(struct timao_runtime_signal *signals) {
  if (signals == NULL)
    return 0;
  struct signalfd_siginfo events[4] = {0};
  const ssize_t count = read(signals->fd, events, sizeof(events));
  if (count < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
      return signals->received;
    return -1;
  }
  if (count == 0 || (size_t)count % sizeof(events[0]) != 0) {
    errno = EIO;
    return -1;
  }
  for (size_t i = 0; i < (size_t)count / sizeof(events[0]); ++i) {
    if (events[i].ssi_signo == SIGTERM)
      signals->received = SIGTERM;
    else if (events[i].ssi_signo == SIGINT && signals->received != SIGTERM)
      signals->received = SIGINT;
  }
  return signals->received;
}

int timao_runtime_signal_destroy(struct timao_runtime_signal *signals) {
  if (signals == NULL)
    return 0;
  int error = 0;
  if (timao_runtime_signal_read(signals) < 0)
    error = errno;
  if (close(signals->fd) < 0 && errno != EINTR)
    error = errno;
  if (sigprocmask(SIG_SETMASK, &signals->original, NULL) < 0)
    error = errno;
  leme_control_free(signals);
  signal_owner = false;
  if (error != 0) {
    errno = error;
    return -1;
  }
  return 0;
}

int timao_runtime_signal_mask(const struct timao_runtime_signal *signals,
                              sigset_t *out) {
  if (out == NULL) {
    errno = EINVAL;
    return -1;
  }
  if (signals != NULL) {
    *out = signals->original;
    return 0;
  }
  return sigprocmask(SIG_SETMASK, NULL, out);
}

int timao_runtime_signal_fd(const struct timao_runtime_signal *signals) {
  return signals != NULL ? signals->fd : -1;
}

void timao_runtime_signal_clear_interrupt(
    struct timao_runtime_signal *signals) {
  if (signals != NULL && signals->received == SIGINT)
    signals->received = 0;
}
