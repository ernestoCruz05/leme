#ifndef TIMAO_OUTPUT_H
#define TIMAO_OUTPUT_H

#include "ipc/json.h"
#include "public/budget.h"

enum timao_output_mode {
  TIMAO_OUTPUT_JSON,
  TIMAO_OUTPUT_RAW,
  TIMAO_OUTPUT_HUMAN
};

struct timao_output_buffer {
  const char *data;
  size_t length;
  struct leme_json storage;
};

enum leme_public_status timao_output_format(
    struct leme_public_budget *account, enum timao_output_mode mode,
    const struct leme_public_value *value, struct timao_output_buffer *out);
void timao_output_buffer_destroy(struct timao_output_buffer *buffer);

struct timao_output_writer;
enum timao_output_progress {
  TIMAO_OUTPUT_DONE,
  TIMAO_OUTPUT_WAIT,
  TIMAO_OUTPUT_CANCELLED,
  TIMAO_OUTPUT_ERROR
};
enum leme_public_status
timao_output_writer_create(struct leme_public_budget *account, int fd,
                           struct timao_output_writer **out);
int timao_output_writer_destroy(struct timao_output_writer *writer);
enum timao_output_progress
timao_output_write(struct timao_output_writer *writer,
                   const struct timao_output_buffer *buffer, size_t *offset,
                   bool cancelled);
int timao_output_writer_error(const struct timao_output_writer *writer);

#endif
