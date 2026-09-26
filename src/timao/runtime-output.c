#include "timao/runtime-internal.h"

static enum timao_status output_error(struct timao_diagnostic *error,
                                      enum leme_public_status status) {
  const char *code = status == LEME_PUBLIC_OOM       ? "out_of_memory"
                     : status == LEME_PUBLIC_LIMIT   ? "resource_limit"
                     : status == LEME_PUBLIC_INVALID ? "type_error"
                                                     : "io_error";
  return timao_error(error, code, "unable to format or write output");
}

struct destination {
  struct timao_output_writer **writer;
  int fd;
};
static enum timao_status write_output(struct timao_runtime *runtime,
                                      const struct timao_output_buffer *buffer,
                                      struct destination destination,
                                      struct timao_diagnostic *error) {
  if (buffer->length == 0)
    return TIMAO_OK;
  if (*destination.writer == NULL) {
    const enum leme_public_status status = timao_output_writer_create(
        runtime->account, destination.fd, destination.writer);
    if (status != LEME_PUBLIC_OK)
      return output_error(error, status);
  }
  runtime->writing = true;
  runtime->writing_fd = -1;
  enum timao_status result = TIMAO_ERROR;
  size_t offset = 0;
  if (runtime->output_notify != NULL &&
      runtime->output_notify(runtime->output_context, true, error) != TIMAO_OK)
    goto done;
  runtime->writing_fd = destination.fd;
  for (;;) {
    const enum timao_output_progress progress = timao_output_write(
        *destination.writer, buffer, &offset, timao_runtime_cancelled(runtime));
    if (progress == TIMAO_OUTPUT_DONE) {
      result = TIMAO_OK;
      break;
    }
    if (progress == TIMAO_OUTPUT_ERROR) {
      runtime->io_error = true;
      timao_error(error, "io_error", "output write failed");
      break;
    }
    const enum timao_runtime_wait waited =
        progress == TIMAO_OUTPUT_CANCELLED
            ? (runtime->io_error ? TIMAO_RUNTIME_IO_ERROR
                                 : TIMAO_RUNTIME_CANCELLED)
            : timao_runtime_poll(runtime, UINT64_MAX);
    if (waited != TIMAO_RUNTIME_PROGRESS) {
      timao_error(error,
                  waited == TIMAO_RUNTIME_CANCELLED ? "cancelled" : "io_error",
                  "output interrupted");
      break;
    }
  }
  runtime->writing_fd = -1;
  if (result == TIMAO_OK && runtime->output_notify != NULL)
    result = runtime->output_notify(runtime->output_context, false, error);
done:
  if (offset != 0 && offset < buffer->length)
    runtime->output_incomplete = true;
  runtime->writing_fd = -1;
  runtime->writing = false;
  return result;
}

static enum timao_status ready(struct timao_runtime *runtime,
                               struct timao_diagnostic *error) {
  if (runtime == NULL || runtime->writing)
    return timao_error(error, "invalid_argument",
                       "invalid or reentrant output");
  if (runtime->output_incomplete) {
    runtime->io_error = true;
    return timao_error(error, "io_error",
                       "previous output record was interrupted");
  }
  if (timao_runtime_cancelled(runtime))
    return timao_error(error, runtime->io_error ? "io_error" : "cancelled",
                       "output interrupted");
  return TIMAO_OK;
}

enum timao_status timao_runtime_emit(struct timao_runtime *runtime,
                                     const struct leme_public_value *value,
                                     struct timao_diagnostic *error) {
  if (ready(runtime, error) != TIMAO_OK)
    return TIMAO_ERROR;
  struct timao_output_buffer buffer = {0};
  const enum leme_public_status status = timao_output_format(
      runtime->account, runtime->output_mode, value, &buffer);
  if (status != LEME_PUBLIC_OK)
    return output_error(error, status);
  const enum timao_status result =
      write_output(runtime, &buffer,
                   (struct destination){.writer = &runtime->output,
                                        .fd = runtime->output_fd},
                   error);
  timao_output_buffer_destroy(&buffer);
  return result;
}

enum timao_status timao_runtime_text(struct timao_runtime *runtime,
                                     struct leme_public_text text,
                                     struct timao_diagnostic *error) {
  if (ready(runtime, error) != TIMAO_OK)
    return TIMAO_ERROR;
  const struct timao_output_buffer buffer = {.data = text.data,
                                             .length = text.length};
  return write_output(runtime, &buffer,
                      (struct destination){.writer = &runtime->descriptions,
                                           .fd = runtime->error_fd},
                      error);
}
