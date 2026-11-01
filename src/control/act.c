#include "control/action.h"
#include "control/eval-internal.h"
#include "public/model-internal.h"
#include "public/value-internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define LEME_CONTROL_ACT_ARGS 4u

static enum leme_control_code report(struct leme_control_error *error,
                                     enum leme_control_code code,
                                     enum leme_control_phase phase,
                                     const char *message) {
  if (error != NULL) {
    error->code = code;
    error->phase = phase;
    (void)snprintf(error->message, sizeof(error->message), "%s", message);
    (void)snprintf(error->expr_path, sizeof(error->expr_path), "/expr");
  }
  return code;
}

enum leme_control_code leme_control_refuse(struct leme_control_error *error,
                                           enum leme_control_code code,
                                           const char *message) {
  return report(error, code, LEME_CONTROL_PREFLIGHT, message);
}

enum leme_control_code leme_control_fail(struct leme_control_error *error,
                                         enum leme_control_code code,
                                         const char *message) {
  return report(error, code, LEME_CONTROL_EXECUTE, message);
}

static bool targeted(enum leme_control_opcode opcode, bool *single,
                     enum leme_public_entity *entity) {
  *single = true;
  switch (opcode) {
  case LEME_CONTROL_OP_CONTAINER_LAYOUT:
  case LEME_CONTROL_OP_CONTAINER_DETACH:
  case LEME_CONTROL_OP_CONTAINER_DISSOLVE:
  case LEME_CONTROL_OP_CONTAINER_JOIN:
  case LEME_CONTROL_OP_FOCUS:
  case LEME_CONTROL_OP_RESIZE:
    *entity = LEME_PUBLIC_VIEW;
    return true;
  case LEME_CONTROL_OP_FOCUS_TAG:
    *entity = LEME_PUBLIC_TAG;
    return true;
  case LEME_CONTROL_OP_FOCUS_OUTPUT:
  case LEME_CONTROL_OP_CONFIGURE_OUTPUT:
    *entity = LEME_PUBLIC_OUTPUT;
    return true;
  default:
    break;
  }
  *single = false;
  switch (opcode) {
  case LEME_CONTROL_OP_MOVE_TO_TAG:
  case LEME_CONTROL_OP_MOVE_TO_OUTPUT:
  case LEME_CONTROL_OP_SET_MINIMIZED:
  case LEME_CONTROL_OP_SET_FLOATING:
  case LEME_CONTROL_OP_SET_FULLSCREEN:
  case LEME_CONTROL_OP_SET_STICKY:
  case LEME_CONTROL_OP_CLOSE:
    *entity = LEME_PUBLIC_VIEW;
    return true;
  case LEME_CONTROL_OP_SET_LAYOUT:
    *entity = LEME_PUBLIC_TAG;
    return true;
  case LEME_CONTROL_OP_SET_INPUT:
    *entity = LEME_PUBLIC_INPUT;
    return true;
  case LEME_CONTROL_OP_SET_OUTPUT_POWER:
    *entity = LEME_PUBLIC_OUTPUT;
    return true;
  default:
    return false;
  }
}

static enum leme_control_code parse_target(
    const struct leme_public_model *model, enum leme_public_entity kind,
    const struct leme_public_value *record, struct leme_control_target *out,
    struct leme_control_error *error) {
  if (leme_public_kind(record) != LEME_PUBLIC_OBJECT)
    return leme_control_refuse(error, LEME_CONTROL_TYPE_ERROR,
                               "target element must be object");
  struct leme_public_text id = {0};
  if (leme_public_as_text(leme_public_get(record, LEME_PUBLIC_TEXT("id")),
                          &id) != LEME_PUBLIC_OK)
    return leme_control_refuse(error, LEME_CONTROL_TYPE_ERROR,
                               "target missing id");
  *out = (struct leme_control_target){.kind = kind};
  if (!leme_public_parse_id(model, kind, id, &out->id, &out->tag_number))
    return leme_control_refuse(error, LEME_CONTROL_INVALID_ARGUMENT,
                               "invalid target id format");
  return LEME_CONTROL_OK;
}

static void follow_root(const struct leme_public_model *model,
                        const struct leme_public_value *record,
                        struct leme_control_target *target) {
  const struct leme_public_value *root =
      leme_public_get(leme_public_get(record, LEME_PUBLIC_TEXT("owner")),
                      LEME_PUBLIC_TEXT("root"));
  struct leme_public_text id = {0};
  struct leme_public_id serial = {0};
  uint16_t slot = 0;
  if (leme_public_as_text(leme_public_get(root, LEME_PUBLIC_TEXT("id")), &id) ==
          LEME_PUBLIC_OK &&
      leme_public_parse_id(model, LEME_PUBLIC_VIEW, id, &serial, &slot)) {
    target->id = serial;
    target->tag_number = 0;
  }
}

static enum leme_control_code parse_destination(
    const struct leme_public_model *model, enum leme_control_opcode opcode,
    const struct leme_public_value *value, struct leme_control_target *out,
    struct leme_control_error *error) {
  const bool tag = opcode == LEME_CONTROL_OP_MOVE_TO_TAG;
  if (value == NULL || leme_public_kind(value) == LEME_PUBLIC_NULL)
    return leme_control_refuse(error, LEME_CONTROL_NOT_FOUND,
                               tag ? "destination tag not found"
                                   : "destination output not found");
  const enum leme_control_code code = parse_target(
      model, tag ? LEME_PUBLIC_TAG : LEME_PUBLIC_OUTPUT, value, out, error);
  if (code != LEME_CONTROL_OK && error != NULL)
    (void)snprintf(error->message, sizeof(error->message),
                   tag ? "invalid destination tag"
                       : "invalid destination "
                         "output");
  return code;
}

static bool seen_before(const uint64_t *seen, size_t count, uint64_t serial) {
  for (size_t i = 0; i < count; ++i)
    if (seen[i] == serial)
      return true;
  return false;
}

static enum leme_control_code
run_targets(const struct leme_control_domain *domain,
            const struct leme_public_model *model,
            const struct leme_control_limits *limits, bool single,
            enum leme_public_entity entity,
            const struct leme_public_value *targets,
            struct leme_control_call *call, size_t *affected,
            struct leme_control_error *error) {
  if (targets == NULL || leme_public_kind(targets) == LEME_PUBLIC_NULL)
    return single ? leme_control_refuse(error, LEME_CONTROL_NOT_FOUND,
                                        "target not found")
                  : leme_control_refuse(error, LEME_CONTROL_TYPE_ERROR,
                                        "target must not be null");
  const enum leme_public_kind kind = leme_public_kind(targets);
  if (kind != LEME_PUBLIC_OBJECT && kind != LEME_PUBLIC_ARRAY)
    return leme_control_refuse(error, LEME_CONTROL_TYPE_ERROR,
                               "invalid target shape");
  const size_t count =
      kind == LEME_PUBLIC_OBJECT ? 1 : leme_public_length(targets);
  if (single && count != 1)
    return leme_control_refuse(error, LEME_CONTROL_CARDINALITY,
                               "single-target action requires one target");
  if (count > limits->targets)
    return leme_control_refuse(error, LEME_CONTROL_RESOURCE_LIMIT,
                               "targets limit exceeded");
  if (count == 0)
    return LEME_CONTROL_OK;
  const bool roots = entity == LEME_PUBLIC_VIEW &&
                     (call->opcode == LEME_CONTROL_OP_MOVE_TO_TAG ||
                      call->opcode == LEME_CONTROL_OP_MOVE_TO_OUTPUT ||
                      call->opcode == LEME_CONTROL_OP_SET_STICKY);
  uint64_t *seen = calloc(count, sizeof(*seen));
  if (seen == NULL)
    return leme_control_refuse(error, LEME_CONTROL_OUT_OF_MEMORY,
                               "out of memory");
  size_t ran = 0;
  enum leme_control_code code = LEME_CONTROL_OK;
  for (size_t i = 0; i < count && code == LEME_CONTROL_OK; ++i) {
    const struct leme_public_value *record =
        kind == LEME_PUBLIC_OBJECT ? targets : leme_public_at(targets, i);
    struct leme_control_target target = {0};
    code = parse_target(model, entity, record, &target, error);
    if (code != LEME_CONTROL_OK)
      break;
    if (roots)
      follow_root(model, record, &target);
    if (seen_before(seen, ran, target.id.serial) && entity == LEME_PUBLIC_VIEW)
      continue;
    seen[ran++] = target.id.serial;
    call->target = &target;
    bool applied = false;
    code = domain->run(domain->context, call, &applied, error);
    if (code == LEME_CONTROL_OK && applied)
      ++*affected;
  }
  call->target = NULL;
  free(seen);
  return code;
}

enum leme_control_code
leme_control_act(struct leme_control_context *context,
                 const struct leme_control_program *program,
                 const struct leme_public_snapshot *snapshot, size_t *affected,
                 struct leme_control_error *error) {
  if (context == NULL || program == NULL || affected == NULL)
    return LEME_CONTROL_INVALID_ARGUMENT;
  *affected = 0;
  const struct leme_control_node *root = leme_control_program_node(
      program, leme_control_program_root_index(program));
  if (root == NULL || root->kind != LEME_CONTROL_NODE_CALL ||
      root->as.call.op == NULL ||
      root->as.call.op->effect != LEME_CONTROL_EFFECT_ACTION)
    return LEME_CONTROL_INVALID_REQUEST;
  const enum leme_control_opcode opcode = root->as.call.op->opcode;
  const size_t argc = root->as.call.arg_count;
  if (argc > LEME_CONTROL_ACT_ARGS)
    return leme_control_refuse(error, LEME_CONTROL_INVALID_REQUEST,
                               "too many action arguments");

  const struct leme_control_domain *domain =
      leme_control_context_domain(context);
  if (domain == NULL || domain->run == NULL)
    return leme_control_refuse(error, LEME_CONTROL_UNSUPPORTED,
                               "actions not supported");
  const struct leme_public_source *source =
      leme_control_context_source(context);
  if (source != NULL && source->locked != NULL &&
      source->locked(source->context))
    return leme_control_refuse(error, LEME_CONTROL_SESSION_LOCKED,
                               "session is locked");

  const struct leme_control_limits *limits =
      leme_control_context_limits(context);
  struct leme_public_builder *builder = NULL;
  if (leme_public_builder_create(limits->response_bytes, NULL, &builder) !=
      LEME_PUBLIC_OK)
    return leme_control_refuse(error, LEME_CONTROL_OUT_OF_MEMORY,
                               "out of memory");
  struct evaluator ev = {
      .context = context,
      .program = program,
      .snapshot = snapshot,
      .builder = builder,
      .limits = limits,
      .meter = {.remaining = limits->work_units},
      .error = error,
  };

  const struct leme_public_value *values[LEME_CONTROL_ACT_ARGS] = {0};
  const struct leme_public_value *owned[LEME_CONTROL_ACT_ARGS] = {0};
  size_t owned_count = 0;
  enum leme_control_code code = LEME_CONTROL_OK;
  for (size_t i = 0; i < argc; ++i) {
    code = eval_node(&ev, root->as.call.arg_indices[i], &values[i]);
    if (code != LEME_CONTROL_OK) {
      if (error != NULL)
        error->phase = LEME_CONTROL_PREFLIGHT;
      goto done;
    }
    if (values[i] != NULL && values[i]->owner == builder)
      owned[owned_count++] = values[i];
  }
  if (leme_public_builder_seal(builder, owned, owned_count) != LEME_PUBLIC_OK) {
    code =
        leme_control_refuse(error, LEME_CONTROL_OUT_OF_MEMORY, "out of memory");
    goto done;
  }

  const struct leme_public_model *model = leme_control_context_model(context);
  bool single = false;
  enum leme_public_entity entity = LEME_PUBLIC_VIEW;
  const bool has_target = targeted(opcode, &single, &entity);
  const bool moves = opcode == LEME_CONTROL_OP_MOVE_TO_TAG ||
                     opcode == LEME_CONTROL_OP_MOVE_TO_OUTPUT;
  const size_t first = has_target ? (moves ? 2u : 1u) : 0u;
  if (argc < first) {
    code = leme_control_refuse(error, LEME_CONTROL_INVALID_REQUEST,
                               "missing action arguments");
    goto done;
  }
  struct leme_control_target destination = {0};
  if (moves) {
    code = parse_destination(model, opcode, values[1], &destination, error);
    if (code != LEME_CONTROL_OK)
      goto done;
  }
  struct leme_control_call call = {
      .opcode = opcode,
      .destination = moves ? &destination : NULL,
      .args = values + first,
      .count = argc - first,
  };
  if (has_target) {
    code = run_targets(domain, model, limits, single, entity, values[0], &call,
                       affected, error);
  } else {
    bool applied = false;
    code = domain->run(domain->context, &call, &applied, error);
    if (code == LEME_CONTROL_OK && applied)
      *affected = 1;
  }
done:
  leme_public_builder_destroy(builder);
  return code;
}
