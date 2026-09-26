#ifndef LEME_CONTROL_ACTION_INTERNAL_H
#define LEME_CONTROL_ACTION_INTERNAL_H

#include "control/action.h"
#include "control/control.h"
#include "control/error.h"
#include "control/expr.h"
#include "public/budget.h"
#include "public/model.h"
#include "public/value.h"

struct leme_control_plan {
  struct leme_control_context *context;
  struct leme_public_budget *account;
  const struct leme_control_domain *domain;
  struct leme_public_text selection_revision;
  char revision_buf[64];
  struct leme_control_intent *intents;
  enum leme_control_outcome *outcomes;
  size_t count;
  size_t affected;
  bool effects_applied;
  bool executed;
  struct leme_control_prepared *prepared;
  struct leme_public_builder *args_builder;
};

enum leme_control_code leme_control_normalize_targets(
    struct leme_control_context *context,
    const struct leme_control_program *program,
    const struct leme_public_snapshot *snapshot,
    struct leme_control_intent **out_intents, size_t *out_count,
    struct leme_public_builder **out_builder, struct leme_control_error *error);

void leme_control_intents_destroy(struct leme_control_intent *intents,
                                  size_t count);

#endif
