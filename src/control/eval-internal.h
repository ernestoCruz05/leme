#ifndef LEME_CONTROL_EVAL_INTERNAL_H
#define LEME_CONTROL_EVAL_INTERNAL_H

#include "control/control.h"
#include "control/error.h"
#include "control/eval.h"
#include "control/expr.h"
#include "control/limits.h"
#include "public/budget.h"
#include "public/model.h"
#include "public/value.h"

struct evaluator {
  struct leme_control_context *context;
  const struct leme_control_program *program;
  const struct leme_public_snapshot *snapshot;
  struct leme_public_builder *builder;
  struct leme_public_budget *account;
  const struct leme_control_limits *limits;
  struct leme_control_meter meter;
  struct leme_control_error *error;
  const struct leme_public_value *current_item;
};

enum leme_control_code eval_set_error(struct evaluator *ev, const char *path,
                                      enum leme_control_code code,
                                      const char *msg);

enum leme_control_code eval_node(struct evaluator *ev, uint32_t node_idx,
                                 const struct leme_public_value **out);

enum leme_control_code eval_scalar_call(struct evaluator *ev,
                                        const struct leme_control_node *node,
                                        const struct leme_public_value **out);

enum leme_control_code eval_lookup_call(struct evaluator *ev,
                                        const struct leme_control_node *node,
                                        const struct leme_public_value **out);

enum leme_control_code
eval_collection_call(struct evaluator *ev, const struct leme_control_node *node,
                     const struct leme_public_value **out);

bool eval_equal(struct evaluator *ev, const struct leme_public_value *left,
                const struct leme_public_value *right);

const struct leme_public_value *
eval_ensure_owned(struct evaluator *ev, const struct leme_public_value *val,
                  enum leme_control_code *out_code);

#endif
