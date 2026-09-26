#ifndef TIMAO_DIAGNOSTIC_H
#define TIMAO_DIAGNOSTIC_H

#include "timao/language.h"

struct timao_source;
struct timao_lowered;
struct timao_call_site {
  struct timao_source *source;
  struct timao_span span;
  struct timao_span name;
};
struct timao_diagnostic {
  char code[48];
  char message[192];
  struct timao_span span;
  struct timao_source *source;
  char fallback_name[192];
  size_t fallback_name_length;
  size_t line;
  size_t column;
  struct timao_call_site calls[16];
  size_t call_count;
  bool calls_truncated;
  struct leme_public_builder *payload_owner;
  void *retained_owner;
  void (*release_owner)(void *owner);
  const struct leme_public_value *payload;
};

void timao_diagnostic_init(struct timao_diagnostic *error);
void timao_diagnostic_destroy(struct timao_diagnostic *error);
void timao_diagnostic_at(struct timao_diagnostic *error,
                         struct timao_source *source, struct timao_span span);
void timao_diagnostic_input(struct timao_diagnostic *error,
                            const struct timao_input *input);
void timao_diagnostic_call(struct timao_diagnostic *error,
                           struct timao_source *source, struct timao_span span,
                           struct timao_span name);
void timao_diagnostic_position(const struct timao_diagnostic *error,
                               size_t *line, size_t *column);
struct leme_public_text
timao_diagnostic_name(const struct timao_diagnostic *error);

enum timao_status
timao_diagnostic_value(struct timao_execution *execution,
                       const struct timao_diagnostic *diagnostic,
                       const struct timao_value **out,
                       struct timao_diagnostic *error);
enum timao_status
timao_diagnostic_take_owned(void *owner, void (*release)(void *),
                            const struct leme_public_value *payload,
                            const struct timao_lowered *descriptor,
                            struct timao_diagnostic *error);
enum timao_status timao_diagnostic_take_remote(
    struct leme_public_builder *owner, const struct leme_public_value *payload,
    const struct timao_lowered *descriptor, struct timao_diagnostic *error);

enum timao_status timao_error(struct timao_diagnostic *error, const char *code,
                              const char *message);

#endif
