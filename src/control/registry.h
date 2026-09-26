#ifndef LEME_CONTROL_REGISTRY_H
#define LEME_CONTROL_REGISTRY_H

#include "public/schema.h"
#include "public/value.h"

#include <stdbool.h>
#include <stddef.h>

enum leme_control_opcode {
#define CONTROL_OP(id, name, is_act, min_a, max_a, args_arr, args_cnt, res,    \
                   desc, work)                                                 \
  LEME_CONTROL_OP_##id,
#include "control/operators.def"
#undef CONTROL_OP
  LEME_CONTROL_OPERATOR_COUNT
};

enum leme_control_effect {
  LEME_CONTROL_EFFECT_PURE = 0,
  LEME_CONTROL_EFFECT_ACTION = 1
};

struct leme_control_operator {
  size_t min_args;
  size_t max_args;
  const struct leme_public_argument *arguments;
  size_t argument_count;
  struct leme_public_text name;
  struct leme_public_text result;
  struct leme_public_text description;
  struct leme_public_text work;
  enum leme_control_opcode opcode;
  enum leme_control_effect effect;
  bool available;
};

struct leme_control_registry {
  const struct leme_public_operation *operations;
  size_t count;
};

const struct leme_control_registry *leme_control_registry(void);

const struct leme_control_operator *
leme_control_operator_find(struct leme_public_text name);

const struct leme_control_operator *
leme_control_operator_by_opcode(enum leme_control_opcode opcode);

const struct leme_public_operation *
leme_control_operation_find(struct leme_public_text name);

uint32_t
leme_control_operator_roots(const struct leme_control_operator *op,
                            const struct leme_public_value *const *arg_literals,
                            size_t arg_count);

bool leme_control_operator_arg_is_item_scope(
    const struct leme_control_operator *op, size_t arg_index);

enum leme_public_status
leme_control_registry_value(struct leme_public_builder *builder,
                            struct leme_public_value **out);

#endif
