#ifndef LEME_CONTROL_COMMAND_H
#define LEME_CONTROL_COMMAND_H

#include "control/action.h"
#include "control/control.h"
#include "control/error.h"
#include "public/identity.h"
#include "public/model.h"
#include "public/value.h"

#include <stddef.h>

struct leme_control_intent_batch {
  struct leme_control_intent *intents;
  size_t count;
  struct leme_public_builder *builder;
};

enum leme_control_code leme_control_command_lower(
    struct leme_control_context *context,
    const struct leme_public_snapshot *snapshot, struct leme_public_text name,
    const struct leme_public_value *argv, struct leme_control_intent_batch *out,
    struct leme_control_error *error);

void leme_control_intent_batch_destroy(struct leme_public_budget *account,
                                       struct leme_control_intent_batch *batch);

#endif
