#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "ipc/json.h"
#include "public/budget.h"

#include <assert.h>
#include <errno.h>
#include <float.h>
#include <inttypes.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void leme_json_fail(struct leme_json *json, enum leme_json_error error) {
  if (json->failed)
    return;
  json->failed = true;
  json->error = error;
}

bool leme_json_reserve(struct leme_json *json, size_t extra) {
  if (json->failed)
    return false;
  if (json->charge != NULL) {
    if (!json->charge(json->meter, 1)) {
      leme_json_fail(json, LEME_JSON_ERROR_LIMIT);
      return false;
    }
  }
  if (json->limit == SIZE_MAX || json->length > json->limit ||
      extra > json->limit - json->length) {
    leme_json_fail(json, LEME_JSON_ERROR_LIMIT);
    return false;
  }
  const size_t needed = json->length + extra + 1;
  if (needed <= json->capacity)
    return true;
  if (json->fixed) {
    leme_json_fail(json, LEME_JSON_ERROR_LIMIT);
    return false;
  }
  const size_t maximum = json->limit + 1;
  size_t capacity = json->capacity == 0 ? 128 : json->capacity;
  if (capacity > maximum)
    capacity = maximum;
  while (capacity < needed) {
    capacity = capacity > maximum / 2 ? maximum : capacity * 2;
  }
  if (json->account != NULL) {
    if (capacity > json->reserved) {
      const enum leme_public_status status =
          leme_public_budget_reserve(json->account, capacity);
      if (status == LEME_PUBLIC_LIMIT) {
        leme_json_fail(json, LEME_JSON_ERROR_LIMIT);
        return false;
      }
      if (status != LEME_PUBLIC_OK) {
        leme_json_fail(json, LEME_JSON_ERROR_OOM);
        return false;
      }
      char *data = malloc(capacity);
      if (data == NULL) {
        leme_public_budget_release(json->account, capacity);
        leme_json_fail(json, LEME_JSON_ERROR_OOM);
        return false;
      }
      if (json->length != 0) {
        memcpy(data, json->data, json->length);
      }
      data[json->length] = '\0';
      free(json->data);
      leme_public_budget_release(json->account, json->reserved);
      json->reserved = capacity;
      json->data = data;
      json->capacity = capacity;
      return true;
    }
  }
  char *data = realloc(json->data, capacity);
  if (data == NULL) {
    leme_json_fail(json, LEME_JSON_ERROR_OOM);
    return false;
  }
  json->data = data;
  json->capacity = capacity;
  return true;
}

void leme_json_append(struct leme_json *json, const char *text,
                      size_t length) {
  if (!leme_json_reserve(json, length))
    return;
  if (length != 0)
    memcpy(json->data + json->length, text, length);
  json->length += length;
  json->data[json->length] = '\0';
}

static void leme_json_separate(struct leme_json *json) {
  if (json->need_comma)
    leme_json_append(json, ",", 1);
}

void leme_json_init_limit(struct leme_json *json, size_t limit) {
  *json = (struct leme_json){.limit = limit};
  if (limit == SIZE_MAX)
    leme_json_fail(json, LEME_JSON_ERROR_LIMIT);
}

void leme_json_init_budget(struct leme_json *json,
                           struct leme_public_budget *account, size_t limit) {
  *json = (struct leme_json){
      .account = account,
      .limit = limit,
  };
  if (limit == SIZE_MAX)
    leme_json_fail(json, LEME_JSON_ERROR_LIMIT);
}

void leme_json_set_meter(struct leme_json *json, void *meter,
                         bool (*charge)(void *meter, size_t units)) {
  if (json != NULL) {
    json->meter = meter;
    json->charge = charge;
  }
}

void leme_json_init(struct leme_json *json) {
  leme_json_init_limit(json, SIZE_MAX - 1);
}

void leme_json_init_fixed(struct leme_json *json, char *buffer, size_t capacity) {
  *json = (struct leme_json){
      .data = buffer,
      .capacity = capacity,
      .limit = (capacity > 0) ? (capacity - 1) : 0,
      .fixed = true,
  };
  if (buffer == NULL || capacity == 0)
    leme_json_fail(json, LEME_JSON_ERROR_LIMIT);
  else
    buffer[0] = '\0';
}

void leme_json_finish(struct leme_json *json) {
  if (!json->fixed && json->data != NULL) {
    free(json->data);
    if (json->account != NULL && json->reserved > 0) {
      leme_public_budget_release(json->account, json->reserved);
    }
  }
  leme_json_init(json);
}

void leme_json_object_begin(struct leme_json *json) {
  leme_json_separate(json);
  leme_json_append(json, "{", 1);
  json->need_comma = false;
}

void leme_json_object_end(struct leme_json *json) {
  leme_json_append(json, "}", 1);
  json->need_comma = true;
}

void leme_json_array_begin(struct leme_json *json) {
  leme_json_separate(json);
  leme_json_append(json, "[", 1);
  json->need_comma = false;
}

void leme_json_array_end(struct leme_json *json) {
  leme_json_append(json, "]", 1);
  json->need_comma = true;
}

static void leme_json_raw_string(struct leme_json *json, const char *value,
                                 size_t length) {
  if (json->failed)
    return;
  if (value == NULL && length != 0) {
    leme_json_fail(json, LEME_JSON_ERROR_INVALID);
    return;
  }
  if (json->length > json->limit || length > json->limit - json->length ||
      json->limit - json->length - length < 2) {
    leme_json_fail(json, LEME_JSON_ERROR_LIMIT);
    return;
  }
  const unsigned char *cursor = (const unsigned char *)value;
  leme_json_append(json, "\"", 1);
  for (size_t i = 0; i < length && !json->failed; ++i) {
    switch (cursor[i]) {
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
    if (cursor[i] < 0x20) {
      char escape[7] = {0};
      const int written =
          snprintf(escape, sizeof(escape), "\\u%04x", (unsigned)cursor[i]);
      if (written < 0 || (size_t)written >= sizeof(escape)) {
        leme_json_fail(json, LEME_JSON_ERROR_INVALID);
        return;
      }
      leme_json_append(json, escape, (size_t)written);
      continue;
    }
    leme_json_append(json, (const char *)&cursor[i], 1);
  }
  leme_json_append(json, "\"", 1);
}

static bool leme_json_cstr_length(struct leme_json *json, const char *text,
                                  size_t *out) {
  *out = 0;
  if (json->failed)
    return false;
  if (text == NULL) {
    leme_json_fail(json, LEME_JSON_ERROR_INVALID);
    return false;
  }
  while (*out < json->limit && text[*out] != '\0')
    ++*out;
  if (*out == json->limit) {
    leme_json_fail(json, LEME_JSON_ERROR_LIMIT);
    return false;
  }
  return true;
}

void leme_json_key_n(struct leme_json *json, const char *text, size_t length) {
  leme_json_separate(json);
  json->need_comma = false;
  leme_json_raw_string(json, text, length);
  leme_json_append(json, ":", 1);
}

void leme_json_key(struct leme_json *json, const char *key) {
  size_t length = 0;
  if (leme_json_cstr_length(json, key, &length))
    leme_json_key_n(json, key, length);
}

void leme_json_string_n(struct leme_json *json, const char *text,
                        size_t length) {
  leme_json_separate(json);
  leme_json_raw_string(json, text, length);
  json->need_comma = true;
}

void leme_json_string(struct leme_json *json, const char *value) {
  if (value == NULL) {
    leme_json_null(json);
    return;
  }
  size_t length = 0;
  if (leme_json_cstr_length(json, value, &length))
    leme_json_string_n(json, value, length);
}

void leme_json_bool(struct leme_json *json, bool value) {
  leme_json_separate(json);
  leme_json_append(json, value ? "true" : "false", value ? 4u : 5u);
  json->need_comma = true;
}

void leme_json_integer(struct leme_json *json, int64_t value) {
  if (json->failed)
    return;
  char buffer[32] = {0};
  const int written = snprintf(buffer, sizeof(buffer), "%" PRId64, value);
  if (written < 0 || (size_t)written >= sizeof(buffer)) {
    leme_json_fail(json, LEME_JSON_ERROR_INVALID);
    return;
  }
  leme_json_separate(json);
  leme_json_append(json, buffer, (size_t)written);
  json->need_comma = true;
}

void leme_json_number(struct leme_json *json, long value) {
  leme_json_integer(json, (int64_t)value);
}

void leme_json_real(struct leme_json *json, double value) {
  if (json->failed)
    return;
  if (!isfinite(value)) {
    leme_json_fail(json, LEME_JSON_ERROR_INVALID);
    return;
  }
  const locale_t locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
  if (locale == (locale_t)0) {
    leme_json_fail(json, LEME_JSON_ERROR_OOM);
    return;
  }
  const locale_t previous = uselocale(locale);
  if (previous == (locale_t)0) {
    freelocale(locale);
    leme_json_fail(json, LEME_JSON_ERROR_INVALID);
    return;
  }
  char buffer[64] = {0};
  const int written = snprintf(buffer, sizeof(buffer), "%.*g", DBL_DECIMAL_DIG,
                               value == 0.0 ? 0.0 : value);
  const locale_t restored = uselocale(previous);
  assert(restored != (locale_t)0);
  (void)restored;
  freelocale(locale);
  if (written < 0 || (size_t)written >= sizeof(buffer)) {
    leme_json_fail(json, LEME_JSON_ERROR_INVALID);
    return;
  }
  leme_json_separate(json);
  leme_json_append(json, buffer, (size_t)written);
  json->need_comma = true;
}

void leme_json_null(struct leme_json *json) {
  leme_json_separate(json);
  leme_json_append(json, "null", 4);
  json->need_comma = true;
}

const char *leme_json_result(const struct leme_json *json) {
  return json->failed ? NULL : json->data;
}
