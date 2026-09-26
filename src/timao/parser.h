#ifndef TIMAO_PARSER_H
#define TIMAO_PARSER_H

#include "timao/lexer.h"

struct timao_node {
  enum timao_token_kind kind;
  struct timao_span span;
  struct leme_public_text text;
  double number;
  void *owned;
  uint32_t first;
  uint32_t next;
  size_t count;
};
struct timao_program {
  struct timao_source *source;
  struct timao_node *nodes;
  size_t count;
  size_t capacity;
  uint32_t *forms;
  size_t form_count;
  size_t references;
  struct timao_limits limits;
};

static inline bool timao_index_valid(size_t index, size_t count) {
  return index < count;
}

const struct timao_node *timao_node_at(const struct timao_program *program,
                                       uint32_t index);
uint32_t timao_child(const struct timao_program *program, uint32_t index,
                     size_t ordinal);
bool timao_node_is(const struct timao_node *node, const char *text);
enum timao_status timao_validate(struct timao_program *program,
                                 struct timao_diagnostic *error);
enum timao_status timao_pipeline(struct timao_program *program, uint32_t index,
                                 struct timao_diagnostic *error);

#endif
