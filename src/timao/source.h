#ifndef TIMAO_SOURCE_H
#define TIMAO_SOURCE_H

#include "timao/heap.h"

struct timao_source {
  struct timao_input input;
  struct leme_public_budget *account;
  size_t references;
  char *bytes_storage;
  char *name_storage;
  size_t *lines;
  size_t line_count;
};

enum timao_status timao_source_create(struct leme_public_budget *account,
                                      const struct timao_limits *limits,
                                      const struct timao_input *input,
                                      struct timao_source **out,
                                      struct timao_diagnostic *error);
void timao_source_ref(struct timao_source *source);
void timao_source_unref(struct timao_source *source);
bool timao_utf8_width(struct leme_public_text text, size_t offset,
                      size_t *width);
void timao_source_position(const struct timao_source *source, size_t offset,
                           size_t *line, size_t *column);
enum timao_status timao_source_check(const struct timao_source *source,
                                     struct timao_diagnostic *error);

#endif
