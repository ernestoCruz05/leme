#ifndef LEME_JSON_H
#define LEME_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct leme_public_budget;

enum leme_json_error {
  LEME_JSON_ERROR_NONE,
  LEME_JSON_ERROR_OOM,
  LEME_JSON_ERROR_LIMIT,
  LEME_JSON_ERROR_INVALID,
};

struct leme_json {
  struct leme_public_budget *account;
  void *meter;
  bool (*charge)(void *meter, size_t units);
  size_t reserved;
  char *data;
  size_t length;
  size_t capacity;
  size_t limit;
  enum leme_json_error error;
  bool failed;
  bool need_comma;
  bool fixed;
};

void leme_json_init(struct leme_json *json);
void leme_json_init_limit(struct leme_json *json, size_t limit);
void leme_json_init_budget(struct leme_json *json,
                           struct leme_public_budget *account, size_t limit);
void leme_json_init_fixed(struct leme_json *json, char *buffer,
                          size_t capacity);
void leme_json_set_meter(struct leme_json *json, void *meter,
                         bool (*charge)(void *meter, size_t units));
void leme_json_append(struct leme_json *json, const char *text, size_t length);
bool leme_json_reserve(struct leme_json *json, size_t extra);
void leme_json_key_n(struct leme_json *json, const char *text, size_t length);
void leme_json_string_n(struct leme_json *json, const char *text,
                        size_t length);
void leme_json_integer(struct leme_json *json, int64_t value);
void leme_json_real(struct leme_json *json, double value);
void leme_json_finish(struct leme_json *json);
void leme_json_object_begin(struct leme_json *json);
void leme_json_object_end(struct leme_json *json);
void leme_json_array_begin(struct leme_json *json);
void leme_json_array_end(struct leme_json *json);
void leme_json_key(struct leme_json *json, const char *key);
void leme_json_string(struct leme_json *json, const char *value);
void leme_json_bool(struct leme_json *json, bool value);
void leme_json_number(struct leme_json *json, long value);
void leme_json_null(struct leme_json *json);
const char *leme_json_result(const struct leme_json *json);

#endif
