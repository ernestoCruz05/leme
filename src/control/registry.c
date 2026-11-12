#include "control/registry.h"

#include <string.h>

#define STATIC_TEXT(s) {.data = (s), .length = sizeof(s) - 1u}

static const struct leme_control_operator operators[] = {
#define CONTROL_OP(id, name_str, is_act, min_a, max_a)                         \
  [LEME_CONTROL_OP_##id] = {                                                   \
      .opcode = LEME_CONTROL_OP_##id,                                          \
      .name = STATIC_TEXT(name_str),                                           \
      .effect =                                                                \
          (is_act) ? LEME_CONTROL_EFFECT_ACTION : LEME_CONTROL_EFFECT_PURE,    \
      .min_args = (min_a),                                                     \
      .max_args = (max_a),                                                     \
  },
#include "control/operators.def"
#undef CONTROL_OP
};

const struct leme_control_operator *
leme_control_operator_find(struct leme_public_text name) {
  if (name.data == NULL)
    return NULL;
  for (size_t i = 0; i < LEME_CONTROL_OPERATOR_COUNT; ++i) {
    if (operators[i].name.length == name.length &&
        memcmp(operators[i].name.data, name.data, name.length) == 0)
      return &operators[i];
  }
  return NULL;
}

uint32_t
leme_control_operator_roots(const struct leme_control_operator *op,
                            const struct leme_public_value *const *arg_literals,
                            size_t arg_count) {
  if (op == NULL)
    return 0;
  switch (op->opcode) {
  case LEME_CONTROL_OP_VIEWS:
  case LEME_CONTROL_OP_VIEW:
  case LEME_CONTROL_OP_FOCUS:
  case LEME_CONTROL_OP_SET_MINIMIZED:
  case LEME_CONTROL_OP_SET_FLOATING:
  case LEME_CONTROL_OP_SET_FULLSCREEN:
  case LEME_CONTROL_OP_SET_STICKY:
  case LEME_CONTROL_OP_RESIZE:
  case LEME_CONTROL_OP_CONTAINER_LAYOUT:
  case LEME_CONTROL_OP_CONTAINER_DETACH:
  case LEME_CONTROL_OP_CONTAINER_DISSOLVE:
  case LEME_CONTROL_OP_CONTAINER_JOIN:
  case LEME_CONTROL_OP_CLOSE:
    return LEME_PUBLIC_ROOT_BIT(LEME_PUBLIC_VIEWS);
  case LEME_CONTROL_OP_TAGS:
  case LEME_CONTROL_OP_FOCUS_TAG:
  case LEME_CONTROL_OP_SET_LAYOUT:
    return LEME_PUBLIC_ROOT_BIT(LEME_PUBLIC_TAGS);
  case LEME_CONTROL_OP_TAG:
    return LEME_PUBLIC_ROOT_BIT(LEME_PUBLIC_TAGS) |
           LEME_PUBLIC_ROOT_BIT(LEME_PUBLIC_OUTPUTS);
  case LEME_CONTROL_OP_OUTPUTS:
  case LEME_CONTROL_OP_OUTPUT:
  case LEME_CONTROL_OP_FOCUS_OUTPUT:
  case LEME_CONTROL_OP_CONFIGURE_OUTPUT:
  case LEME_CONTROL_OP_SET_OUTPUT_POWER:
    return LEME_PUBLIC_ROOT_BIT(LEME_PUBLIC_OUTPUTS);
  case LEME_CONTROL_OP_INPUTS:
  case LEME_CONTROL_OP_INPUT:
  case LEME_CONTROL_OP_SET_INPUT:
    return LEME_PUBLIC_ROOT_BIT(LEME_PUBLIC_INPUTS);
  case LEME_CONTROL_OP_SESSION:
  case LEME_CONTROL_OP_SET_MODE:
    return LEME_PUBLIC_ROOT_BIT(LEME_PUBLIC_SESSION);
  case LEME_CONTROL_OP_CONFIG:
  case LEME_CONTROL_OP_SET_CONFIG:
  case LEME_CONTROL_OP_RELOAD_CONFIG:
    return LEME_PUBLIC_ROOT_BIT(LEME_PUBLIC_CONFIG);
  case LEME_CONTROL_OP_RUNTIME:
    return LEME_PUBLIC_ROOT_BIT(LEME_PUBLIC_RUNTIME);
  case LEME_CONTROL_OP_STATUS:
    return LEME_PUBLIC_ROOT_BIT(LEME_PUBLIC_STATUS);
  case LEME_CONTROL_OP_SET_KEYBOARD_LAYOUT:
    return LEME_PUBLIC_ROOT_BIT(LEME_PUBLIC_INPUTS) |
           LEME_PUBLIC_ROOT_BIT(LEME_PUBLIC_SESSION);
  case LEME_CONTROL_OP_MOVE_TO_TAG:
    return LEME_PUBLIC_ROOT_BIT(LEME_PUBLIC_VIEWS) |
           LEME_PUBLIC_ROOT_BIT(LEME_PUBLIC_TAGS);
  case LEME_CONTROL_OP_MOVE_TO_OUTPUT:
    return LEME_PUBLIC_ROOT_BIT(LEME_PUBLIC_VIEWS) |
           LEME_PUBLIC_ROOT_BIT(LEME_PUBLIC_OUTPUTS);
  case LEME_CONTROL_OP_COMMAND:
    return LEME_PUBLIC_ROOT_BIT(LEME_PUBLIC_VIEWS) |
           LEME_PUBLIC_ROOT_BIT(LEME_PUBLIC_TAGS) |
           LEME_PUBLIC_ROOT_BIT(LEME_PUBLIC_OUTPUTS) |
           LEME_PUBLIC_ROOT_BIT(LEME_PUBLIC_INPUTS) |
           LEME_PUBLIC_ROOT_BIT(LEME_PUBLIC_SESSION);
  case LEME_CONTROL_OP_BY_ID: {
    if (arg_count > 0 && arg_literals != NULL && arg_literals[0] != NULL) {
      struct leme_public_text kind_text = {0};
      if (leme_public_as_text(arg_literals[0], &kind_text) == LEME_PUBLIC_OK) {
        if (kind_text.length == 4 && memcmp(kind_text.data, "view", 4) == 0)
          return LEME_PUBLIC_ROOT_BIT(LEME_PUBLIC_VIEWS);
        if (kind_text.length == 3 && memcmp(kind_text.data, "tag", 3) == 0)
          return LEME_PUBLIC_ROOT_BIT(LEME_PUBLIC_TAGS);
        if (kind_text.length == 6 && memcmp(kind_text.data, "output", 6) == 0)
          return LEME_PUBLIC_ROOT_BIT(LEME_PUBLIC_OUTPUTS);
        if (kind_text.length == 5 && memcmp(kind_text.data, "input", 5) == 0)
          return LEME_PUBLIC_ROOT_BIT(LEME_PUBLIC_INPUTS);
      }
    }
    return LEME_PUBLIC_ROOT_BIT(LEME_PUBLIC_VIEWS) |
           LEME_PUBLIC_ROOT_BIT(LEME_PUBLIC_TAGS) |
           LEME_PUBLIC_ROOT_BIT(LEME_PUBLIC_OUTPUTS) |
           LEME_PUBLIC_ROOT_BIT(LEME_PUBLIC_INPUTS);
  }
  default:
    return 0;
  }
}

bool leme_control_operator_arg_is_item_scope(
    const struct leme_control_operator *op, size_t arg_index) {
  if (op == NULL)
    return false;
  switch (op->opcode) {
  case LEME_CONTROL_OP_WHERE:
  case LEME_CONTROL_OP_MAP:
  case LEME_CONTROL_OP_SORT_BY:
    return arg_index == 1;
  case LEME_CONTROL_OP_SELECT:
    return arg_index >= 1;
  default:
    return false;
  }
}
