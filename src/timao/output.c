#include "timao/output.h"

#include <stdlib.h>
#include <string.h>

static bool printable(yyjson_val *value) {
  if (value == NULL || yyjson_is_arr(value) || yyjson_is_obj(value))
    return false;
  if (!yyjson_is_str(value))
    return true;
  const char *text = yyjson_get_str(value);
  const size_t length = yyjson_get_len(value);
  return memchr(text, '\n', length) == NULL &&
         memchr(text, '\r', length) == NULL &&
         memchr(text, '\0', length) == NULL;
}

static bool write_json(FILE *stream, yyjson_val *value) {
  size_t length = 0;
  char *text = yyjson_val_write(value, YYJSON_WRITE_NOFLAG, &length);
  if (text == NULL)
    return false;
  const bool written = fwrite(text, 1, length, stream) == length;
  free(text);
  return written && fputc('\n', stream) != EOF;
}

static bool write_raw(FILE *stream, yyjson_val *value) {
  if (!yyjson_is_str(value))
    return write_json(stream, value);
  const size_t length = yyjson_get_len(value);
  return fwrite(yyjson_get_str(value), 1, length, stream) == length &&
         fputc('\n', stream) != EOF;
}

bool timao_output_value(FILE *stream, enum timao_format format,
                        yyjson_val *value) {
  bool written = false;
  if (format == TIMAO_FORMAT_JSON)
    written = value != NULL && write_json(stream, value);
  else if (yyjson_is_arr(value)) {
    yyjson_val *item = NULL;
    yyjson_arr_iter iter = yyjson_arr_iter_with(value);
    while ((item = yyjson_arr_iter_next(&iter)) != NULL)
      if (!printable(item))
        return false;
    written = true;
    iter = yyjson_arr_iter_with(value);
    while (written && (item = yyjson_arr_iter_next(&iter)) != NULL)
      written = write_raw(stream, item);
  } else
    written = printable(value) && write_raw(stream, value);
  return fflush(stream) == 0 && written;
}
