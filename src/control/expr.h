#ifndef LEME_CONTROL_EXPR_H
#define LEME_CONTROL_EXPR_H

#include "control/control.h"
#include "control/error.h"
#include "control/limits.h"
#include "control/registry.h"
#include "control/request.h"
#include "control/types.h"
#include "public/value.h"

#include <stddef.h>
#include <stdint.h>

struct leme_control_program;

enum leme_control_node_kind {
  LEME_CONTROL_NODE_LITERAL,
  LEME_CONTROL_NODE_FIELD,
  LEME_CONTROL_NODE_CALL
};

struct leme_control_field_path {
  struct leme_public_text components[16];
  size_t count;
};

struct leme_control_node {
  enum leme_control_node_kind kind;
  struct leme_control_type type;
  uint32_t roots;
  size_t depth;
  union {
    const struct leme_public_value *literal;
    struct leme_control_field_path field;
    struct {
      const struct leme_control_operator *op;
      uint32_t *arg_indices;
      size_t arg_count;
    } call;
  } as;
};

enum leme_control_code
leme_control_compile(struct leme_control_context *context,
                     const struct leme_control_request *request,
                     struct leme_control_program **out,
                     struct leme_control_error *error);

enum leme_control_code
leme_control_compile_work(struct leme_control_context *context,
                          const struct leme_control_request *request,
                          struct leme_control_meter *meter,
                          struct leme_control_program **out,
                          struct leme_control_error *error);

uint32_t leme_control_program_roots(const struct leme_control_program *program);
void leme_control_program_destroy(struct leme_control_program *program);

size_t
leme_control_program_node_count(const struct leme_control_program *program);
const struct leme_control_node *
leme_control_program_node(const struct leme_control_program *program,
                          size_t index);
uint32_t
leme_control_program_root_index(const struct leme_control_program *program);
const struct leme_control_type *
leme_control_program_type(const struct leme_control_program *program);

#endif
