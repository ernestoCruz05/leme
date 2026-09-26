#include "timao/output.h"
#include "control/memory.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

struct timao_output_writer {
  int fd, original_flags, error;
};

enum leme_public_status
timao_output_writer_create(struct leme_public_budget *account, int fd,
                           struct timao_output_writer **out) {
  if (out == NULL)
    return LEME_PUBLIC_INVALID;
  *out = NULL;
  if (account == NULL || fd < 0)
    return LEME_PUBLIC_INVALID;
  const int flags = fcntl(fd, F_GETFL);
  if (flags < 0)
    return LEME_PUBLIC_UNAVAILABLE;
  struct timao_output_writer *writer =
      leme_control_alloc(account, sizeof(*writer));
  if (writer == NULL)
    return errno == ENOSPC || errno == EOVERFLOW ? LEME_PUBLIC_LIMIT
                                                 : LEME_PUBLIC_OOM;
  *writer = (struct timao_output_writer){.fd = fd, .original_flags = flags};
  if (!(flags & O_NONBLOCK) && fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    const int error = errno;
    leme_control_free(writer);
    errno = error;
    return LEME_PUBLIC_UNAVAILABLE;
  }
  *out = writer;
  return LEME_PUBLIC_OK;
}

int timao_output_writer_destroy(struct timao_output_writer *writer) {
  if (writer == NULL)
    return 0;
  int result = 0;
  int error = 0;
  if (!(writer->original_flags & O_NONBLOCK)) {
    const int flags = fcntl(writer->fd, F_GETFL);
    if (flags < 0 || fcntl(writer->fd, F_SETFL, flags & ~O_NONBLOCK) < 0) {
      result = -1;
      error = errno;
    }
  }
  leme_control_free(writer);
  if (result < 0)
    errno = error;
  return result;
}

struct guarded_result {
  ssize_t count;
  int error;
};

static struct guarded_result write_guarded(int fd, const char *data,
                                           size_t length) {
  sigset_t mask = {0}, old = {0}, pending = {0};
  if (sigemptyset(&mask) < 0 || sigaddset(&mask, SIGPIPE) < 0 ||
      sigprocmask(SIG_BLOCK, &mask, &old) < 0)
    return (struct guarded_result){.count = -1, .error = errno};
  if (sigpending(&pending) < 0) {
    const int error = errno;
    const int result = sigprocmask(SIG_SETMASK, &old, NULL);
    return (struct guarded_result){.count = -1,
                                   .error = result == 0 ? error : errno};
  }
  const bool existed = sigismember(&pending, SIGPIPE) == 1;
  const ssize_t count = write(fd, data, length);
  int error = count < 0 ? errno : 0;
  if (count < 0 && error == EPIPE && !existed) {
    const struct timespec zero = {0};
    int received = 0;
    do {
      received = sigtimedwait(&mask, NULL, &zero);
    } while (received < 0 && errno == EINTR);
    if (received < 0 && errno != EAGAIN)
      error = errno;
  }
  if (sigprocmask(SIG_SETMASK, &old, NULL) < 0)
    error = errno;
  return (struct guarded_result){.count = count, .error = error};
}

enum timao_output_progress
timao_output_write(struct timao_output_writer *writer,
                   const struct timao_output_buffer *buffer, size_t *offset,
                   bool cancelled) {
  if (writer == NULL)
    return TIMAO_OUTPUT_ERROR;
  if (buffer == NULL || offset == NULL || *offset > buffer->length ||
      (buffer->length != 0 && buffer->data == NULL)) {
    writer->error = EINVAL;
    return TIMAO_OUTPUT_ERROR;
  }
  if (writer->error != 0)
    return TIMAO_OUTPUT_ERROR;
  if (cancelled)
    return TIMAO_OUTPUT_CANCELLED;
  if (*offset == buffer->length)
    return TIMAO_OUTPUT_DONE;
  const size_t remaining = buffer->length - *offset;
  const size_t length = remaining > 4096 ? 4096 : remaining;
  const struct guarded_result result =
      write_guarded(writer->fd, buffer->data + *offset, length);
  if (result.count > 0)
    *offset += (size_t)result.count;
  if (result.error != 0) {
    if (result.count < 0 &&
        (result.error == EAGAIN || result.error == EWOULDBLOCK ||
         result.error == EINTR))
      return TIMAO_OUTPUT_WAIT;
    writer->error = result.error;
    return TIMAO_OUTPUT_ERROR;
  }
  return *offset == buffer->length ? TIMAO_OUTPUT_DONE : TIMAO_OUTPUT_WAIT;
}

int timao_output_writer_error(const struct timao_output_writer *writer) {
  return writer != NULL ? writer->error : EINVAL;
}
