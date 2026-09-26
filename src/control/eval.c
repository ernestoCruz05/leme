#include "control/eval-internal.h"
#include "control/memory.h"
#include "public/value-internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct leme_control_evaluation {
  struct leme_public_budget *account;
  struct leme_public_builder *builder;
  const struct leme_public_value *value;
};

enum leme_control_code eval_set_error(struct evaluator *ev, const char *path,
                                      enum leme_control_code code,
                                      const char *msg) {
  if (ev->error != NULL) {
    ev->error->code = code;
    ev->error->phase = LEME_CONTROL_EVALUATE;
    (void)snprintf(ev->error->message, sizeof(ev->error->message), "%s", msg);
    (void)snprintf(ev->error->expr_path, sizeof(ev->error->expr_path), "%s",
                   path != NULL ? path : "/expr");
  }
  return code;
}

enum leme_control_code eval_node(struct evaluator *ev, uint32_t node_idx,
                                 const struct leme_public_value **out) {
  if (leme_control_charge(&ev->meter, 1) != LEME_CONTROL_OK)
    return eval_set_error(ev, "/expr", LEME_CONTROL_RESOURCE_LIMIT,
                          "evaluation budget exceeded");
  const struct leme_control_node *node =
      leme_control_program_node(ev->program, node_idx);
  if (node == NULL)
    return eval_set_error(ev, "/expr", LEME_CONTROL_INVALID_ARGUMENT,
                          "invalid node index");

  switch (node->kind) {
  case LEME_CONTROL_NODE_LITERAL:
    *out = node->as.literal;
    return LEME_CONTROL_OK;
  case LEME_CONTROL_NODE_FIELD: {
    if (ev->current_item == NULL)
      return eval_set_error(ev, "/expr", LEME_CONTROL_INVALID_ARGUMENT,
                            "field outside item scope");
    const struct leme_public_value *cur = ev->current_item;
    for (size_t i = 0; i < node->as.field.count; ++i) {
      if (cur == NULL)
        return eval_set_error(ev, "/expr", LEME_CONTROL_NOT_FOUND,
                              "field traversal failed");
      if (leme_public_kind(cur) == LEME_PUBLIC_NULL) {
        *out = cur;
        return LEME_CONTROL_OK;
      }
      if (leme_public_kind(cur) != LEME_PUBLIC_OBJECT)
        return eval_set_error(ev, "/expr", LEME_CONTROL_NOT_FOUND,
                              "field traversal failed");
      const struct leme_public_value *next =
          leme_public_get(cur, node->as.field.components[i]);
      if (next == NULL && ev->snapshot != NULL) {
        const struct leme_public_value *type_val = leme_public_get(
            cur, (struct leme_public_text){.data = "type", .length = 4});
        const struct leme_public_value *id_val = leme_public_get(
            cur, (struct leme_public_text){.data = "id", .length = 2});
        if (type_val != NULL && id_val != NULL &&
            leme_public_kind(type_val) == LEME_PUBLIC_STRING &&
            leme_public_kind(id_val) == LEME_PUBLIC_STRING) {
          struct leme_public_text type_text = {0};
          struct leme_public_text id_text = {0};
          leme_public_as_text(type_val, &type_text);
          leme_public_as_text(id_val, &id_text);
          enum leme_public_entity ent = LEME_PUBLIC_VIEW;
          bool known_ent = false;
          if (type_text.length == 4 && memcmp(type_text.data, "view", 4) == 0) {
            ent = LEME_PUBLIC_VIEW;
            known_ent = true;
          } else if (type_text.length == 6 &&
                     memcmp(type_text.data, "output", 6) == 0) {
            ent = LEME_PUBLIC_OUTPUT;
            known_ent = true;
          } else if (type_text.length == 3 &&
                     memcmp(type_text.data, "tag", 3) == 0) {
            ent = LEME_PUBLIC_TAG;
            known_ent = true;
          } else if (type_text.length == 5 &&
                     memcmp(type_text.data, "input", 5) == 0) {
            ent = LEME_PUBLIC_INPUT;
            known_ent = true;
          }
          if (known_ent) {
            const struct leme_public_value *resolved = NULL;
            if (leme_public_snapshot_find(ev->snapshot, ent, id_text,
                                          &resolved) == LEME_PUBLIC_OK &&
                resolved != NULL) {
              cur = resolved;
              next = leme_public_get(cur, node->as.field.components[i]);
            }
          }
        }
      }
      if (next == NULL)
        return eval_set_error(ev, "/expr", LEME_CONTROL_NOT_FOUND,
                              "field not found on item");
      cur = next;
    }
    *out = cur;
    return LEME_CONTROL_OK;
  }
  case LEME_CONTROL_NODE_CALL: {
    enum leme_control_opcode op = node->as.call.op->opcode;
    if (op <= LEME_CONTROL_OP_BY_ID || op == LEME_CONTROL_OP_DESCRIBE)
      return eval_lookup_call(ev, node, out);
    if (op >= LEME_CONTROL_OP_WHERE && op <= LEME_CONTROL_OP_FIRST)
      return eval_collection_call(ev, node, out);
    return eval_scalar_call(ev, node, out);
  }
  default:
    return eval_set_error(ev, "/expr", LEME_CONTROL_INVALID_REQUEST,
                          "unknown node kind");
  }
}

static uint64_t monotonic_now_ns(void *context) {
  return leme_control_now_ns(context);
}

static enum leme_public_status evaluation_work(void *context, size_t units) {
  return leme_control_charge(context, units) == LEME_CONTROL_OK
             ? LEME_PUBLIC_OK
             : LEME_PUBLIC_LIMIT;
}

enum leme_control_code
leme_control_evaluate(struct leme_control_context *context,
                      const struct leme_control_program *program,
                      const struct leme_public_snapshot *snapshot,
                      struct leme_control_evaluation **out,
                      struct leme_control_error *error) {
  return leme_control_evaluate_work(context, program, snapshot, NULL, out,
                                    error);
}

enum leme_control_code
leme_control_evaluate_work(struct leme_control_context *context,
                           const struct leme_control_program *program,
                           const struct leme_public_snapshot *snapshot,
                           struct leme_control_meter *meter,
                           struct leme_control_evaluation **out,
                           struct leme_control_error *error) {
  if (out == NULL)
    return LEME_CONTROL_INVALID_ARGUMENT;
  *out = NULL;
  if (context == NULL || program == NULL)
    return LEME_CONTROL_INVALID_ARGUMENT;

  struct leme_public_budget *account = leme_control_context_account(context);
  const struct leme_control_limits *limits =
      leme_control_context_limits(context);

  struct leme_control_evaluation *eval =
      leme_control_alloc(account, sizeof(struct leme_control_evaluation));
  if (eval == NULL) {
    const enum leme_control_code code = errno == ENOSPC
                                            ? LEME_CONTROL_RESOURCE_LIMIT
                                            : LEME_CONTROL_OUT_OF_MEMORY;
    if (error != NULL) {
      error->code = code;
      error->phase = LEME_CONTROL_EVALUATE;
    }
    return code;
  }
  memset(eval, 0, sizeof(*eval));
  eval->account = account;

  size_t max_bytes = limits != NULL && limits->response_bytes > 0
                         ? limits->response_bytes
                         : 1048576;
  enum leme_public_status status =
      leme_public_builder_create_budget(account, max_bytes, &eval->builder);
  if (status != LEME_PUBLIC_OK) {
    enum leme_control_code code =
        (status == LEME_PUBLIC_LIMIT ? LEME_CONTROL_RESOURCE_LIMIT
                                     : LEME_CONTROL_OUT_OF_MEMORY);
    if (error != NULL) {
      error->code = code;
      error->phase = LEME_CONTROL_EVALUATE;
    }
    leme_control_free(eval);
    return code;
  }

  struct evaluator ev = {0};
  ev.context = context;
  ev.program = program;
  ev.snapshot = snapshot;
  ev.builder = eval->builder;
  ev.account = account;
  ev.limits = limits;
  uint64_t deadline = leme_control_context_deadline(context);
  if (meter == NULL && deadline == 0 && limits != NULL &&
      limits->deadline_ns > 0) {
    const uint64_t now = monotonic_now_ns(NULL);
    deadline = limits->deadline_ns > UINT64_MAX - now
                   ? UINT64_MAX
                   : now + limits->deadline_ns;
  }
  ev.meter = (struct leme_control_meter){
      .remaining = limits != NULL ? limits->work_units : 100000,
      .deadline_ns = deadline,
      .context = NULL,
      .now_ns = monotonic_now_ns,
  };
  if (meter != NULL)
    ev.meter = *meter;
  struct leme_control_error local_error = {0};
  ev.error = error != NULL ? error : &local_error;
  ev.current_item = NULL;
  const struct leme_public_work work = {.context = &ev.meter,
                                        .step = evaluation_work};
  leme_public_builder_set_work(eval->builder, &work);

  const struct leme_public_value *res = NULL;
  uint32_t root_idx = leme_control_program_root_index(program);
  enum leme_control_code code = eval_node(&ev, root_idx, &res);
  if (code != LEME_CONTROL_OK)
    goto done;

  if (res != NULL && res->owner != eval->builder) {
    res = eval_ensure_owned(&ev, res, &code);
    if (res == NULL)
      goto done;
  }

  if (res != NULL) {
    status = leme_public_builder_seal(eval->builder, &res, 1);
    if (status != LEME_PUBLIC_OK) {
      enum leme_control_code c =
          (status == LEME_PUBLIC_LIMIT ? LEME_CONTROL_RESOURCE_LIMIT
                                       : LEME_CONTROL_OUT_OF_MEMORY);
      if (error != NULL) {
        error->code = c;
        error->phase = LEME_CONTROL_EVALUATE;
      }
      code = c;
      goto done;
    }
  } else if (eval->builder != NULL) {
    status = leme_public_builder_seal(eval->builder, NULL, 0);
    if (status != LEME_PUBLIC_OK) {
      code = eval_set_error(&ev, "/expr", LEME_CONTROL_RESOURCE_LIMIT,
                            "evaluation sealing failed");
      goto done;
    }
  }

  eval->value = res;
done:
  if (meter != NULL)
    *meter = ev.meter;
  leme_public_builder_set_work(eval->builder, NULL);
  if (code != LEME_CONTROL_OK) {
    leme_control_evaluation_destroy(eval);
    return code;
  }
  *out = eval;
  return LEME_CONTROL_OK;
}

const struct leme_public_value *leme_control_evaluation_value(
    const struct leme_control_evaluation *evaluation) {
  return evaluation != NULL ? evaluation->value : NULL;
}

void leme_control_evaluation_destroy(
    struct leme_control_evaluation *evaluation) {
  if (evaluation == NULL)
    return;
  if (evaluation->builder != NULL)
    leme_public_builder_destroy(evaluation->builder);
  leme_control_free(evaluation);
}
