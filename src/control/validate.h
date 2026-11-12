#ifndef LEME_CONTROL_VALIDATE_H
#define LEME_CONTROL_VALIDATE_H

#include "control/error.h"
#include "control/registry.h"
#include "control/types.h"
#include "public/schema.h"

enum leme_control_code
leme_control_resolve_field_path(const struct leme_control_type *item_type,
                                const struct leme_public_text *components,
                                size_t count,
                                struct leme_control_type *out_type);

enum leme_control_code
leme_control_infer_operator_result(const struct leme_control_operator *op,
                                   const struct leme_control_type *arg_types,
                                   size_t arg_count,
                                   struct leme_control_type *out_type);

bool leme_control_is_valid_action_target(
    const struct leme_control_operator *op, size_t arg_index,
    const struct leme_control_type *target_type);

#endif
