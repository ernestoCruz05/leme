#include "timao/source.h"
#include "timao/diagnostic.h"
#include <stdlib.h>
#include <string.h>

bool timao_utf8_width(struct leme_public_text text, size_t offset,
                      size_t *width) {
  if (offset >= text.length || text.data == NULL || width == NULL)
    return false;
  const unsigned char c = (unsigned char)text.data[offset];
  if (c < 0x80) {
    *width = 1;
    return true;
  }
  const size_t count = c >= 0xc2 && c <= 0xdf   ? 2
                       : c >= 0xe0 && c <= 0xef ? 3
                       : c >= 0xf0 && c <= 0xf4 ? 4
                                                : 0;
  if (count == 0 || count > text.length - offset)
    return false;
  uint32_t point = c & (count == 2 ? 0x1fu : count == 3 ? 0x0fu : 0x07u);
  for (size_t i = 1; i < count; ++i) {
    const unsigned char next = (unsigned char)text.data[offset + i];
    if ((next & 0xc0u) != 0x80u)
      return false;
    point = (point << 6) | (next & 0x3fu);
  }
  if ((count == 2 && point < 0x80) || (count == 3 && point < 0x800) ||
      (count == 4 && point < 0x10000) || point > 0x10ffff ||
      (point >= 0xd800 && point <= 0xdfff))
    return false;
  *width = count;
  return true;
}

enum timao_status timao_source_check(const struct timao_source *source,
                                     struct timao_diagnostic *error) {
  if (source->input.cancelled != NULL &&
      source->input.cancelled(source->input.cancel_context))
    return timao_error(error, "cancelled", "source processing cancelled");
  return TIMAO_OK;
}

enum timao_status timao_source_create(struct leme_public_budget *account,
                                      const struct timao_limits *limits,
                                      const struct timao_input *input,
                                      struct timao_source **out,
                                      struct timao_diagnostic *error) {
  if (out == NULL)
    return timao_error(error, "invalid_argument", "missing source output");
  *out = NULL;
  if (account == NULL || limits == NULL || input == NULL ||
      (input->bytes.length != 0 && input->bytes.data == NULL) ||
      (input->name.length != 0 && input->name.data == NULL))
    return timao_error(error, "invalid_argument", "invalid source input");
  if (input->bytes.length > limits->source_bytes ||
      input->name.length > 1048576)
    return timao_error(error, "resource_limit", "source exceeds byte limit");
  struct timao_source check = {.input = *input};
  size_t lines = 1;
  for (size_t pos = 0; pos < input->bytes.length;) {
    if (timao_source_check(&check, error) != TIMAO_OK)
      return TIMAO_ERROR;
    size_t width = 0;
    if (input->bytes.data[pos] == '\0' ||
        !timao_utf8_width(input->bytes, pos, &width)) {
      timao_error(error, "syntax_error", "invalid UTF-8 or raw NUL in source");
      if (error != NULL)
        error->span = (struct timao_span){pos, pos + 1};
      return TIMAO_ERROR;
    }
    if (input->bytes.data[pos] == '\n')
      ++lines;
    pos += width;
  }
  if (lines > SIZE_MAX / sizeof(size_t))
    return timao_error(error, "resource_limit", "source line table overflow");
  struct timao_source *source =
      timao_memory_alloc(account, sizeof(*source), error);
  if (source == NULL)
    return TIMAO_ERROR;
  source->account = account;
  source->references = 1;
  source->input = *input;
  source->input.bytes = (struct leme_public_text){0};
  source->input.name = (struct leme_public_text){0};
  char *bytes = timao_memory_alloc(account, input->bytes.length + 1, error);
  if (bytes == NULL)
    goto fail;
  source->bytes_storage = bytes;
  source->input.bytes = (struct leme_public_text){bytes, input->bytes.length};
  for (size_t p = 0; p < input->bytes.length;) {
    if (timao_source_check(source, error) != TIMAO_OK)
      goto fail;
    const size_t count =
        input->bytes.length - p > 128 ? 128 : input->bytes.length - p;
    memcpy(bytes + p, input->bytes.data + p, count);
    p += count;
  }
  char *name = timao_memory_alloc(account, input->name.length + 1, error);
  if (name == NULL)
    goto fail;
  source->name_storage = name;
  source->input.name = (struct leme_public_text){name, input->name.length};
  for (size_t p = 0; p < input->name.length;) {
    if (timao_source_check(source, error) != TIMAO_OK)
      goto fail;
    const size_t count =
        input->name.length - p > 128 ? 128 : input->name.length - p;
    memcpy(name + p, input->name.data + p, count);
    p += count;
  }
  source->lines = timao_memory_alloc(account, lines * sizeof(size_t), error);
  if (source->lines == NULL)
    goto fail;
  source->line_count = 1;
  for (size_t p = 0; p < input->bytes.length; ++p) {
    if (p % 128 == 0 && timao_source_check(source, error) != TIMAO_OK)
      goto fail;
    if (bytes[p] == '\n')
      source->lines[source->line_count++] = p + 1;
  }
  *out = source;
  return TIMAO_OK;
fail:
  timao_source_unref(source);
  return TIMAO_ERROR;
}

void timao_source_ref(struct timao_source *source) {
  if (source == NULL)
    return;
  if (source->references == SIZE_MAX)
    abort();
  ++source->references;
}
void timao_source_unref(struct timao_source *source) {
  if (source == NULL || --source->references != 0)
    return;
  timao_memory_free(source->lines);
  timao_memory_free(source->name_storage);
  timao_memory_free(source->bytes_storage);
  timao_memory_free(source);
}
static void store_coordinate(size_t *output, size_t value) {
  if (output != NULL)
    *output = value;
}
void timao_source_position(const struct timao_source *source, size_t offset,
                           size_t *line, size_t *column) {
  if (offset > source->input.bytes.length)
    offset = source->input.bytes.length;
  size_t low = 0, high = source->line_count;
  while (low + 1 < high) {
    const size_t mid = low + (high - low) / 2;
    if (source->lines[mid] <= offset)
      low = mid;
    else
      high = mid;
  }
  size_t scalar_column = 1;
  for (size_t pos = source->lines[low]; pos < offset; ++pos)
    if (((unsigned char)source->input.bytes.data[pos] & 0xc0u) != 0x80u)
      ++scalar_column;
  store_coordinate(line, low + 1);
  store_coordinate(column, scalar_column);
}
