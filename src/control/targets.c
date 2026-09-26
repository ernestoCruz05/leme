#include "control/action-internal.h"
#include "control/command.h"
#include "control/eval-internal.h"
#include "control/memory.h"
#include "public/identity.h"
#include "public/model-internal.h"
#include "public/model.h"
#include "public/value-internal.h"
#include "public/value.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t monotonic_now_ns(void *context) {
  return leme_control_now_ns(context);
}

static void *unconst(const void *p) {
  void *out = NULL;
  memcpy((void *)&out, (const void *)&p, sizeof(out));
  return out;
}

static bool op_is_single_target(enum leme_control_opcode opcode,
                                enum leme_public_entity *out_entity) {
  switch (opcode) {
  case LEME_CONTROL_OP_FOCUS:
  case LEME_CONTROL_OP_RESIZE:
    *out_entity = LEME_PUBLIC_VIEW;
    return true;
  case LEME_CONTROL_OP_FOCUS_TAG:
    *out_entity = LEME_PUBLIC_TAG;
    return true;
  case LEME_CONTROL_OP_FOCUS_OUTPUT:
  case LEME_CONTROL_OP_CONFIGURE_OUTPUT:
    *out_entity = LEME_PUBLIC_OUTPUT;
    return true;
  default:
    return false;
  }
}

static bool op_is_bulk_target(enum leme_control_opcode opcode,
                              enum leme_public_entity *out_entity) {
  switch (opcode) {
  case LEME_CONTROL_OP_MOVE_TO_TAG:
  case LEME_CONTROL_OP_MOVE_TO_OUTPUT:
  case LEME_CONTROL_OP_SET_FLOATING:
  case LEME_CONTROL_OP_SET_FULLSCREEN:
  case LEME_CONTROL_OP_SET_STICKY:
  case LEME_CONTROL_OP_CLOSE:
    *out_entity = LEME_PUBLIC_VIEW;
    return true;
  case LEME_CONTROL_OP_SET_LAYOUT:
    *out_entity = LEME_PUBLIC_TAG;
    return true;
  case LEME_CONTROL_OP_SET_INPUT:
    *out_entity = LEME_PUBLIC_INPUT;
    return true;
  case LEME_CONTROL_OP_SET_OUTPUT_POWER:
    *out_entity = LEME_PUBLIC_OUTPUT;
    return true;
  default:
    return false;
  }
}

static struct leme_public_text dup_text(struct leme_public_budget *account,
                                        struct leme_public_text src) {
  if (src.length == 0 || src.data == NULL) {
    return (struct leme_public_text){.data = "", .length = 0};
  }
  char *buf = leme_control_alloc(account, src.length + 1);
  if (buf == NULL) {
    return (struct leme_public_text){0};
  }
  memcpy(buf, src.data, src.length);
  buf[src.length] = '\0';
  return (struct leme_public_text){.data = buf, .length = src.length};
}

void leme_control_intents_destroy(struct leme_control_intent *intents,
                                  size_t count) {
  if (intents == NULL) {
    return;
  }
  for (size_t i = 0; i < count; ++i) {
    if (intents[i].effective_id.data != NULL &&
        intents[i].effective_id.length > 0) {
      leme_control_free(unconst(intents[i].effective_id.data));
    }
    if (intents[i].requested_targets != NULL) {
      for (size_t r = 0; r < intents[i].requested_count; ++r) {
        if (intents[i].requested_targets[r].data != NULL &&
            intents[i].requested_targets[r].length > 0) {
          leme_control_free(unconst(intents[i].requested_targets[r].data));
        }
      }
      leme_control_free(intents[i].requested_targets);
    }
  }
  leme_control_free(intents);
}

static enum leme_control_code set_preflight_error(
    struct leme_control_error *error, enum leme_control_code code,
    const char *msg) {
  if (error != NULL) {
    error->code = code;
    error->phase = LEME_CONTROL_PREFLIGHT;
    (void)snprintf(error->message, sizeof(error->message), "%s", msg);
    (void)snprintf(error->expr_path, sizeof(error->expr_path), "/expr");
    error->effects_applied = false;
  }
  return code;
}

enum leme_control_code leme_control_normalize_targets(
    struct leme_control_context *context,
    const struct leme_control_program *program,
    const struct leme_public_snapshot *snapshot,
    struct leme_control_intent **out_intents,
    size_t *out_count,
    struct leme_public_builder **out_builder,
    struct leme_control_error *error) {
  if (context == NULL || program == NULL || out_intents == NULL ||
      out_count == NULL || out_builder == NULL) {
    return LEME_CONTROL_INVALID_ARGUMENT;
  }
  *out_intents = NULL;
  *out_count = 0;
  *out_builder = NULL;

  uint32_t root_idx = leme_control_program_root_index(program);
  const struct leme_control_node *root_node =
      leme_control_program_node(program, root_idx);
  if (root_node == NULL || root_node->kind != LEME_CONTROL_NODE_CALL ||
      root_node->as.call.op == NULL) {
    return LEME_CONTROL_INVALID_REQUEST;
  }

  const struct leme_control_operator *op = root_node->as.call.op;
  if (op->effect != LEME_CONTROL_EFFECT_ACTION) {
    return LEME_CONTROL_INVALID_REQUEST;
  }

  struct leme_public_budget *account = leme_control_context_account(context);
  const struct leme_control_limits *limits =
      leme_control_context_limits(context);
  struct leme_public_model *model = leme_control_context_model(context);

  enum leme_public_entity expected_entity = LEME_PUBLIC_VIEW;
  bool is_single = op_is_single_target(op->opcode, &expected_entity);
  bool is_bulk = op_is_bulk_target(op->opcode, &expected_entity);

  struct leme_public_builder *b = NULL;
  enum leme_public_status b_st =
      leme_public_builder_create_budget(account, limits->response_bytes, &b);
  if (b_st != LEME_PUBLIC_OK) {
    return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                               "out of memory");
  }

  uint64_t deadline = leme_control_context_deadline(context);
  if (deadline == 0 && limits != NULL && limits->deadline_ns > 0) {
    deadline = monotonic_now_ns(NULL) + limits->deadline_ns;
  }
  struct evaluator ev = {
      .context = context,
      .program = program,
      .snapshot = snapshot,
      .builder = b,
      .account = account,
      .limits = limits,
      .error = error,
      .current_item = NULL,
      .meter = {
          .remaining = limits != NULL ? limits->work_units : 100000,
          .deadline_ns = deadline,
          .context = NULL,
          .now_ns = monotonic_now_ns,
      },
  };

  if (op->opcode == LEME_CONTROL_OP_COMMAND) {
    if (root_node->as.call.arg_count != 2) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_INVALID_REQUEST,
                                 "command requires 2 arguments");
    }
    const struct leme_public_value *name_val = NULL;
    enum leme_control_code ev_code =
        eval_node(&ev, root_node->as.call.arg_indices[0], &name_val);
    if (ev_code != LEME_CONTROL_OK) {
      leme_public_builder_destroy(b);
      if (error != NULL) {
        error->phase = LEME_CONTROL_PREFLIGHT;
      }
      return ev_code;
    }
    if (name_val == NULL || leme_public_kind(name_val) != LEME_PUBLIC_STRING) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_TYPE_ERROR,
                                 "command name must be string");
    }
    struct leme_public_text name_text = {0};
    leme_public_as_text(name_val, &name_text);

    const struct leme_public_value *argv_val = NULL;
    ev_code = eval_node(&ev, root_node->as.call.arg_indices[1], &argv_val);
    if (ev_code != LEME_CONTROL_OK) {
      leme_public_builder_destroy(b);
      if (error != NULL) {
        error->phase = LEME_CONTROL_PREFLIGHT;
      }
      return ev_code;
    }
    if (argv_val == NULL || leme_public_kind(argv_val) != LEME_PUBLIC_ARRAY) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_TYPE_ERROR,
                                 "command argv must be array");
    }

    struct leme_control_intent_batch batch = {0};
    enum leme_control_code lower_code = leme_control_command_lower(
        context, snapshot, name_text, argv_val, &batch, error);
    if (lower_code != LEME_CONTROL_OK) {
      leme_public_builder_destroy(b);
      return lower_code;
    }
    leme_public_builder_destroy(b);
    *out_intents = batch.intents;
    *out_count = batch.count;
    *out_builder = batch.builder;
    return LEME_CONTROL_OK;
  }

  if (!is_single && !is_bulk) {
    const struct leme_public_value *common_args = NULL;
    if (root_node->as.call.arg_count == 1) {
      const struct leme_public_value *raw_arg = NULL;
      enum leme_control_code ev_code =
          eval_node(&ev, root_node->as.call.arg_indices[0], &raw_arg);
      if (ev_code != LEME_CONTROL_OK) {
        leme_public_builder_destroy(b);
        if (error != NULL) {
          error->phase = LEME_CONTROL_PREFLIGHT;
        }
        return ev_code;
      }
      if (raw_arg != NULL && raw_arg->owner != b) {
        struct leme_public_value *cloned = NULL;
        if (leme_public_clone(b, raw_arg, &cloned) != LEME_PUBLIC_OK) {
          leme_public_builder_destroy(b);
          return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                     "out of memory");
        }
        common_args = cloned;
      } else {
        common_args = raw_arg;
      }
    } else if (root_node->as.call.arg_count > 1) {
      struct leme_public_value *arr = NULL;
      if (leme_public_array(b, root_node->as.call.arg_count, &arr) !=
          LEME_PUBLIC_OK) {
        leme_public_builder_destroy(b);
        return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                   "out of memory");
      }
      for (size_t a = 0; a < root_node->as.call.arg_count; ++a) {
        const struct leme_public_value *raw_arg = NULL;
        enum leme_control_code ev_code =
            eval_node(&ev, root_node->as.call.arg_indices[a], &raw_arg);
        if (ev_code != LEME_CONTROL_OK) {
          leme_public_builder_destroy(b);
          if (error != NULL) {
            error->phase = LEME_CONTROL_PREFLIGHT;
          }
          return ev_code;
        }
        struct leme_public_value *elem = NULL;
        if (raw_arg != NULL) {
          if (raw_arg->owner != b) {
            if (leme_public_clone(b, raw_arg, &elem) != LEME_PUBLIC_OK) {
              leme_public_builder_destroy(b);
              return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                         "out of memory");
            }
          } else {
            void *p = NULL;
            memcpy((void *)&p, (const void *)&raw_arg, sizeof(p));
            elem = (struct leme_public_value *)p;
          }
        }
        if (leme_public_array_set(b, arr, a, elem) != LEME_PUBLIC_OK) {
          leme_public_builder_destroy(b);
          return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                     "out of memory");
        }
      }
      common_args = arr;
    }

    struct leme_control_intent *intent =
        leme_control_alloc(account, sizeof(struct leme_control_intent));
    if (intent == NULL) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                 "out of memory");
    }
    memset(intent, 0, sizeof(*intent));
    intent->opcode = op->opcode;
    intent->has_target = false;
    intent->args = common_args;

    const struct leme_public_value *roots[1] = {common_args};
    size_t root_count = (common_args != NULL && common_args->owner == b) ? 1 : 0;
    if (leme_public_builder_seal(b, roots, root_count) != LEME_PUBLIC_OK) {
      leme_control_free(intent);
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                 "failed to seal args builder");
    }
    *out_builder = b;
    *out_intents = intent;
    *out_count = 1;
    return LEME_CONTROL_OK;
  }

  bool has_dest = false;
  struct leme_control_target dest_target = {0};
  const struct leme_public_value *common_args = NULL;

  if (op->opcode == LEME_CONTROL_OP_MOVE_TO_TAG) {
    if (root_node->as.call.arg_count < 2) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_INVALID_ARGUMENT,
                                 "missing destination tag");
    }
    const struct leme_public_value *dest_val = NULL;
    enum leme_control_code ev_code =
        eval_node(&ev, root_node->as.call.arg_indices[1], &dest_val);
    if (ev_code != LEME_CONTROL_OK) {
      leme_public_builder_destroy(b);
      if (error != NULL) {
        error->phase = LEME_CONTROL_PREFLIGHT;
      }
      return ev_code;
    }
    if (dest_val == NULL || leme_public_kind(dest_val) == LEME_PUBLIC_NULL) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_NOT_FOUND,
                                 "destination tag not found");
    }
    if (leme_public_kind(dest_val) != LEME_PUBLIC_OBJECT) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_TYPE_ERROR,
                                 "destination tag must be object");
    }
    const struct leme_public_value *id_val =
        leme_public_get(dest_val, LEME_PUBLIC_TEXT("id"));
    if (id_val == NULL || leme_public_kind(id_val) != LEME_PUBLIC_STRING) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_TYPE_ERROR,
                                 "destination tag missing id");
    }
    struct leme_public_text id_text = {0};
    leme_public_as_text(id_val, &id_text);
    struct leme_public_id d_id = {0};
    uint16_t d_slot = 0;
    if (!leme_public_parse_id(model, LEME_PUBLIC_TAG, id_text, &d_id,
                              &d_slot)) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_INVALID_ARGUMENT,
                                 "invalid destination tag id");
    }
    dest_target.kind = LEME_PUBLIC_TAG;
    dest_target.id = d_id;
    dest_target.tag_number = d_slot;
    has_dest = true;
  } else if (op->opcode == LEME_CONTROL_OP_MOVE_TO_OUTPUT) {
    if (root_node->as.call.arg_count < 2) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_INVALID_ARGUMENT,
                                 "missing destination output");
    }
    const struct leme_public_value *dest_val = NULL;
    enum leme_control_code ev_code =
        eval_node(&ev, root_node->as.call.arg_indices[1], &dest_val);
    if (ev_code != LEME_CONTROL_OK) {
      leme_public_builder_destroy(b);
      if (error != NULL) {
        error->phase = LEME_CONTROL_PREFLIGHT;
      }
      return ev_code;
    }
    if (dest_val == NULL || leme_public_kind(dest_val) == LEME_PUBLIC_NULL) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_NOT_FOUND,
                                 "destination output not found");
    }
    if (leme_public_kind(dest_val) != LEME_PUBLIC_OBJECT) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_TYPE_ERROR,
                                 "destination output must be object");
    }
    const struct leme_public_value *id_val =
        leme_public_get(dest_val, LEME_PUBLIC_TEXT("id"));
    if (id_val == NULL || leme_public_kind(id_val) != LEME_PUBLIC_STRING) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_TYPE_ERROR,
                                 "destination output missing id");
    }
    struct leme_public_text id_text = {0};
    leme_public_as_text(id_val, &id_text);
    struct leme_public_id d_id = {0};
    uint16_t d_slot = 0;
    if (!leme_public_parse_id(model, LEME_PUBLIC_OUTPUT, id_text, &d_id,
                              &d_slot)) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_INVALID_ARGUMENT,
                                 "invalid destination output id");
    }
    dest_target.kind = LEME_PUBLIC_OUTPUT;
    dest_target.id = d_id;
    dest_target.tag_number = 0;
    has_dest = true;
  } else if (root_node->as.call.arg_count == 2) {
    const struct leme_public_value *raw_arg = NULL;
    enum leme_control_code ev_code = eval_node(
        &ev, root_node->as.call.arg_indices[1], &raw_arg);
    if (ev_code != LEME_CONTROL_OK) {
      leme_public_builder_destroy(b);
      if (error != NULL) {
        error->phase = LEME_CONTROL_PREFLIGHT;
      }
      return ev_code;
    }
    if (raw_arg != NULL && raw_arg->owner != b) {
      struct leme_public_value *cloned = NULL;
      if (leme_public_clone(b, raw_arg, &cloned) != LEME_PUBLIC_OK) {
        leme_public_builder_destroy(b);
        return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                   "out of memory");
      }
      common_args = cloned;
    } else {
      common_args = raw_arg;
    }
  } else if (root_node->as.call.arg_count > 2) {
    const size_t extra_count = root_node->as.call.arg_count - 1;
    struct leme_public_value *arr = NULL;
    if (leme_public_array(b, extra_count, &arr) != LEME_PUBLIC_OK) {
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                 "out of memory");
    }
    for (size_t a = 0; a < extra_count; ++a) {
      const struct leme_public_value *val = NULL;
      enum leme_control_code ev_code = eval_node(
          &ev, root_node->as.call.arg_indices[a + 1], &val);
      if (ev_code != LEME_CONTROL_OK) {
        leme_public_builder_destroy(b);
        if (error != NULL) {
          error->phase = LEME_CONTROL_PREFLIGHT;
        }
        return ev_code;
      }
      const struct leme_public_value *item = val;
      if (val != NULL && val->owner != b) {
        struct leme_public_value *cloned = NULL;
        if (leme_public_clone(b, val, &cloned) != LEME_PUBLIC_OK) {
          leme_public_builder_destroy(b);
          return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                     "out of memory");
        }
        item = cloned;
      }
      if (leme_public_array_set(b, arr, a, item) != LEME_PUBLIC_OK) {
        leme_public_builder_destroy(b);
        return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                   "out of memory");
      }
    }
    common_args = arr;
  }

  const struct leme_public_value *target_val = NULL;
  enum leme_control_code ev_code =
      eval_node(&ev, root_node->as.call.arg_indices[0], &target_val);
  if (ev_code != LEME_CONTROL_OK) {
    leme_public_builder_destroy(b);
    if (error != NULL) {
      error->phase = LEME_CONTROL_PREFLIGHT;
    }
    return ev_code;
  }

  if (target_val == NULL || leme_public_kind(target_val) == LEME_PUBLIC_NULL) {
    leme_public_builder_destroy(b);
    if (is_single) {
      return set_preflight_error(error, LEME_CONTROL_NOT_FOUND,
                                 "target not found");
    }
    return set_preflight_error(error, LEME_CONTROL_TYPE_ERROR,
                               "target must not be null");
  }

  enum leme_public_kind tkind = leme_public_kind(target_val);
  if (tkind != LEME_PUBLIC_OBJECT && tkind != LEME_PUBLIC_ARRAY) {
    leme_public_builder_destroy(b);
    return set_preflight_error(error, LEME_CONTROL_TYPE_ERROR,
                               "invalid target shape");
  }

  size_t raw_count =
      tkind == LEME_PUBLIC_OBJECT ? 1 : leme_public_length(target_val);

  if (is_single && raw_count != 1) {
    leme_public_builder_destroy(b);
    return set_preflight_error(error, LEME_CONTROL_CARDINALITY,
                               "single-target action requires one target");
  }

  if (raw_count > limits->targets) {
    leme_public_builder_destroy(b);
    return set_preflight_error(error, LEME_CONTROL_RESOURCE_LIMIT,
                               "targets limit exceeded");
  }

  if (raw_count == 0) {
    leme_public_builder_destroy(b);
    *out_intents = NULL;
    *out_count = 0;
    return LEME_CONTROL_OK;
  }

  struct leme_control_intent *intents = leme_control_alloc(
      account, raw_count * sizeof(struct leme_control_intent));
  if (intents == NULL) {
    leme_public_builder_destroy(b);
    return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                               "out of memory");
  }
  memset(intents, 0, raw_count * sizeof(struct leme_control_intent));
  size_t num_intents = 0;

  for (size_t k = 0; k < raw_count; ++k) {
    if (leme_control_charge(&ev.meter, 1) != LEME_CONTROL_OK) {
      leme_control_intents_destroy(intents, num_intents);
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_RESOURCE_LIMIT,
                                 "preparation work limit exceeded");
    }
    const struct leme_public_value *elem =
        tkind == LEME_PUBLIC_OBJECT ? target_val : leme_public_at(target_val, k);
    if (elem == NULL || leme_public_kind(elem) != LEME_PUBLIC_OBJECT) {
      leme_control_intents_destroy(intents, num_intents);
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_TYPE_ERROR,
                                 "target element must be object");
    }

    const struct leme_public_value *id_val =
        leme_public_get(elem, LEME_PUBLIC_TEXT("id"));
    if (id_val == NULL || leme_public_kind(id_val) != LEME_PUBLIC_STRING) {
      leme_control_intents_destroy(intents, num_intents);
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_TYPE_ERROR,
                                 "target missing id");
    }

    struct leme_public_text req_id_text = {0};
    leme_public_as_text(id_val, &req_id_text);

    struct leme_public_id req_pid = {0};
    uint16_t req_tag = 0;
    if (!leme_public_parse_id(model, expected_entity, req_id_text, &req_pid,
                              &req_tag)) {
      leme_control_intents_destroy(intents, num_intents);
      leme_public_builder_destroy(b);
      return set_preflight_error(error, LEME_CONTROL_INVALID_ARGUMENT,
                                 "invalid target id format");
    }

    struct leme_control_target eff_target = {
        .kind = expected_entity,
        .id = req_pid,
        .tag_number = req_tag,
    };
    struct leme_public_text eff_id_text = req_id_text;

    if ((op->opcode == LEME_CONTROL_OP_MOVE_TO_TAG ||
         op->opcode == LEME_CONTROL_OP_MOVE_TO_OUTPUT ||
         op->opcode == LEME_CONTROL_OP_SET_STICKY) &&
        expected_entity == LEME_PUBLIC_VIEW) {
      const struct leme_public_value *owner =
          leme_public_get(elem, LEME_PUBLIC_TEXT("owner"));
      if (owner != NULL && leme_public_kind(owner) == LEME_PUBLIC_OBJECT) {
        const struct leme_public_value *root_ref =
            leme_public_get(owner, LEME_PUBLIC_TEXT("root"));
        if (root_ref != NULL &&
            leme_public_kind(root_ref) == LEME_PUBLIC_OBJECT) {
          const struct leme_public_value *root_id =
              leme_public_get(root_ref, LEME_PUBLIC_TEXT("id"));
          if (root_id != NULL &&
              leme_public_kind(root_id) == LEME_PUBLIC_STRING) {
            struct leme_public_text root_id_text = {0};
            leme_public_as_text(root_id, &root_id_text);
            struct leme_public_id r_pid = {0};
            uint16_t r_tag = 0;
            if (leme_public_parse_id(model, LEME_PUBLIC_VIEW, root_id_text,
                                     &r_pid, &r_tag)) {
              eff_target.id = r_pid;
              eff_target.tag_number = 0;
              eff_id_text = root_id_text;
            }
          }
        }
      }
    }

    int found = -1;
    for (size_t i = 0; i < num_intents; ++i) {
      if (intents[i].effective_id.length == eff_id_text.length &&
          memcmp(intents[i].effective_id.data, eff_id_text.data,
                 eff_id_text.length) == 0) {
        found = (int)i;
        break;
      }
    }

    if (found >= 0) {
      bool has_req = false;
      for (size_t r = 0; r < intents[found].requested_count; ++r) {
        if (intents[found].requested_targets[r].length == req_id_text.length &&
            memcmp(intents[found].requested_targets[r].data, req_id_text.data,
                   req_id_text.length) == 0) {
          has_req = true;
          break;
        }
      }

      if (!has_req) {
        size_t old_rc = intents[found].requested_count;
        size_t new_rc = old_rc + 1;
        struct leme_public_text *new_rt = leme_control_alloc(
            account, new_rc * sizeof(struct leme_public_text));
        if (new_rt == NULL) {
          leme_control_intents_destroy(intents, num_intents);
          leme_public_builder_destroy(b);
          return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                     "out of memory");
        }
        for (size_t r = 0; r < old_rc; ++r) {
          new_rt[r] = intents[found].requested_targets[r];
        }
        new_rt[old_rc] = dup_text(account, req_id_text);
        if (new_rt[old_rc].data == NULL) {
          leme_control_free(new_rt);
          leme_control_intents_destroy(intents, num_intents);
          leme_public_builder_destroy(b);
          return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                     "out of memory");
        }
        leme_control_free(intents[found].requested_targets);
        intents[found].requested_targets = new_rt;
        intents[found].requested_count = new_rc;
      }
    } else {
      if (num_intents >= limits->targets) {
        leme_control_intents_destroy(intents, num_intents);
        leme_public_builder_destroy(b);
        return set_preflight_error(error, LEME_CONTROL_RESOURCE_LIMIT,
                                   "targets limit exceeded");
      }

      struct leme_public_text *rt =
          leme_control_alloc(account, sizeof(struct leme_public_text));
      if (rt == NULL) {
        leme_control_intents_destroy(intents, num_intents);
        leme_public_builder_destroy(b);
        return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                   "out of memory");
      }

      rt[0] = dup_text(account, req_id_text);
      if (rt[0].data == NULL) {
        leme_control_free(rt);
        leme_control_intents_destroy(intents, num_intents);
        leme_public_builder_destroy(b);
        return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                   "out of memory");
      }

      struct leme_public_text eff_copy = dup_text(account, eff_id_text);
      if (eff_copy.data == NULL) {
        leme_control_free(unconst(rt[0].data));
        leme_control_free(rt);
        leme_control_intents_destroy(intents, num_intents);
        leme_public_builder_destroy(b);
        return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                   "out of memory");
      }

      intents[num_intents].opcode = op->opcode;
      intents[num_intents].has_target = true;
      intents[num_intents].target = eff_target;
      intents[num_intents].effective_id = eff_copy;
      intents[num_intents].requested_id = rt[0];
      intents[num_intents].requested_count = 1;
      intents[num_intents].requested_targets = rt;
      intents[num_intents].has_destination = has_dest;
      intents[num_intents].destination = dest_target;
      intents[num_intents].args = common_args;
      num_intents++;
    }
  }

  if (is_single && num_intents != 1) {
    leme_control_intents_destroy(intents, num_intents);
    leme_public_builder_destroy(b);
    return set_preflight_error(error, LEME_CONTROL_CARDINALITY,
                               "single-target action requires one target");
  }

  const struct leme_public_value *roots[1] = {common_args};
  size_t root_count = (common_args != NULL && common_args->owner == b) ? 1 : 0;
  if (leme_public_builder_seal(b, roots, root_count) != LEME_PUBLIC_OK) {
    leme_control_intents_destroy(intents, num_intents);
    leme_public_builder_destroy(b);
    return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                               "failed to seal args builder");
  }

  *out_builder = b;
  *out_intents = intents;
  *out_count = num_intents;
  return LEME_CONTROL_OK;
}
