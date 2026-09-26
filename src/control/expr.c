#include "control/expr.h"

#include "control/control.h"
#include "control/memory.h"
#include "control/validate.h"
#include "public/schema.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t monotonic_now_ns(void *context) {
  return leme_control_now_ns(context);
}

struct leme_control_program {
  struct leme_public_budget *account;
  const struct leme_control_request *request;
  struct leme_control_node *nodes;
  size_t node_count;
  size_t node_capacity;
  uint32_t *arg_pool;
  size_t arg_pool_count;
  size_t arg_pool_capacity;
  uint32_t root_index;
  uint32_t roots;
  struct leme_control_type result_type;
};

struct compiler {
  struct leme_control_context *context;
  struct leme_public_budget *account;
  const struct leme_control_limits *limits;
  enum leme_control_request_op request_op;
  struct leme_control_program *program;
  struct leme_control_error *error;
  struct leme_control_meter meter;
  char path_buf[1024];
  size_t path_len;
};

static enum leme_control_code
set_error(struct compiler *c, enum leme_control_code code, const char *msg) {
  if (c->error != NULL) {
    c->error->code = code;
    c->error->phase = LEME_CONTROL_VALIDATE;
    (void)snprintf(c->error->message, sizeof(c->error->message), "%s", msg);
    (void)snprintf(c->error->expr_path, sizeof(c->error->expr_path), "%s",
                   c->path_buf);
  }
  return code;
}

static size_t push_path_arg(struct compiler *c, size_t index) {
  size_t prev_len = c->path_len;
  int n = snprintf(c->path_buf + c->path_len, sizeof(c->path_buf) - c->path_len,
                   "/args/%zu", index);
  if (n > 0)
    c->path_len += (size_t)n;
  return prev_len;
}

static void pop_path_arg(struct compiler *c, size_t prev_len) {
  c->path_len = prev_len;
  c->path_buf[c->path_len] = '\0';
}

static enum leme_control_code
reserve_node(struct compiler *c, uint32_t *out_idx) {
  if (c->program->node_count >= c->limits->expression_nodes)
    return set_error(c, LEME_CONTROL_RESOURCE_LIMIT,
                     "expression node limit exceeded");

  if (c->program->node_count == c->program->node_capacity) {
    size_t new_cap =
        c->program->node_capacity == 0 ? 16 : c->program->node_capacity * 2;
    if (new_cap > c->limits->expression_nodes)
      new_cap = c->limits->expression_nodes;

    struct leme_control_node *new_nodes =
        c->program->nodes == NULL
            ? leme_control_alloc(c->account,
                                 new_cap * sizeof(struct leme_control_node))
            : leme_control_realloc(c->program->nodes,
                                   new_cap * sizeof(struct leme_control_node));
    if (new_nodes == NULL) {
      enum leme_control_code code =
          errno == ENOSPC ? LEME_CONTROL_RESOURCE_LIMIT
                          : LEME_CONTROL_OUT_OF_MEMORY;
      return set_error(c, code, "node allocation failed");
    }
    c->program->nodes = new_nodes;
    c->program->node_capacity = new_cap;
  }

  *out_idx = (uint32_t)c->program->node_count++;
  return LEME_CONTROL_OK;
}

static enum leme_control_code
reserve_args(struct compiler *c, size_t count, uint32_t **out_ptr) {
  if (count == 0) {
    *out_ptr = NULL;
    return LEME_CONTROL_OK;
  }
  if (c->program->arg_pool_count + count > c->program->arg_pool_capacity) {
    size_t needed = c->program->arg_pool_count + count;
    size_t new_cap =
        c->program->arg_pool_capacity == 0 ? 32 : c->program->arg_pool_capacity * 2;
    if (new_cap < needed)
      new_cap = needed;

    uint32_t *new_pool =
        c->program->arg_pool == NULL
            ? leme_control_alloc(c->account, new_cap * sizeof(uint32_t))
            : leme_control_realloc(c->program->arg_pool,
                                   new_cap * sizeof(uint32_t));
    if (new_pool == NULL) {
      enum leme_control_code code =
          errno == ENOSPC ? LEME_CONTROL_RESOURCE_LIMIT
                          : LEME_CONTROL_OUT_OF_MEMORY;
      return set_error(c, code, "arg allocation failed");
    }
    c->program->arg_pool = new_pool;
    c->program->arg_pool_capacity = new_cap;
  }

  *out_ptr = &c->program->arg_pool[c->program->arg_pool_count];
  c->program->arg_pool_count += count;
  return LEME_CONTROL_OK;
}

static enum leme_control_code
compile_node(struct compiler *c, const struct leme_public_value *node_val,
             const struct leme_control_type *item_scope, size_t depth,
             bool is_root, uint32_t *out_idx);

static enum leme_control_code
compile_literal(struct compiler *c, const struct leme_public_value *val,
                size_t depth, uint32_t *out_idx) {
  uint32_t idx = 0;
  enum leme_control_code code = reserve_node(c, &idx);
  if (code != LEME_CONTROL_OK)
    return code;

  struct leme_control_node *node = &c->program->nodes[idx];
  node->kind = LEME_CONTROL_NODE_LITERAL;
  node->type = leme_control_type_from_value(val);
  node->roots = 0;
  node->depth = depth;
  node->as.literal = val;

  *out_idx = idx;
  return LEME_CONTROL_OK;
}

static enum leme_control_code
compile_field(struct compiler *c, const struct leme_public_value *fval,
              const struct leme_control_type *item_scope, size_t depth,
              uint32_t *out_idx) {
  if (leme_public_kind(fval) != LEME_PUBLIC_ARRAY)
    return set_error(c, LEME_CONTROL_INVALID_REQUEST,
                     "field path must be an array");

  size_t count = leme_public_length(fval);
  if (count == 0)
    return set_error(c, LEME_CONTROL_INVALID_ARGUMENT,
                     "field path cannot be empty");
  if (count > 16)
    return set_error(c, LEME_CONTROL_RESOURCE_LIMIT,
                     "field path exceeds 16 components");

  struct leme_control_field_path path = {0};
  path.count = count;
  for (size_t i = 0; i < count; ++i) {
    const struct leme_public_value *comp = leme_public_at(fval, i);
    struct leme_public_text text = {0};
    if (leme_public_as_text(comp, &text) != LEME_PUBLIC_OK || text.length == 0)
      return set_error(c, LEME_CONTROL_INVALID_ARGUMENT,
                       "field component must be nonempty string");
    path.components[i] = text;
  }

  if (item_scope == NULL)
    return set_error(c, LEME_CONTROL_INVALID_ARGUMENT,
                     "field selector outside item scope");

  struct leme_control_type field_type = {0};
  enum leme_control_code code =
      leme_control_resolve_field_path(item_scope, path.components, count,
                                      &field_type);
  if (code != LEME_CONTROL_OK)
    return set_error(c, code, "failed to resolve field path");

  uint32_t idx = 0;
  code = reserve_node(c, &idx);
  if (code != LEME_CONTROL_OK)
    return code;

  struct leme_control_node *node = &c->program->nodes[idx];
  node->kind = LEME_CONTROL_NODE_FIELD;
  node->type = field_type;
  node->roots = 0;
  node->depth = depth;
  node->as.field = path;

  *out_idx = idx;
  return LEME_CONTROL_OK;
}

static enum leme_control_code
compile_call(struct compiler *c, const struct leme_public_value *call_val,
             const struct leme_control_type *item_scope,
             const struct leme_public_value *args_val, size_t depth,
             bool is_root, uint32_t *out_idx) {
  struct leme_public_text call_name = {0};
  if (leme_public_as_text(call_val, &call_name) != LEME_PUBLIC_OK)
    return set_error(c, LEME_CONTROL_INVALID_REQUEST,
                     "call name must be string");

  if (leme_public_kind(args_val) != LEME_PUBLIC_ARRAY)
    return set_error(c, LEME_CONTROL_INVALID_REQUEST,
                     "args must be an array");

  const struct leme_control_operator *op =
      leme_control_operator_find(call_name);
  if (op == NULL)
    return set_error(c, LEME_CONTROL_UNKNOWN_OPERATOR, "unknown operator");

  size_t arg_count = leme_public_length(args_val);
  if (arg_count < op->min_args || arg_count > op->max_args)
    return set_error(c, LEME_CONTROL_INVALID_ARGUMENT,
                     "operator arity mismatch");

  if (c->request_op != LEME_CONTROL_ACT &&
      op->effect == LEME_CONTROL_EFFECT_ACTION)
    return set_error(c, LEME_CONTROL_INVALID_REQUEST,
                     "action operator not allowed in pure request");

  if (c->request_op == LEME_CONTROL_ACT) {
    if (is_root && op->effect != LEME_CONTROL_EFFECT_ACTION)
      return set_error(c, LEME_CONTROL_INVALID_REQUEST,
                       "act request must have action operator at root");
    if (!is_root && op->effect == LEME_CONTROL_EFFECT_ACTION)
      return set_error(c, LEME_CONTROL_INVALID_REQUEST,
                       "nested action operator not permitted");
  }

  if (op->opcode == LEME_CONTROL_OP_ITEM && item_scope == NULL)
    return set_error(c, LEME_CONTROL_INVALID_ARGUMENT,
                     "item operator outside item scope");

  uint32_t *arg_indices = NULL;
  enum leme_control_code code = reserve_args(c, arg_count, &arg_indices);
  if (code != LEME_CONTROL_OK)
    return code;

  struct leme_control_type child_types[16];
  const struct leme_public_value *child_literals[16];
  const struct leme_control_type *cur_item_scope = item_scope;

  for (size_t i = 0; i < arg_count; ++i) {
    const struct leme_public_value *arg_node = leme_public_at(args_val, i);
    size_t prev_len = push_path_arg(c, i);

    const struct leme_control_type *scope_for_arg = item_scope;
    if (leme_control_operator_arg_is_item_scope(op, i)) {
      scope_for_arg = cur_item_scope;
    }

    uint32_t child_idx = 0;
    code = compile_node(c, arg_node, scope_for_arg, depth + 1, false,
                        &child_idx);
    if (code != LEME_CONTROL_OK) {
      pop_path_arg(c, prev_len);
      return code;
    }

    arg_indices[i] = child_idx;
    if (i < 16) {
      child_types[i] = c->program->nodes[child_idx].type;
      child_literals[i] =
          c->program->nodes[child_idx].kind == LEME_CONTROL_NODE_LITERAL
              ? c->program->nodes[child_idx].as.literal
              : NULL;
    }

    if (i == 0 && (op->opcode == LEME_CONTROL_OP_WHERE ||
                   op->opcode == LEME_CONTROL_OP_SELECT ||
                   op->opcode == LEME_CONTROL_OP_MAP ||
                   op->opcode == LEME_CONTROL_OP_SORT_BY)) {
      if (child_types[0].kind == LEME_CONTROL_TYPE_ARRAY &&
          child_types[0].item_type != NULL &&
          child_types[0].item_type->kind != LEME_CONTROL_TYPE_ANY) {
        cur_item_scope = child_types[0].item_type;
      } else if (op->opcode == LEME_CONTROL_OP_SELECT ||
                 op->opcode == LEME_CONTROL_OP_SORT_BY) {
        static const struct leme_control_type untyped_rec = {
            .kind = LEME_CONTROL_TYPE_RECORD,
            .provenance = LEME_CONTROL_PROVENANCE_SYNTHETIC,
        };
        cur_item_scope = &untyped_rec;
      } else {
        static const struct leme_control_type untyped_any = {
            .kind = LEME_CONTROL_TYPE_ANY};
        cur_item_scope = &untyped_any;
      }
    }

    pop_path_arg(c, prev_len);
  }

  for (size_t i = 0; i < arg_count; ++i) {
    uint32_t child_idx = arg_indices[i];
    const struct leme_control_node *child = &c->program->nodes[child_idx];

    if (op->opcode == LEME_CONTROL_OP_SELECT && i >= 1) {
      if (child->kind != LEME_CONTROL_NODE_FIELD) {
        size_t prev_len = push_path_arg(c, i);
        code = set_error(c, LEME_CONTROL_INVALID_ARGUMENT,
                         "select argument must be a field node");
        pop_path_arg(c, prev_len);
        return code;
      }
      for (size_t j = 1; j < i; ++j) {
        const struct leme_control_node *prev =
            &c->program->nodes[arg_indices[j]];
        if (prev->kind == LEME_CONTROL_NODE_FIELD &&
            prev->as.field.count == child->as.field.count) {
          bool match = true;
          for (size_t k = 0; k < child->as.field.count; ++k) {
            if (prev->as.field.components[k].length !=
                    child->as.field.components[k].length ||
                memcmp(prev->as.field.components[k].data,
                       child->as.field.components[k].data,
                       child->as.field.components[k].length) != 0) {
              match = false;
              break;
            }
          }
          if (match) {
            size_t prev_len = push_path_arg(c, i);
            code = set_error(c, LEME_CONTROL_INVALID_ARGUMENT,
                             "duplicate field in select");
            pop_path_arg(c, prev_len);
            return code;
          }
        }
      }
    }

    if (op->opcode == LEME_CONTROL_OP_WHERE && i == 1) {
      if (child->type.kind != LEME_CONTROL_TYPE_BOOLEAN &&
          child->type.kind != LEME_CONTROL_TYPE_ANY) {
        size_t prev_len = push_path_arg(c, 1);
        code = set_error(c, LEME_CONTROL_TYPE_ERROR,
                         "where predicate must be boolean");
        pop_path_arg(c, prev_len);
        return code;
      }
    }

    if (op->opcode == LEME_CONTROL_OP_IF && i == 0) {
      if (child->type.kind != LEME_CONTROL_TYPE_BOOLEAN &&
          child->type.kind != LEME_CONTROL_TYPE_ANY) {
        size_t prev_len = push_path_arg(c, 0);
        code = set_error(c, LEME_CONTROL_TYPE_ERROR,
                         "if condition must be boolean");
        pop_path_arg(c, prev_len);
        return code;
      }
    }

    if ((op->opcode == LEME_CONTROL_OP_NOT ||
         op->opcode == LEME_CONTROL_OP_AND ||
         op->opcode == LEME_CONTROL_OP_OR) &&
        child->type.kind != LEME_CONTROL_TYPE_BOOLEAN &&
        child->type.kind != LEME_CONTROL_TYPE_ANY) {
      size_t prev_len = push_path_arg(c, i);
      code = set_error(c, LEME_CONTROL_TYPE_ERROR,
                       "expected boolean argument");
      pop_path_arg(c, prev_len);
      return code;
    }

    if ((op->opcode == LEME_CONTROL_OP_ADD ||
         op->opcode == LEME_CONTROL_OP_SUB ||
         op->opcode == LEME_CONTROL_OP_MUL ||
         op->opcode == LEME_CONTROL_OP_DIV) &&
        child->type.kind != LEME_CONTROL_TYPE_NUMBER &&
        child->type.kind != LEME_CONTROL_TYPE_ANY) {
      size_t prev_len = push_path_arg(c, i);
      code = set_error(c, LEME_CONTROL_TYPE_ERROR,
                       "expected number argument");
      pop_path_arg(c, prev_len);
      return code;
    }

    if (op->opcode == LEME_CONTROL_OP_OBJECT) {
      if (i % 2 == 0 && child->type.kind != LEME_CONTROL_TYPE_STRING &&
          child->type.kind != LEME_CONTROL_TYPE_ANY) {
        size_t prev_len = push_path_arg(c, i);
        code = set_error(c, LEME_CONTROL_TYPE_ERROR,
                         "object key must be string");
        pop_path_arg(c, prev_len);
        return code;
      }
    }

    if (op->opcode == LEME_CONTROL_OP_LT || op->opcode == LEME_CONTROL_OP_LE ||
        op->opcode == LEME_CONTROL_OP_GT || op->opcode == LEME_CONTROL_OP_GE) {
      if (child->type.kind != LEME_CONTROL_TYPE_NUMBER &&
          child->type.kind != LEME_CONTROL_TYPE_STRING &&
          child->type.kind != LEME_CONTROL_TYPE_ANY) {
        size_t prev_len = push_path_arg(c, i);
        code = set_error(c, LEME_CONTROL_TYPE_ERROR,
                         "comparison operand must be number or string");
        pop_path_arg(c, prev_len);
        return code;
      }
    }

    if (op->effect == LEME_CONTROL_EFFECT_ACTION) {
      if (!leme_control_is_valid_action_target(op, i, &child->type)) {
        size_t prev_len = push_path_arg(c, i);
        code = set_error(c, LEME_CONTROL_TYPE_ERROR,
                         "action target must be trusted snapshot entity");
        pop_path_arg(c, prev_len);
        return code;
      }
    }
  }

  if (op->opcode == LEME_CONTROL_OP_OBJECT) {
    if (arg_count % 2 != 0)
      return set_error(c, LEME_CONTROL_INVALID_ARGUMENT,
                       "object requires even number of arguments");
    for (size_t k1 = 0; k1 < arg_count; k1 += 2) {
      if (child_literals[k1] != NULL) {
        struct leme_public_text key1 = {0};
        if (leme_public_as_text(child_literals[k1], &key1) == LEME_PUBLIC_OK) {
          for (size_t k2 = k1 + 2; k2 < arg_count; k2 += 2) {
            if (child_literals[k2] != NULL) {
              struct leme_public_text key2 = {0};
              if (leme_public_as_text(child_literals[k2], &key2) ==
                  LEME_PUBLIC_OK) {
                if (key1.length == key2.length &&
                    memcmp(key1.data, key2.data, key1.length) == 0)
                  return set_error(c, LEME_CONTROL_INVALID_ARGUMENT,
                                   "duplicate object key");
              }
            }
          }
        }
      }
    }
  }

  if ((op->opcode == LEME_CONTROL_OP_LT || op->opcode == LEME_CONTROL_OP_LE ||
       op->opcode == LEME_CONTROL_OP_GT || op->opcode == LEME_CONTROL_OP_GE) &&
      arg_count == 2) {
    if (child_types[0].kind != LEME_CONTROL_TYPE_ANY &&
        child_types[1].kind != LEME_CONTROL_TYPE_ANY &&
        child_types[0].kind != child_types[1].kind)
      return set_error(c, LEME_CONTROL_TYPE_ERROR,
                       "mismatched comparison types");
  }

  struct leme_control_type res_type = {0};
  code = leme_control_infer_operator_result(op, child_types, arg_count,
                                           &res_type);
  if (code != LEME_CONTROL_OK)
    return set_error(c, code, "type inference failed");

  uint32_t roots =
      leme_control_operator_roots(op, child_literals, arg_count);
  for (size_t i = 0; i < arg_count; ++i) {
    roots |= c->program->nodes[arg_indices[i]].roots;
  }

  uint32_t idx = 0;
  code = reserve_node(c, &idx);
  if (code != LEME_CONTROL_OK)
    return code;

  struct leme_control_node *node = &c->program->nodes[idx];
  node->kind = LEME_CONTROL_NODE_CALL;
  node->type = res_type;
  node->roots = roots;
  node->depth = depth;
  node->as.call.op = op;
  node->as.call.arg_indices = arg_indices;
  node->as.call.arg_count = arg_count;

  *out_idx = idx;
  return LEME_CONTROL_OK;
}

static enum leme_control_code
compile_node(struct compiler *c, const struct leme_public_value *node_val,
             const struct leme_control_type *item_scope, size_t depth,
             bool is_root, uint32_t *out_idx) {
  if (leme_control_charge(&c->meter, 1) != LEME_CONTROL_OK)
    return set_error(c, LEME_CONTROL_RESOURCE_LIMIT,
                     "validation budget exceeded");

  if (depth > c->limits->expression_depth)
    return set_error(c, LEME_CONTROL_RESOURCE_LIMIT,
                     "expression depth limit exceeded");

  if (node_val == NULL || leme_public_kind(node_val) != LEME_PUBLIC_OBJECT)
    return set_error(c, LEME_CONTROL_INVALID_REQUEST,
                     "expression node must be an object");

  size_t count = leme_public_length(node_val);
  const struct leme_public_value *literal =
      leme_public_get(node_val, LEME_PUBLIC_TEXT("literal"));
  const struct leme_public_value *field =
      leme_public_get(node_val, LEME_PUBLIC_TEXT("field"));
  const struct leme_public_value *call =
      leme_public_get(node_val, LEME_PUBLIC_TEXT("call"));
  const struct leme_public_value *args =
      leme_public_get(node_val, LEME_PUBLIC_TEXT("args"));

  if (literal != NULL) {
    if (count != 1)
      return set_error(c, LEME_CONTROL_INVALID_REQUEST,
                       "literal node must have no other members");
    return compile_literal(c, literal, depth, out_idx);
  }

  if (field != NULL) {
    if (count != 1)
      return set_error(c, LEME_CONTROL_INVALID_REQUEST,
                       "field node must have no other members");
    return compile_field(c, field, item_scope, depth, out_idx);
  }

  if (call != NULL && args != NULL) {
    if (count != 2)
      return set_error(c, LEME_CONTROL_INVALID_REQUEST,
                       "call node must have exactly call and args");
    return compile_call(c, call, item_scope, args, depth, is_root, out_idx);
  }

  return set_error(c, LEME_CONTROL_INVALID_REQUEST,
                   "malformed expression node shape");
}

enum leme_control_code leme_control_compile(
    struct leme_control_context *context,
    const struct leme_control_request *request,
    struct leme_control_program **out, struct leme_control_error *error) {
  return leme_control_compile_work(context, request, NULL, out, error);
}

enum leme_control_code
leme_control_compile_work(struct leme_control_context *context,
                          const struct leme_control_request *request,
                          struct leme_control_meter *meter,
                          struct leme_control_program **out,
                          struct leme_control_error *error) {
  if (out == NULL)
    return LEME_CONTROL_INVALID_ARGUMENT;
  *out = NULL;
  if (context == NULL || request == NULL)
    return LEME_CONTROL_INVALID_ARGUMENT;

  const struct leme_public_value *expr =
      leme_control_request_expression(request);
  if (expr == NULL)
    return LEME_CONTROL_INVALID_REQUEST;

  struct leme_public_budget *account = leme_control_context_account(context);
  const struct leme_control_limits *limits =
      leme_control_context_limits(context);
  if (limits == NULL)
    return LEME_CONTROL_INVALID_ARGUMENT;

  struct leme_control_program *prog =
      leme_control_alloc(account, sizeof(struct leme_control_program));
  if (prog == NULL) {
    enum leme_control_code code =
        errno == ENOSPC ? LEME_CONTROL_RESOURCE_LIMIT
                        : LEME_CONTROL_OUT_OF_MEMORY;
    if (error != NULL) {
      error->code = code;
      error->phase = LEME_CONTROL_VALIDATE;
    }
    return code;
  }
  memset(prog, 0, sizeof(*prog));
  prog->account = account;
  prog->request = request;

  uint64_t deadline = leme_control_context_deadline(context);
  if (meter == NULL && deadline == 0 && limits->deadline_ns > 0) {
    const uint64_t now = monotonic_now_ns(NULL);
    deadline = limits->deadline_ns > UINT64_MAX - now
                   ? UINT64_MAX
                   : now + limits->deadline_ns;
  }

  struct compiler c = {0};
  c.context = context;
  c.account = account;
  c.limits = limits;
  c.request_op = leme_control_request_operation(request);
  c.program = prog;
  c.error = error;
  c.meter = (struct leme_control_meter){
      .remaining = limits != NULL ? limits->work_units : 100000,
      .deadline_ns = deadline,
      .context = NULL,
      .now_ns = monotonic_now_ns,
  };
  if (meter != NULL)
    c.meter = *meter;
  memcpy(c.path_buf, "/expr", 5);
  c.path_len = 5;
  c.path_buf[5] = '\0';

  uint32_t root_idx = 0;
  enum leme_control_code code =
      compile_node(&c, expr, NULL, 1, true, &root_idx);
  if (meter != NULL)
    *meter = c.meter;
  if (code != LEME_CONTROL_OK) {
    leme_control_program_destroy(prog);
    return code;
  }

  prog->root_index = root_idx;
  prog->roots = prog->nodes[root_idx].roots;
  prog->result_type = prog->nodes[root_idx].type;

  *out = prog;
  return LEME_CONTROL_OK;
}

uint32_t leme_control_program_roots(const struct leme_control_program *program) {
  return program != NULL ? program->roots : 0;
}

void leme_control_program_destroy(struct leme_control_program *program) {
  if (program == NULL)
    return;
  if (program->nodes != NULL)
    leme_control_free(program->nodes);
  if (program->arg_pool != NULL)
    leme_control_free(program->arg_pool);
  leme_control_free(program);
}

size_t
leme_control_program_node_count(const struct leme_control_program *program) {
  return program != NULL ? program->node_count : 0;
}

const struct leme_control_node *
leme_control_program_node(const struct leme_control_program *program,
                          size_t index) {
  if (program == NULL || index >= program->node_count)
    return NULL;
  return &program->nodes[index];
}

uint32_t
leme_control_program_root_index(const struct leme_control_program *program) {
  return program != NULL ? program->root_index : 0;
}

const struct leme_control_type *
leme_control_program_type(const struct leme_control_program *program) {
  return program != NULL ? &program->result_type : NULL;
}
