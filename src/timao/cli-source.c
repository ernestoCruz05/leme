#include "timao/cli-internal.h"
#include "control/memory.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

static int resource(struct timao_diagnostic *error) {
  timao_error(error,
              errno == ENOSPC || errno == EOVERFLOW ? "resource_limit"
                                                    : "out_of_memory",
              "unable to retain source");
  return 1;
}

int timao_cli_source_load(struct timao_runtime *runtime, const char *path,
                          int input_fd, struct timao_cli_source *out,
                          struct timao_diagnostic *error) {
  timao_diagnostic_destroy(error);
  if (out != NULL)
    *out = (struct timao_cli_source){0};
  if (runtime == NULL || path == NULL || out == NULL) {
    timao_error(error, "source_error", "invalid source input");
    return 2;
  }
  const bool owned = strcmp(path, "-") != 0;
  const int fd =
      owned ? open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC) : input_fd;
  const int flags = fd >= 0 ? fcntl(fd, F_GETFL) : -1;
  bool changed = false;
  int result = 2;
  if (flags < 0) {
    timao_error(error, "source_error", "unable to open source");
    goto done;
  }
  if ((flags & O_NONBLOCK) == 0) {
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
      timao_error(error, "source_error",
                  "unable to make source input nonblocking");
      goto done;
    }
    changed = true;
  }
  size_t capacity =
      runtime->limits.source_bytes < 4096 ? runtime->limits.source_bytes : 4096;
  out->data = leme_control_alloc(runtime->language_account, capacity);
  if (out->data == NULL) {
    result = resource(error);
    goto done;
  }
  for (;;) {
    if (timao_runtime_cancelled(runtime)) {
      timao_error(error, "cancelled", "source reading interrupted");
      result = timao_runtime_exit_status(runtime, 1);
      break;
    }
    if (out->length == capacity && capacity < runtime->limits.source_bytes) {
      const size_t next = capacity > runtime->limits.source_bytes / 2
                              ? runtime->limits.source_bytes
                              : capacity * 2;
      char *replacement = leme_control_realloc(out->data, next);
      if (replacement == NULL) {
        result = resource(error);
        break;
      }
      out->data = replacement;
      capacity = next;
    }
    char extra = 0;
    const bool full = out->length == capacity;
    const size_t space = capacity - out->length;
    const size_t amount = full ? 1 : (space > 4096 ? 4096 : space);
    const ssize_t received =
        read(fd, full ? &extra : out->data + out->length, amount);
    if (received == 0) {
      result = 0;
      break;
    }
    if (received > 0) {
      if (full) {
        timao_error(error, "resource_limit", "source exceeds byte limit");
        result = 1;
        break;
      }
      out->length += (size_t)received;
      continue;
    }
    if (errno == EINTR)
      continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      const enum timao_runtime_wait waited = timao_runtime_poll_fd(
          runtime, UINT64_MAX,
          &(const struct timao_runtime_poll_input){.fd = fd, .events = POLLIN});
      if (waited == TIMAO_RUNTIME_PROGRESS)
        continue;
      timao_error(error,
                  waited == TIMAO_RUNTIME_CANCELLED ? "cancelled"
                                                    : "source_error",
                  "source input wait stopped");
      result = timao_runtime_exit_status(runtime, 1);
      break;
    }
    timao_error(error, "source_error", "unable to read source");
    break;
  }
done:
  if (changed && fcntl(fd, F_SETFL, flags) < 0) {
    runtime->io_error = true;
    timao_error(error, "io_error", "unable to restore source descriptor flags");
    result = 1;
  }
  if (owned && fd >= 0 && close(fd) < 0 && result == 0) {
    timao_error(error, "source_error", "unable to close source");
    result = 2;
  }
  if (result != 0)
    timao_cli_source_destroy(out);
  return result;
}
void timao_cli_source_destroy(struct timao_cli_source *source) {
  if (source == NULL)
    return;
  leme_control_free(source->data);
  *source = (struct timao_cli_source){0};
}
