#ifndef LEME_CONTROL_EVAL_H
#define LEME_CONTROL_EVAL_H

#include "control/control.h"
#include "control/error.h"
#include "control/expr.h"
#include "public/model.h"
#include "public/value.h"

struct leme_control_evaluation;

enum leme_control_code leme_control_evaluate(
    struct leme_control_context *context,
    const struct leme_control_program *program,
    const struct leme_public_snapshot *snapshot,
    struct leme_control_evaluation **out, struct leme_control_error *error);

enum leme_control_code
leme_control_evaluate_work(struct leme_control_context *context,
                           const struct leme_control_program *program,
                           const struct leme_public_snapshot *snapshot,
                           struct leme_control_meter *meter,
                           struct leme_control_evaluation **out,
                           struct leme_control_error *error);

const struct leme_public_value *
leme_control_evaluation_value(const struct leme_control_evaluation *evaluation);

void leme_control_evaluation_destroy(struct leme_control_evaluation *evaluation);

#endif
