#ifndef LEME_CONTROL_VALIDATE_H
#define LEME_CONTROL_VALIDATE_H

#include "control/error.h"
#include "control/registry.h"
#include "control/types.h"
#include "public/schema.h"

const struct leme_public_field *
leme_control_schema_find_field(const struct leme_public_schema *schema,
                               struct leme_public_text name);

const struct leme_public_schema *
leme_control_schema_follow_ref(const struct leme_public_schema *schema);

enum leme_control_code
leme_control_resolve_field_path(const struct leme_control_type *item_type,
                                const struct leme_public_text *components,
                                size_t count,
                                struct leme_control_type *out_type);

enum leme_control_code leme_control_infer_operator_result(
    const struct leme_control_operator *op,
    const struct leme_control_type *arg_types, size_t arg_count,
    struct leme_control_type *out_type);

bool leme_control_is_valid_action_target(
    const struct leme_control_operator *op, size_t arg_index,
    const struct leme_control_type *target_type);

#endif
