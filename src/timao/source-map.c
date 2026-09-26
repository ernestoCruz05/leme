#include "timao/lower-internal.h"
#include <string.h>

static bool prefix(struct leme_public_text path, size_t offset,
                   const char *text, size_t length) {
  return offset <= path.length && length <= path.length - offset &&
         memcmp(path.data + offset, text, length) == 0;
}
bool timao_lowered_span(const struct timao_lowered *lowered,
                        struct leme_public_text path, struct timao_span *out) {
  if (lowered == NULL || out == NULL || path.data == NULL ||
      path.length > 1024 || !prefix(path, 0, "/expr", 5) ||
      (path.length > 5 && path.data[5] != '/'))
    return false;
  const struct timao_lowered_owner *owner = lowered->owner;
  if (owner->count == 0)
    return false;
  for (size_t i = 0; i < path.length; ++i) {
    if (path.data[i] == '~' &&
        (i + 1 == path.length ||
         (path.data[i + 1] != '0' && path.data[i + 1] != '1')))
      return false;
    if (path.data[i] == '~')
      ++i;
  }
  size_t index = 0, position = 5;
  while (position < path.length) {
    const enum timao_token_kind kind = owner->map[index].kind;
    if (kind != TIMAO_TOKEN_OPEN) {
      const char *name = kind == TIMAO_TOKEN_FIELD ? "/field" : "/literal";
      const size_t length = kind == TIMAO_TOKEN_FIELD ? 6 : 8;
      if (!prefix(path, position, name, length) ||
          (path.length > position + length &&
           path.data[position + length] != '/'))
        return false;
      *out = owner->map[index].span;
      return true;
    }
    if (prefix(path, position, "/call", 5) && path.length == position + 5) {
      *out = owner->map[index].span;
      return true;
    }
    if (!prefix(path, position, "/args/", 6))
      return false;
    position += 6;
    if (position == path.length || path.data[position] < '0' ||
        path.data[position] > '9')
      return false;
    size_t argument = 0, digits = 0;
    const bool zero = path.data[position] == '0';
    while (position < path.length && path.data[position] != '/') {
      const unsigned char digit = (unsigned char)path.data[position];
      ++digits;
      if (digit < '0' || digit > '9' || digits > 4 || (zero && digits > 1))
        return false;
      argument = argument * 10 + (size_t)(digit - '0');
      ++position;
    }
    size_t child = owner->count;
    for (size_t i = index + 1; i < owner->count; ++i)
      if (owner->map[i].parent == index && owner->map[i].argument == argument) {
        child = i;
        break;
      }
    if (child == owner->count)
      return false;
    index = child;
  }
  *out = owner->map[index].span;
  return true;
}
