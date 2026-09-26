#ifndef LEME_PUBLIC_VALUE_H
#define LEME_PUBLIC_VALUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LEME_PUBLIC_SAFE_INTEGER INT64_C(9007199254740991)
#define LEME_PUBLIC_MAX_DEPTH 64u
#define LEME_PUBLIC_TEXT(s)                                                    \
  ((struct leme_public_text){.data = (s), .length = sizeof(s) - 1u})

struct leme_public_text {
  const char *data;
  size_t length;
};
struct leme_public_builder;
struct leme_public_value;

enum leme_public_status {
  LEME_PUBLIC_OK,
  LEME_PUBLIC_OOM,
  LEME_PUBLIC_LIMIT,
  LEME_PUBLIC_INVALID,
  LEME_PUBLIC_UNKNOWN_FIELD,
  LEME_PUBLIC_NOT_FOUND,
  LEME_PUBLIC_TYPE_ERROR,
  LEME_PUBLIC_LOCKED,
  LEME_PUBLIC_UNAVAILABLE
};
enum leme_public_kind {
  LEME_PUBLIC_NULL,
  LEME_PUBLIC_BOOLEAN,
  LEME_PUBLIC_NUMBER,
  LEME_PUBLIC_STRING,
  LEME_PUBLIC_ARRAY,
  LEME_PUBLIC_OBJECT
};
struct leme_public_allocator {
  void *context;
  void *(*allocate)(void *context, size_t bytes);
  void (*release)(void *context, void *allocation);
};
struct leme_public_work {
  void *context;
  enum leme_public_status (*step)(void *context, size_t units);
};

enum leme_public_status
leme_public_builder_create(size_t maximum_bytes,
                           const struct leme_public_allocator *allocator,
                           struct leme_public_builder **out);
void leme_public_builder_destroy(struct leme_public_builder *builder);
size_t leme_public_builder_bytes(const struct leme_public_builder *builder);
enum leme_public_status
leme_public_builder_status(const struct leme_public_builder *builder);
void leme_public_builder_set_work(struct leme_public_builder *builder,
                                  const struct leme_public_work *work);
enum leme_public_status
leme_public_builder_seal(struct leme_public_builder *builder,
                         const struct leme_public_value *const *roots,
                         size_t count);

enum leme_public_status leme_public_null(struct leme_public_builder *b,
                                         struct leme_public_value **out);
enum leme_public_status leme_public_boolean(struct leme_public_builder *b,
                                            bool input,
                                            struct leme_public_value **out);
enum leme_public_status leme_public_integer(struct leme_public_builder *b,
                                            int64_t input,
                                            struct leme_public_value **out);
enum leme_public_status leme_public_number(struct leme_public_builder *b,
                                           double input,
                                           struct leme_public_value **out);
enum leme_public_status leme_public_string(struct leme_public_builder *b,
                                           struct leme_public_text input,
                                           bool repair_utf8,
                                           struct leme_public_value **out);
enum leme_public_status leme_public_array(struct leme_public_builder *b,
                                          size_t count,
                                          struct leme_public_value **out);
enum leme_public_status leme_public_object(struct leme_public_builder *b,
                                           size_t count,
                                           struct leme_public_value **out);
enum leme_public_status
leme_public_array_set(struct leme_public_builder *b,
                      struct leme_public_value *array, size_t index,
                      const struct leme_public_value *item);
enum leme_public_status leme_public_object_set(
    struct leme_public_builder *b, struct leme_public_value *object,
    struct leme_public_text key, const struct leme_public_value *item);

enum leme_public_status leme_public_put_null(struct leme_public_builder *b,
                                             struct leme_public_value *object,
                                             struct leme_public_text key);
enum leme_public_status leme_public_put_bool(struct leme_public_builder *b,
                                             struct leme_public_value *object,
                                             struct leme_public_text key,
                                             bool input);
enum leme_public_status leme_public_put_int(struct leme_public_builder *b,
                                            struct leme_public_value *object,
                                            struct leme_public_text key,
                                            int64_t input);
enum leme_public_status leme_public_put_number(struct leme_public_builder *b,
                                               struct leme_public_value *object,
                                               struct leme_public_text key,
                                               double input);
enum leme_public_status leme_public_put_text(struct leme_public_builder *b,
                                             struct leme_public_value *object,
                                             struct leme_public_text key,
                                             struct leme_public_text input,
                                             bool repair_utf8);
enum leme_public_status leme_public_put_cstr(struct leme_public_builder *b,
                                             struct leme_public_value *object,
                                             struct leme_public_text key,
                                             const char *input);

enum leme_public_kind leme_public_kind(const struct leme_public_value *value);
size_t leme_public_length(const struct leme_public_value *value);
const struct leme_public_value *
leme_public_at(const struct leme_public_value *array, size_t index);
const struct leme_public_value *
leme_public_get(const struct leme_public_value *object,
                struct leme_public_text key);
struct leme_public_text
leme_public_key_at(const struct leme_public_value *object, size_t index);
const struct leme_public_value *
leme_public_member_at(const struct leme_public_value *object, size_t index);
enum leme_public_status
leme_public_as_bool(const struct leme_public_value *value, bool *out);
enum leme_public_status
leme_public_as_number(const struct leme_public_value *value, double *out);
enum leme_public_status
leme_public_as_integer(const struct leme_public_value *value, int64_t *out);
enum leme_public_status
leme_public_as_text(const struct leme_public_value *value,
                    struct leme_public_text *out);
bool leme_public_equal(const struct leme_public_value *left,
                       const struct leme_public_value *right);
enum leme_public_status leme_public_equal_checked(
    const struct leme_public_value *left, const struct leme_public_value *right,
    const struct leme_public_work *work, bool *out);

#endif
