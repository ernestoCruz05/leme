#ifndef LEME_JSON_H
#define LEME_JSON_H

#include <stdbool.h>
#include <stddef.h>

struct leme_json {
    char *data;
    size_t length;
    size_t capacity;
    bool failed;
    bool need_comma;
};

void leme_json_init(struct leme_json *json);
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
