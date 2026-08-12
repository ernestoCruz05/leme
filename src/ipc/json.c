#include "ipc/json.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool
leme_json_reserve(struct leme_json *json, size_t extra)
{
    size_t needed;
    size_t capacity;
    char *data;

    if (json->failed) {
        return false;
    }
    if (extra > SIZE_MAX - 1 - json->length) {
        json->failed = true;
        return false;
    }
    needed = json->length + extra + 1;
    if (needed <= json->capacity) {
        return true;
    }
    capacity = json->capacity == 0 ? 128 : json->capacity;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) {
            json->failed = true;
            return false;
        }
        capacity *= 2;
    }
    data = realloc(json->data, capacity);
    if (data == NULL) {
        json->failed = true;
        return false;
    }
    json->data = data;
    json->capacity = capacity;
    return true;
}

static void
leme_json_append(struct leme_json *json, const char *text, size_t length)
{
    if (!leme_json_reserve(json, length)) {
        return;
    }
    memcpy(json->data + json->length, text, length);
    json->length += length;
    json->data[json->length] = '\0';
}

static void
leme_json_separate(struct leme_json *json)
{
    if (json->need_comma) {
        leme_json_append(json, ",", 1);
    }
}

void
leme_json_init(struct leme_json *json)
{
    *json = (struct leme_json){0};
}

void
leme_json_finish(struct leme_json *json)
{
    free(json->data);
    *json = (struct leme_json){0};
}

void
leme_json_object_begin(struct leme_json *json)
{
    leme_json_separate(json);
    leme_json_append(json, "{", 1);
    json->need_comma = false;
}

void
leme_json_object_end(struct leme_json *json)
{
    leme_json_append(json, "}", 1);
    json->need_comma = true;
}

void
leme_json_array_begin(struct leme_json *json)
{
    leme_json_separate(json);
    leme_json_append(json, "[", 1);
    json->need_comma = false;
}

void
leme_json_array_end(struct leme_json *json)
{
    leme_json_append(json, "]", 1);
    json->need_comma = true;
}

static void
leme_json_raw_string(struct leme_json *json, const char *value)
{
    const unsigned char *cursor = (const unsigned char *)value;

    leme_json_append(json, "\"", 1);
    for (; *cursor != '\0'; cursor++) {
        char escape[7];
        int written;

        switch (*cursor) {
        case '"':
            leme_json_append(json, "\\\"", 2);
            continue;
        case '\\':
            leme_json_append(json, "\\\\", 2);
            continue;
        case '\n':
            leme_json_append(json, "\\n", 2);
            continue;
        case '\t':
            leme_json_append(json, "\\t", 2);
            continue;
        case '\r':
            leme_json_append(json, "\\r", 2);
            continue;
        case '\b':
            leme_json_append(json, "\\b", 2);
            continue;
        case '\f':
            leme_json_append(json, "\\f", 2);
            continue;
        default:
            break;
        }
        if (*cursor < 0x20) {
            written = snprintf(escape, sizeof(escape), "\\u%04x", *cursor);
            if (written < 0 || (size_t)written >= sizeof(escape)) {
                json->failed = true;
                return;
            }
            leme_json_append(json, escape, (size_t)written);
            continue;
        }
        leme_json_append(json, (const char *)cursor, 1);
    }
    leme_json_append(json, "\"", 1);
}

void
leme_json_key(struct leme_json *json, const char *key)
{
    leme_json_separate(json);
    json->need_comma = false;
    leme_json_raw_string(json, key);
    leme_json_append(json, ":", 1);
}

void
leme_json_string(struct leme_json *json, const char *value)
{
    if (value == NULL) {
        leme_json_null(json);
        return;
    }
    leme_json_separate(json);
    leme_json_raw_string(json, value);
    json->need_comma = true;
}

void
leme_json_bool(struct leme_json *json, bool value)
{
    leme_json_separate(json);
    leme_json_append(json, value ? "true" : "false", value ? 4u : 5u);
    json->need_comma = true;
}

void
leme_json_number(struct leme_json *json, long value)
{
    char buffer[32];
    int written;

    written = snprintf(buffer, sizeof(buffer), "%ld", value);
    if (written < 0 || (size_t)written >= sizeof(buffer)) {
        json->failed = true;
        return;
    }
    leme_json_separate(json);
    leme_json_append(json, buffer, (size_t)written);
    json->need_comma = true;
}

void
leme_json_null(struct leme_json *json)
{
    leme_json_separate(json);
    leme_json_append(json, "null", 4);
    json->need_comma = true;
}

const char *
leme_json_result(const struct leme_json *json)
{
    return json->failed ? NULL : json->data;
}
