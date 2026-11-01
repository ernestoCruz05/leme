#include "control/number.h"

#include <locale.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct number_scan {
  struct leme_public_text raw;
  size_t pos;
  size_t digits;
  size_t integers;
  size_t first;
  char significant[16];
  size_t significant_count;
  bool tail_nonzero;
};

static bool digit(char c) { return c >= '0' && c <= '9'; }

static void mantissa_digit(struct number_scan *s) {
  const char c = s->raw.data[s->pos];
  if (s->first == SIZE_MAX && c != '0')
    s->first = s->digits;
  if (s->first != SIZE_MAX) {
    if (s->significant_count < sizeof(s->significant))
      s->significant[s->significant_count++] = c;
    else if (c != '0')
      s->tail_nonzero = true;
  }
  ++s->digits;
  ++s->pos;
}

enum leme_public_status
leme_control_parse_number_checked(struct leme_public_text raw, double *out) {
  if (raw.data == NULL || raw.length == 0 || out == NULL)
    return LEME_PUBLIC_INVALID;
  if (raw.length > (size_t)INT64_MAX - 1024 || raw.length == SIZE_MAX)
    return LEME_PUBLIC_LIMIT;
  struct number_scan s = {.raw = raw, .first = SIZE_MAX};
  if (raw.data[s.pos] == '-')
    ++s.pos;
  if (s.pos == raw.length || !digit(raw.data[s.pos]))
    return LEME_PUBLIC_INVALID;
  const bool leading_zero = raw.data[s.pos] == '0';
  do {
    if (leading_zero && s.digits != 0)
      return LEME_PUBLIC_INVALID;
    mantissa_digit(&s);
  } while (s.pos < raw.length && digit(raw.data[s.pos]));
  s.integers = s.digits;
  if (s.pos < raw.length && raw.data[s.pos] == '.') {
    ++s.pos;
    if (s.pos == raw.length || !digit(raw.data[s.pos]))
      return LEME_PUBLIC_INVALID;
    do {
      mantissa_digit(&s);
    } while (s.pos < raw.length && digit(raw.data[s.pos]));
  }
  int64_t exponent = 0;
  if (s.pos < raw.length &&
      (raw.data[s.pos] == 'e' || raw.data[s.pos] == 'E')) {
    ++s.pos;
    bool negative = false;
    if (s.pos < raw.length &&
        (raw.data[s.pos] == '+' || raw.data[s.pos] == '-')) {
      negative = raw.data[s.pos] == '-';
      ++s.pos;
    }
    if (s.pos == raw.length || !digit(raw.data[s.pos]))
      return LEME_PUBLIC_INVALID;
    const int64_t cap = (int64_t)raw.length + 400;
    do {
      const int64_t d = raw.data[s.pos] - '0';
      exponent = exponent > (cap - d) / 10 ? cap : exponent * 10 + d;
      ++s.pos;
    } while (s.pos < raw.length && digit(raw.data[s.pos]));
    if (negative)
      exponent = -exponent;
  }
  if (s.pos != raw.length)
    return LEME_PUBLIC_INVALID;
  if (s.first == SIZE_MAX) {
    *out = 0.0;
    return LEME_PUBLIC_OK;
  }
  const int64_t position = (int64_t)s.integers - (int64_t)s.first;
  if (exponent > 16 - position || exponent < -350 - position)
    return LEME_PUBLIC_INVALID;
  if (position + exponent == 16) {
    for (size_t i = s.significant_count; i < sizeof(s.significant); ++i)
      s.significant[i] = '0';
    const int order = memcmp(s.significant, "9007199254740991", 16);
    if (order > 0 || (order == 0 && s.tail_nonzero))
      return LEME_PUBLIC_INVALID;
  }
  char *buffer = malloc(raw.length + 1);
  if (buffer == NULL)
    return LEME_PUBLIC_OOM;
  enum leme_public_status status = LEME_PUBLIC_OK;
  memcpy(buffer, raw.data, raw.length);
  buffer[raw.length] = '\0';
  const locale_t locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
  if (locale == (locale_t)0) {
    status = LEME_PUBLIC_OOM;
    goto done;
  }
  const locale_t previous = uselocale(locale);
  if (previous == (locale_t)0) {
    freelocale(locale);
    status = LEME_PUBLIC_INVALID;
    goto done;
  }
  char *end = NULL;
  const double value = strtod(buffer, &end);
  const locale_t restored = uselocale(previous);
  if (restored == (locale_t)0)
    abort();
  freelocale(locale);
  if (end != buffer + raw.length || !isfinite(value) || value == 0.0 ||
      fabs(value) > 9007199254740991.0) {
    status = LEME_PUBLIC_INVALID;
    goto done;
  }
  *out = value;
done:
  free(buffer);
  return status;
}

bool leme_control_parse_number(const char *raw, size_t len, double *out_value) {
  return leme_control_parse_number_checked((struct leme_public_text){raw, len},
                                           out_value) == LEME_PUBLIC_OK;
}
