#ifndef LEME_CONTROL_NUMBER_H
#define LEME_CONTROL_NUMBER_H

#include "public/value.h"

#include <stdbool.h>
#include <stddef.h>

enum leme_public_status leme_control_parse_number_checked(
    struct leme_public_text raw, const struct leme_public_allocator *allocator,
    const struct leme_public_work *work, double *out);

bool leme_control_parse_number(const char *raw, size_t len, double *out_value);

#endif
