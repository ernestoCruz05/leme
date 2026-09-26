#include "control/registry.h"

#include <string.h>

#define STATIC_TEXT(s)                                                         \
  { .data = (s), .length = sizeof(s) - 1u }

static const struct leme_public_argument args_1_string[] = {
    {STATIC_TEXT("string"), STATIC_TEXT("outer")}};

static const struct leme_public_argument args_tag[] = {
    {STATIC_TEXT("output | string"), STATIC_TEXT("outer")},
    {STATIC_TEXT("integer"), STATIC_TEXT("outer")}};

static const struct leme_public_argument args_by_id[] = {
    {STATIC_TEXT("string"), STATIC_TEXT("outer")},
    {STATIC_TEXT("string"), STATIC_TEXT("outer")}};

static const struct leme_public_argument args_where[] = {
    {STATIC_TEXT("array<T>"), STATIC_TEXT("outer")},
    {STATIC_TEXT("boolean"), STATIC_TEXT("item")}};

static const struct leme_public_argument args_select[] = {
    {STATIC_TEXT("array<T>"), STATIC_TEXT("outer")},
    {STATIC_TEXT("FIELD"), STATIC_TEXT("item")}};

static const struct leme_public_argument args_map[] = {
    {STATIC_TEXT("array<T>"), STATIC_TEXT("outer")},
    {STATIC_TEXT("any"), STATIC_TEXT("item")}};

static const struct leme_public_argument args_sort_by[] = {
    {STATIC_TEXT("array<T>"), STATIC_TEXT("outer")},
    {STATIC_TEXT("FIELD"), STATIC_TEXT("item")},
    {STATIC_TEXT("string"), STATIC_TEXT("outer")}};

static const struct leme_public_argument args_limit[] = {
    {STATIC_TEXT("array<T>"), STATIC_TEXT("outer")},
    {STATIC_TEXT("integer"), STATIC_TEXT("outer")}};

static const struct leme_public_argument args_first[] = {
    {STATIC_TEXT("array<T>"), STATIC_TEXT("outer")}};

static const struct leme_public_argument args_count[] = {
    {STATIC_TEXT("array"), STATIC_TEXT("outer")}};

static const struct leme_public_argument args_get[] = {
    {STATIC_TEXT("object | reference"), STATIC_TEXT("outer")},
    {STATIC_TEXT("string"), STATIC_TEXT("outer")}};

static const struct leme_public_argument args_1_any[] = {
    {STATIC_TEXT("any"), STATIC_TEXT("outer")}};

static const struct leme_public_argument args_contains[] = {
    {STATIC_TEXT("array"), STATIC_TEXT("outer")},
    {STATIC_TEXT("any"), STATIC_TEXT("outer")}};

static const struct leme_public_argument args_2_any[] = {
    {STATIC_TEXT("any"), STATIC_TEXT("outer")},
    {STATIC_TEXT("any"), STATIC_TEXT("outer")}};

static const struct leme_public_argument args_2_number_or_string[] = {
    {STATIC_TEXT("number | string"), STATIC_TEXT("outer")},
    {STATIC_TEXT("number | string"), STATIC_TEXT("outer")}};

static const struct leme_public_argument args_2_number[] = {
    {STATIC_TEXT("number"), STATIC_TEXT("outer")},
    {STATIC_TEXT("number"), STATIC_TEXT("outer")}};

static const struct leme_public_argument args_minus[] = {
    {STATIC_TEXT("number"), STATIC_TEXT("outer")},
    {STATIC_TEXT("number"), STATIC_TEXT("outer")}};

static const struct leme_public_argument args_1_boolean[] = {
    {STATIC_TEXT("boolean"), STATIC_TEXT("outer")}};

static const struct leme_public_argument args_if[] = {
    {STATIC_TEXT("boolean"), STATIC_TEXT("outer")},
    {STATIC_TEXT("any"), STATIC_TEXT("outer")},
    {STATIC_TEXT("any"), STATIC_TEXT("outer")}};

static const struct leme_public_argument args_describe[] = {
    {STATIC_TEXT("string"), STATIC_TEXT("outer")},
    {STATIC_TEXT("string"), STATIC_TEXT("outer")}};

static const struct leme_public_argument args_focus_view[] = {
    {STATIC_TEXT("view"), STATIC_TEXT("outer")}};

static const struct leme_public_argument args_focus_tag[] = {
    {STATIC_TEXT("tag"), STATIC_TEXT("outer")}};

static const struct leme_public_argument args_focus_output[] = {
    {STATIC_TEXT("output"), STATIC_TEXT("outer")}};

static const struct leme_public_argument args_move_to_tag[] = {
    {STATIC_TEXT("array<view>"), STATIC_TEXT("outer")},
    {STATIC_TEXT("tag"), STATIC_TEXT("outer")}};

static const struct leme_public_argument args_move_to_output[] = {
    {STATIC_TEXT("array<view>"), STATIC_TEXT("outer")},
    {STATIC_TEXT("output"), STATIC_TEXT("outer")}};

static const struct leme_public_argument args_views_boolean[] = {
    {STATIC_TEXT("array<view>"), STATIC_TEXT("outer")},
    {STATIC_TEXT("boolean"), STATIC_TEXT("outer")}};

static const struct leme_public_argument args_resize[] = {
    {STATIC_TEXT("view"), STATIC_TEXT("outer")},
    {STATIC_TEXT("string"), STATIC_TEXT("outer")},
    {STATIC_TEXT("integer"), STATIC_TEXT("outer")}};

static const struct leme_public_argument args_close[] = {
    {STATIC_TEXT("array<view>"), STATIC_TEXT("outer")}};

static const struct leme_public_argument args_set_layout[] = {
    {STATIC_TEXT("array<tag>"), STATIC_TEXT("outer")},
    {STATIC_TEXT("string"), STATIC_TEXT("outer")}};

static const struct leme_public_argument args_set_input[] = {
    {STATIC_TEXT("array<input>"), STATIC_TEXT("outer")},
    {STATIC_TEXT("string"), STATIC_TEXT("outer")},
    {STATIC_TEXT("any"), STATIC_TEXT("outer")}};

static const struct leme_public_argument args_configure_output[] = {
    {STATIC_TEXT("output"), STATIC_TEXT("outer")},
    {STATIC_TEXT("object"), STATIC_TEXT("outer")}};

static const struct leme_public_argument args_set_output_power[] = {
    {STATIC_TEXT("array<output>"), STATIC_TEXT("outer")},
    {STATIC_TEXT("boolean"), STATIC_TEXT("outer")}};

static const struct leme_public_argument args_set_config[] = {
    {STATIC_TEXT("array<string>"), STATIC_TEXT("outer")},
    {STATIC_TEXT("any"), STATIC_TEXT("outer")}};

static const struct leme_public_argument args_command[] = {
    {STATIC_TEXT("string"), STATIC_TEXT("outer")},
    {STATIC_TEXT("array<string>"), STATIC_TEXT("outer")}};

static const struct leme_public_operation public_operations[] = {
#define CONTROL_OP(id, name_str, is_act, min_a, max_a, args_arr, args_cnt,    \
                   res_str, desc_str, work_str)                                \
  {                                                                            \
      .name = STATIC_TEXT(name_str),                                          \
      .documentation = STATIC_TEXT(desc_str),                                 \
      .action = (is_act),                                                      \
      .available = true,                                                       \
      .stability = STATIC_TEXT("api-versioned"),                               \
      .min_args = (min_a),                                                     \
      .max_args = (max_a),                                                     \
      .arguments = (args_arr),                                                 \
      .argument_count = (args_cnt),                                            \
      .result = STATIC_TEXT(res_str),                                          \
      .work = STATIC_TEXT(work_str),                                          \
  },
#include "control/operators.def"
#undef CONTROL_OP
};

static const struct leme_control_operator operators[] = {
#define CONTROL_OP(id, name_str, is_act, min_a, max_a, args_arr, args_cnt,    \
                   res_str, desc_str, work_str)                                \
  [LEME_CONTROL_OP_##id] = {                                                   \
      .opcode = LEME_CONTROL_OP_##id,                                          \
      .name = STATIC_TEXT(name_str),                                          \
      .effect =                                                                \
          (is_act) ? LEME_CONTROL_EFFECT_ACTION : LEME_CONTROL_EFFECT_PURE,    \
      .min_args = (min_a),                                                     \
      .max_args = (max_a),                                                     \
      .arguments = (args_arr),                                                 \
      .argument_count = (args_cnt),                                            \
      .result = STATIC_TEXT(res_str),                                          \
      .description = STATIC_TEXT(desc_str),                                   \
      .work = STATIC_TEXT(work_str),                                          \
      .available = true,                                                       \
  },
#include "control/operators.def"
#undef CONTROL_OP
};

static const struct leme_control_registry registry = {
    .operations = public_operations,
    .count = sizeof(public_operations) / sizeof(public_operations[0])};

const struct leme_control_registry *leme_control_registry(void) {
  return &registry;
}

const struct leme_control_operator *
leme_control_operator_by_opcode(enum leme_control_opcode opcode) {
  if ((size_t)opcode >= LEME_CONTROL_OPERATOR_COUNT)
    return NULL;
  return &operators[opcode];
}

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

const struct leme_public_operation *
leme_control_operation_find(struct leme_public_text name) {
  const struct leme_control_operator *op = leme_control_operator_find(name);
  if (op == NULL || (size_t)op->opcode >= registry.count)
    return NULL;
  return &registry.operations[op->opcode];
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
  case LEME_CONTROL_OP_SET_FLOATING:
  case LEME_CONTROL_OP_SET_FULLSCREEN:
  case LEME_CONTROL_OP_SET_STICKY:
  case LEME_CONTROL_OP_RESIZE:
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
