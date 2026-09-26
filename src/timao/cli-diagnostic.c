#include "timao/cli-internal.h"
#include "timao/source.h"
#include "public/json.h"
#include <string.h>

static void text(struct leme_json *json, struct leme_public_text value) {
  static const char hex[] = "0123456789abcdef";
  if (json->need_comma)
    leme_json_append(json, ",", 1);
  leme_json_append(json, "\"", 1);
  for (size_t i = 0; i < value.length && !json->failed;) {
    size_t width = 0;
    if (!timao_utf8_width(value, i, &width)) {
      leme_json_append(json, "\\ufffd", 6);
      ++i;
      continue;
    }
    const unsigned char byte = (unsigned char)value.data[i];
    if (byte == '"' || byte == '\\') {
      const char escaped[2] = {'\\', (char)byte};
      leme_json_append(json, escaped, sizeof(escaped));
    } else if (byte < 32) {
      const char escaped[6] = {'\\',           'u',           '0', '0',
                               hex[byte >> 4], hex[byte & 15]};
      leme_json_append(json, escaped, sizeof(escaped));
    } else
      leme_json_append(json, value.data + i, width);
    i += width;
  }
  leme_json_append(json, "\"", 1);
  json->need_comma = true;
}

struct source_position {
  size_t line, column;
};
static void position(struct leme_json *json, struct leme_public_text name,
                     struct source_position at) {
  leme_json_object_begin(json);
  leme_json_key(json, "name");
  text(json, name);
  leme_json_key(json, "line");
  leme_json_integer(json, at.line > INT64_MAX ? INT64_MAX : (int64_t)at.line);
  leme_json_key(json, "column");
  leme_json_integer(json,
                    at.column > INT64_MAX ? INT64_MAX : (int64_t)at.column);
  leme_json_object_end(json);
}

static bool sanitize(struct leme_json *json) {
  size_t extra = 0;
  for (size_t i = 0; i < json->length; ++i) {
    const unsigned char byte = (unsigned char)json->data[i];
    size_t growth = 0;
    if (byte == 127)
      growth = 5;
    else if (byte == 0xc2 && json->length - i > 1 &&
             (unsigned char)json->data[i + 1] >= 0x80 &&
             (unsigned char)json->data[i + 1] <= 0x9f) {
      growth = 4;
      ++i;
    }
    if (growth > json->capacity - json->length - extra - 1)
      return false;
    extra += growth;
  }
  size_t from = json->length, to = from + extra;
  json->data[to] = '\0';
  json->length = to;
  static const char hex[] = "0123456789abcdef";
  while (from != 0) {
    const unsigned char byte = (unsigned char)json->data[from - 1];
    size_t width = byte == 127 ? 1 : 0;
    if (byte >= 0x80 && byte <= 0x9f && from > 1 &&
        (unsigned char)json->data[from - 2] == 0xc2)
      width = 2;
    if (width != 0) {
      const char escape[6] = {'\\',           'u',           '0', '0',
                              hex[byte >> 4], hex[byte & 15]};
      to -= sizeof(escape);
      from -= width;
      memcpy(json->data + to, escape, sizeof(escape));
    } else
      json->data[--to] = json->data[--from];
  }
  return true;
}

bool timao_cli_diagnostic(const struct timao_diagnostic *error,
                          enum timao_output_mode format, char *buffer,
                          size_t capacity, struct timao_output_buffer *out) {
  if (out == NULL)
    return false;
  *out = (struct timao_output_buffer){0};
  if (error == NULL || buffer == NULL || capacity < 2 ||
      (format != TIMAO_OUTPUT_JSON && format != TIMAO_OUTPUT_RAW &&
       format != TIMAO_OUTPUT_HUMAN))
    return false;
  struct leme_json json = {0};
  leme_json_init_fixed(&json, buffer, capacity);
  leme_json_object_begin(&json);
  leme_json_key(&json, "code");
  text(&json, (struct leme_public_text){
                  error->code, strnlen(error->code, sizeof(error->code))});
  leme_json_key(&json, "message");
  text(&json,
       (struct leme_public_text){
           error->message, strnlen(error->message, sizeof(error->message))});
  const struct leme_public_text name = timao_diagnostic_name(error);
  if (name.length != 0) {
    size_t line = 0, column = 0;
    timao_diagnostic_position(error, &line, &column);
    leme_json_key(&json, "source");
    position(&json, name,
             (struct source_position){.line = line, .column = column});
  }
  if (error->payload != NULL) {
    leme_json_key(&json, "details");
    if (leme_public_write_json(error->payload, &json) != LEME_PUBLIC_OK)
      return false;
  }
  if (error->call_count != 0) {
    leme_json_key(&json, "calls");
    leme_json_array_begin(&json);
    for (size_t i = 0; i < error->call_count && i < 16; ++i) {
      const struct timao_call_site *call = &error->calls[i];
      size_t line = 0, column = 0;
      timao_source_position(call->source, call->span.begin, &line, &column);
      position(&json, call->source->input.name,
               (struct source_position){.line = line, .column = column});
    }
    leme_json_array_end(&json);
    leme_json_key(&json, "calls_truncated");
    leme_json_bool(&json, error->calls_truncated);
  }
  leme_json_object_end(&json);
  leme_json_append(&json, "\n", 1);
  if (json.failed || !sanitize(&json))
    return false;
  out->data = buffer;
  out->length = json.length;
  return true;
}
