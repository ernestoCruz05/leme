#ifndef LEME_PUBLIC_VALUE_INTERNAL_H
#define LEME_PUBLIC_VALUE_INTERNAL_H

#include "public/value.h"

struct leme_public_chunk;
struct leme_public_budget;
struct leme_public_member {
  struct leme_public_text key;
  const struct leme_public_value *value;
};
struct leme_public_mark {
  bool visiting;
  size_t height;
  size_t work;
};
struct leme_public_value {
  struct leme_public_builder *owner;
  struct leme_public_value *next;
  struct leme_public_mark *mark;
  enum leme_public_kind kind;
  union {
    bool boolean;
    double number;
    struct leme_public_text string;
    struct {
      size_t count;
      const struct leme_public_value **items;
    } array;
    struct {
      size_t count;
      size_t used;
      struct leme_public_member *members;
    } object;
  } data;
};
struct leme_public_builder {
  size_t maximum;
  size_t bytes;
  struct leme_public_allocator allocator;
  struct leme_public_budget *budget;
  struct leme_public_chunk *chunks;
  struct leme_public_value *values;
  enum leme_public_status status;
  bool sealed;
  struct leme_public_work work;
};

struct leme_public_allocator leme_public_default_allocator(void);
enum leme_public_status
leme_public_builder_with_budget(size_t maximum_bytes,
                                struct leme_public_budget *budget,
                                const struct leme_public_allocator *allocator,
                                struct leme_public_builder **out);
enum leme_public_status leme_public_clone(struct leme_public_builder *b,
                                          const struct leme_public_value *input,
                                          struct leme_public_value **out);

enum leme_public_status leme_public_fail(struct leme_public_builder *b,
                                         enum leme_public_status status);
enum leme_public_status leme_public_mutable(struct leme_public_builder *b);
void *leme_public_allocate(struct leme_public_builder *b, size_t count,
                           size_t size, size_t alignment);
enum leme_public_status leme_public_utf8_copy(struct leme_public_builder *b,
                                              struct leme_public_text input,
                                              bool repair,
                                              struct leme_public_text *out);

#endif
