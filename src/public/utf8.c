#include "public/value-internal.h"

#include <stdalign.h>
#include <string.h>

static size_t sequence_length(const unsigned char *input, size_t length,
                              bool *valid) {
  *valid = false;
  const unsigned char lead = input[0];
  if (lead < 0x80) {
    *valid = true;
    return 1;
  }
  size_t width = 0;
  if (lead >= 0xc2 && lead <= 0xdf)
    width = 2;
  else if (lead >= 0xe0 && lead <= 0xef)
    width = 3;
  else if (lead >= 0xf0 && lead <= 0xf4)
    width = 4;
  else
    return 1;
  for (size_t i = 1; i < width; ++i) {
    if (i >= length)
      return i;
    const unsigned char byte = input[i];
    if (byte < 0x80 || byte > 0xbf)
      return i;
    if (i == 1 &&
        ((lead == 0xe0 && byte < 0xa0) || (lead == 0xed && byte > 0x9f) ||
         (lead == 0xf0 && byte < 0x90) || (lead == 0xf4 && byte > 0x8f)))
      return 1;
  }
  *valid = true;
  return width;
}

enum leme_public_status leme_public_utf8_copy(struct leme_public_builder *b,
                                              struct leme_public_text input,
                                              bool repair,
                                              struct leme_public_text *out) {
  if (out == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  *out = (struct leme_public_text){0};
  enum leme_public_status status = leme_public_mutable(b);
  if (status != LEME_PUBLIC_OK)
    return status;
  if (input.data == NULL && input.length != 0)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  if (input.length >= b->maximum)
    return leme_public_fail(b, LEME_PUBLIC_LIMIT);
  if (b->work.step != NULL && input.length > 0) {
    status = b->work.step(b->work.context, input.length);
    if (status != LEME_PUBLIC_OK)
      return leme_public_fail(b, status);
  }
  const unsigned char *source = (const unsigned char *)input.data;
  size_t length = 0;
  for (size_t offset = 0; offset < input.length;) {
    bool valid = false;
    const size_t consumed =
        sequence_length(&source[offset], input.length - offset, &valid);
    if (!valid && !repair)
      return leme_public_fail(b, LEME_PUBLIC_INVALID);
    const size_t produced = valid ? consumed : 3;
    if (produced >= b->maximum - length)
      return leme_public_fail(b, LEME_PUBLIC_LIMIT);
    length += produced;
    offset += consumed;
  }
  char *data = leme_public_allocate(b, length + 1, sizeof(char), alignof(char));
  if (data == NULL)
    return leme_public_builder_status(b);
  size_t written = 0;
  for (size_t offset = 0; offset < input.length;) {
    bool valid = false;
    const size_t consumed =
        sequence_length(&source[offset], input.length - offset, &valid);
    const size_t produced = valid ? consumed : 3;
    if ((!valid && !repair) || produced > length - written) {
      return leme_public_fail(b, LEME_PUBLIC_INVALID);
    }
    if (valid)
      memcpy(&data[written], &source[offset], consumed);
    else
      memcpy(&data[written], "\xef\xbf\xbd", 3);
    written += produced;
    offset += consumed;
  }
  data[written] = '\0';
  *out = (struct leme_public_text){.data = data, .length = written};
  return LEME_PUBLIC_OK;
}
