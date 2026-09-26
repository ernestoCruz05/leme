#include "timao/output.h"
#include "public/json.h"
#include "public/value-internal.h"

#include <string.h>

static enum leme_public_status status(const struct leme_json *json) {
  if (!json->failed)
    return LEME_PUBLIC_OK;
  return json->error == LEME_JSON_ERROR_OOM     ? LEME_PUBLIC_OOM
         : json->error == LEME_JSON_ERROR_LIMIT ? LEME_PUBLIC_LIMIT
                                                : LEME_PUBLIC_INVALID;
}

static bool scalar(const struct leme_public_value *value) {
  return value != NULL && leme_public_kind(value) != LEME_PUBLIC_ARRAY &&
         leme_public_kind(value) != LEME_PUBLIC_OBJECT;
}

static enum leme_public_status
raw_scalar(struct leme_json *json, const struct leme_public_value *value) {
  if (!scalar(value))
    return LEME_PUBLIC_INVALID;
  json->need_comma = false;
  if (leme_public_kind(value) == LEME_PUBLIC_STRING) {
    struct leme_public_text text = {0};
    if (leme_public_as_text(value, &text) != LEME_PUBLIC_OK)
      return LEME_PUBLIC_INVALID;
    for (size_t i = 0; i < text.length; ++i)
      if (text.data[i] == '\n' || text.data[i] == '\r' || text.data[i] == '\0')
        return LEME_PUBLIC_INVALID;
    leme_json_append(json, text.data, text.length);
  } else {
    const enum leme_public_status result = leme_public_write_json(value, json);
    if (result != LEME_PUBLIC_OK)
      return result;
  }
  leme_json_append(json, "\n", 1);
  return status(json);
}

static void indent(struct leme_json *json, size_t count) {
  for (size_t i = 0; i < count && !json->failed; ++i)
    leme_json_append(json, "  ", 2);
}

static enum leme_public_status pretty(struct leme_json *json,
                                      const struct leme_public_value *value,
                                      size_t depth) {
  if (depth > LEME_PUBLIC_MAX_DEPTH)
    return LEME_PUBLIC_LIMIT;
  json->need_comma = false;
  if (scalar(value))
    return leme_public_write_json(value, json);
  const bool object = leme_public_kind(value) == LEME_PUBLIC_OBJECT;
  const size_t count = leme_public_length(value);
  leme_json_append(json, object ? "{" : "[", 1);
  if (count != 0)
    leme_json_append(json, "\n", 1);
  for (size_t i = 0; i < count && !json->failed; ++i) {
    indent(json, depth);
    const struct leme_public_value *child = NULL;
    if (object) {
      const struct leme_public_text key = leme_public_key_at(value, i);
      json->need_comma = false;
      leme_json_string_n(json, key.data, key.length);
      leme_json_append(json, ": ", 2);
      child = leme_public_member_at(value, i);
    } else {
      child = leme_public_at(value, i);
    }
    const enum leme_public_status result = pretty(json, child, depth + 1);
    if (result != LEME_PUBLIC_OK)
      return result;
    if (i + 1 != count)
      leme_json_append(json, ",", 1);
    leme_json_append(json, "\n", 1);
  }
  if (count != 0)
    indent(json, depth - 1);
  leme_json_append(json, object ? "}" : "]", 1);
  return status(json);
}

static bool tabular(const struct leme_public_value *value) {
  if (leme_public_kind(value) != LEME_PUBLIC_ARRAY ||
      leme_public_length(value) == 0 || leme_public_length(value) > 1024)
    return false;
  const struct leme_public_value *first = leme_public_at(value, 0);
  const size_t columns = leme_public_length(first);
  if (leme_public_kind(first) != LEME_PUBLIC_OBJECT || columns == 0 ||
      columns > 16)
    return false;
  for (size_t row = 0; row < leme_public_length(value); ++row) {
    const struct leme_public_value *item = leme_public_at(value, row);
    if (leme_public_kind(item) != LEME_PUBLIC_OBJECT ||
        leme_public_length(item) != columns)
      return false;
    for (size_t column = 0; column < columns; ++column)
      if (!scalar(leme_public_get(item, leme_public_key_at(first, column))))
        return false;
  }
  return true;
}

static enum leme_public_status table(struct leme_json *json,
                                     const struct leme_public_value *value) {
  const struct leme_public_value *first = leme_public_at(value, 0);
  const size_t columns = leme_public_length(first);
  for (size_t column = 0; column < columns; ++column) {
    if (column != 0)
      leme_json_append(json, " | ", 3);
    const struct leme_public_text key = leme_public_key_at(first, column);
    json->need_comma = false;
    leme_json_string_n(json, key.data, key.length);
  }
  leme_json_append(json, "\n", 1);
  for (size_t row = 0; row < leme_public_length(value) && !json->failed;
       ++row) {
    for (size_t column = 0; column < columns; ++column) {
      if (column != 0)
        leme_json_append(json, " | ", 3);
      json->need_comma = false;
      const enum leme_public_status result = leme_public_write_json(
          leme_public_get(leme_public_at(value, row),
                          leme_public_key_at(first, column)),
          json);
      if (result != LEME_PUBLIC_OK)
        return result;
    }
    leme_json_append(json, "\n", 1);
  }
  return status(json);
}

static size_t terminal_control(const char *data, size_t length) {
  if ((unsigned char)data[0] == 0x7f)
    return 1;
  if (length >= 2 && (unsigned char)data[0] == 0xc2 &&
      (unsigned char)data[1] >= 0x80 && (unsigned char)data[1] <= 0x9f)
    return 2;
  return 0;
}

static enum leme_public_status sanitize(struct leme_json *json) {
  size_t first = 0;
  while (first < json->length &&
         terminal_control(json->data + first, json->length - first) == 0)
    ++first;
  if (first == json->length)
    return status(json);
  struct leme_json safe = {0};
  leme_json_init_budget(&safe, json->account, json->limit);
  leme_json_append(&safe, json->data, first);
  static const char hex[] = "0123456789abcdef";
  for (size_t i = first; i < json->length && !safe.failed;) {
    const size_t bytes = terminal_control(json->data + i, json->length - i);
    if (bytes != 0) {
      const unsigned char code = (unsigned char)json->data[i + bytes - 1];
      const char escape[6] = {'\\',           'u',           '0', '0',
                              hex[code >> 4], hex[code & 15]};
      leme_json_append(&safe, escape, sizeof(escape));
      i += bytes;
    } else {
      leme_json_append(&safe, json->data + i, 1);
      ++i;
    }
  }
  const enum leme_public_status result = status(&safe);
  if (result == LEME_PUBLIC_OK) {
    leme_json_finish(json);
    *json = safe;
  } else {
    leme_json_finish(&safe);
  }
  return result;
}

enum leme_public_status timao_output_format(
    struct leme_public_budget *account, enum timao_output_mode mode,
    const struct leme_public_value *value, struct timao_output_buffer *out) {
  if (out == NULL)
    return LEME_PUBLIC_INVALID;
  *out = (struct timao_output_buffer){0};
  if (account == NULL || value == NULL || !value->owner->sealed ||
      (mode != TIMAO_OUTPUT_JSON && mode != TIMAO_OUTPUT_RAW &&
       mode != TIMAO_OUTPUT_HUMAN))
    return LEME_PUBLIC_INVALID;
  leme_public_budget_ref(account);
  struct leme_json json = {0};
  leme_json_init_budget(&json, account, 67108864);
  enum leme_public_status result = LEME_PUBLIC_OK;
  if (mode == TIMAO_OUTPUT_JSON) {
    result = leme_public_write_json(value, &json);
    leme_json_append(&json, "\n", 1);
  } else if (mode == TIMAO_OUTPUT_RAW) {
    if (leme_public_kind(value) == LEME_PUBLIC_ARRAY) {
      for (size_t i = 0;
           i < leme_public_length(value) && result == LEME_PUBLIC_OK; ++i)
        result = raw_scalar(&json, leme_public_at(value, i));
    } else {
      result = raw_scalar(&json, value);
    }
  } else {
    if (tabular(value)) {
      result = table(&json, value);
    } else {
      result = pretty(&json, value, 1);
      leme_json_append(&json, "\n", 1);
    }
    if (result == LEME_PUBLIC_OK)
      result = sanitize(&json);
  }
  if (result == LEME_PUBLIC_OK)
    result = status(&json);
  if (result != LEME_PUBLIC_OK) {
    leme_json_finish(&json);
    leme_public_budget_unref(account);
    return result;
  }
  *out = (struct timao_output_buffer){
      .data = json.data, .length = json.length, .storage = json};
  return LEME_PUBLIC_OK;
}

void timao_output_buffer_destroy(struct timao_output_buffer *buffer) {
  if (buffer == NULL)
    return;
  struct leme_public_budget *account = buffer->storage.account;
  leme_json_finish(&buffer->storage);
  leme_public_budget_unref(account);
  *buffer = (struct timao_output_buffer){0};
}
