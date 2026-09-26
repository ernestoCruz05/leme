#ifndef TIMAO_LOWER_INTERNAL_H
#define TIMAO_LOWER_INTERNAL_H

#include "timao/lower.h"
#include "timao/parser.h"

struct timao_lowered_owner;
struct timao_lowered {
  struct timao_lowered_owner *owner;
};
struct timao_source_entry {
  struct timao_span span;
  size_t parent;
  size_t argument;
  enum timao_token_kind kind;
};
struct timao_lowered_owner {
  struct timao_lowered view;
  struct leme_public_budget *account;
  struct leme_public_builder *builder;
  struct timao_source *source;
  const struct leme_public_value *value;
  struct timao_source_entry *map;
  size_t count;
  size_t capacity;
  size_t references;
};

#endif
