#ifndef LEME_CONTROL_SOURCE_H
#define LEME_CONTROL_SOURCE_H

#include "control/error.h"
#include "control/limits.h"
#include "public/budget.h"
#include "public/value.h"

enum leme_control_code leme_control_source_read(
    struct leme_public_budget *account, struct leme_public_text source,
    const struct leme_control_limits *limits,
    struct leme_public_builder **owner, const struct leme_public_value **out,
    struct leme_control_error *error);

#endif
