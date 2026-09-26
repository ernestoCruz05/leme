#include "timao/lower-internal.h"
#include "timao/language-internal.h"
#include "timao/diagnostic.h"
#include "control/registry.h"
#include <string.h>
#include <stdlib.h>

struct lowering {
  struct timao_execution *execution;
  const struct timao_program *program;
  struct timao_lowered_owner *owner;
  struct timao_diagnostic *error;
};
static enum timao_status public_error(struct lowering *lower,
                                      enum leme_public_status status) {
  if (lower->execution->vm->meter.failure != NULL)
    return timao_charge(&lower->execution->vm->meter, 0, lower->error);
  return timao_error(lower->error,
                     status == LEME_PUBLIC_OOM     ? "out_of_memory"
                     : status == LEME_PUBLIC_LIMIT ? "resource_limit"
                                                   : "invalid_argument",
                     "remote expression construction failed");
}
static enum leme_public_status work(void *context, size_t units) {
  struct lowering *lower = context;
  return timao_charge(&lower->execution->vm->meter, units, lower->error) ==
                 TIMAO_OK
             ? LEME_PUBLIC_OK
             : LEME_PUBLIC_LIMIT;
}
static enum timao_status entry(struct lowering *lower,
                               const struct timao_node *node, size_t parent,
                               size_t argument, size_t *index) {
  struct timao_lowered_owner *owner = lower->owner;
  if (owner->count == 4096)
    return timao_error(lower->error, "resource_limit",
                       "remote expression node limit exceeded");
  if (owner->count == owner->capacity) {
    const size_t capacity = owner->capacity == 0 ? 32 : owner->capacity * 2;
    struct timao_source_entry *map = timao_memory_alloc(
        owner->account, capacity * sizeof(*map), lower->error);
    if (map == NULL)
      return TIMAO_ERROR;
    for (size_t offset = 0; offset < owner->count;) {
      if (timao_charge(&lower->execution->vm->meter, 1, lower->error) !=
          TIMAO_OK) {
        timao_memory_free(map);
        return TIMAO_ERROR;
      }
      const size_t count =
          owner->count - offset > 128 ? 128 : owner->count - offset;
      memcpy(map + offset, owner->map + offset, count * sizeof(*map));
      offset += count;
    }
    timao_memory_free(owner->map);
    owner->map = map;
    owner->capacity = capacity;
  }
  *index = owner->count;
  owner->map[owner->count++] = (struct timao_source_entry){.span = node->span,
                                                           .parent = parent,
                                                           .argument = argument,
                                                           .kind = node->kind};
  return TIMAO_OK;
}
static enum timao_status lower_node(struct lowering *lower, uint32_t node_index,
                                    size_t parent, size_t argument,
                                    size_t depth, bool item, bool action,
                                    struct leme_public_value **out);
static enum timao_status lower_node_impl(struct lowering *lower,
                                         uint32_t node_index, size_t parent,
                                         size_t argument, size_t depth,
                                         bool item, bool action,
                                         struct leme_public_value **out) {
  if (!timao_index_valid(node_index, lower->program->count) ||
      (parent != SIZE_MAX && !timao_index_valid(parent, lower->owner->count)))
    return timao_error(lower->error, "invalid_argument",
                       "invalid lowering location");
  const struct timao_node *node = timao_node_at(lower->program, node_index);
  if (node == NULL)
    return timao_error(lower->error, "invalid_argument",
                       "invalid remote expression node");
  if (!timao_index_valid(depth, 65) || !timao_index_valid(argument, 4096))
    return timao_error(lower->error, "resource_limit",
                       "remote expression depth or argument limit exceeded");
  if (timao_charge(&lower->execution->vm->meter, 1, lower->error) != TIMAO_OK)
    return TIMAO_ERROR;
  size_t index = 0;
  if (entry(lower, node, parent, argument, &index) != TIMAO_OK)
    return TIMAO_ERROR;
  struct leme_public_builder *builder = lower->owner->builder;
  struct leme_public_value *object = NULL, *value = NULL;
  enum leme_public_status status = leme_public_object(
      builder, node->kind == TIMAO_TOKEN_OPEN ? 2 : 1, &object);
  if (status != LEME_PUBLIC_OK)
    return public_error(lower, status);
  if (node->kind == TIMAO_TOKEN_OPEN) {
    const struct timao_node *head = timao_node_at(lower->program, node->first);
    const struct leme_control_operator *op =
        head == NULL ? NULL : leme_control_operator_find(head->text);
    if (op == NULL || (op->effect == LEME_CONTROL_EFFECT_ACTION) != action ||
        (op->opcode == LEME_CONTROL_OP_ITEM && !item))
      return timao_error(lower->error, "invalid_remote_expression",
                         "operator is not valid at this remote boundary");
    if (node->count - 1 < op->min_args || node->count - 1 > op->max_args)
      return timao_error(lower->error, "arity_error",
                         "remote operator arity mismatch");
    status = leme_public_string(builder, head->text, false, &value);
    if (status == LEME_PUBLIC_OK)
      status = leme_public_object_set(builder, object, LEME_PUBLIC_TEXT("call"),
                                      value);
    if (status == LEME_PUBLIC_OK)
      status = leme_public_array(builder, node->count - 1, &value);
    if (status != LEME_PUBLIC_OK)
      return public_error(lower, status);
    uint32_t child = head->next;
    for (size_t i = 0; i < node->count - 1; ++i) {
      struct leme_public_value *arg = NULL;
      if (lower_node(lower, child, index, i, depth + 1,
                     item || leme_control_operator_arg_is_item_scope(op, i),
                     false, &arg) != TIMAO_OK)
        return TIMAO_ERROR;
      status = leme_public_array_set(builder, value, i, arg);
      if (status != LEME_PUBLIC_OK)
        return public_error(lower, status);
      child = lower->program->nodes[child].next;
    }
    status = leme_public_object_set(builder, object, LEME_PUBLIC_TEXT("args"),
                                    value);
  } else if (action)
    return timao_error(lower->error, "invalid_remote_expression",
                       "act requires a root action call");
  else if (node->kind == TIMAO_TOKEN_FIELD) {
    if (!item)
      return timao_error(lower->error, "invalid_remote_expression",
                         "remote field outside item scope");
    size_t count = 0;
    for (size_t i = 0; i < node->text.length; ++i) {
      if (i % 128 == 0 && timao_charge(&lower->execution->vm->meter, 1,
                                       lower->error) != TIMAO_OK)
        return TIMAO_ERROR;
      if (node->text.data[i] == '.')
        ++count;
    }
    if (count > 16)
      return timao_error(lower->error, "resource_limit",
                         "remote field path limit exceeded");
    status = leme_public_array(builder, count, &value);
    if (status != LEME_PUBLIC_OK)
      return public_error(lower, status);
    size_t start = 1, component = 0;
    for (size_t i = 1; i <= node->text.length; ++i) {
      if (i % 128 == 0 && timao_charge(&lower->execution->vm->meter, 1,
                                       lower->error) != TIMAO_OK)
        return TIMAO_ERROR;
      if (i != node->text.length && node->text.data[i] != '.')
        continue;
      struct leme_public_value *part = NULL;
      status = leme_public_string(
          builder,
          (struct leme_public_text){node->text.data + start, i - start}, false,
          &part);
      if (status == LEME_PUBLIC_OK)
        status = leme_public_array_set(builder, value, component++, part);
      if (status != LEME_PUBLIC_OK)
        return public_error(lower, status);
      start = i + 1;
    }
    status = leme_public_object_set(builder, object, LEME_PUBLIC_TEXT("field"),
                                    value);
  } else {
    switch (node->kind) {
    case TIMAO_TOKEN_NULL:
      status = leme_public_null(builder, &value);
      break;
    case TIMAO_TOKEN_TRUE:
    case TIMAO_TOKEN_FALSE:
      status =
          leme_public_boolean(builder, node->kind == TIMAO_TOKEN_TRUE, &value);
      break;
    case TIMAO_TOKEN_NUMBER:
      status = leme_public_number(builder, node->number, &value);
      break;
    case TIMAO_TOKEN_STRING:
      status = leme_public_string(builder, node->text, false, &value);
      break;
    case TIMAO_TOKEN_NAME: {
      const struct timao_value *captured = NULL;
      struct timao_environment *environment = lower->execution->environment;
      if (environment == NULL)
        environment = lower->execution->vm->globals;
      if (timao_environment_get(lower->execution->vm, environment, node->text,
                                &captured, lower->error) != TIMAO_OK)
        return TIMAO_ERROR;
      if (timao_export(lower->execution, captured, builder, &value,
                       lower->error) != TIMAO_OK)
        return TIMAO_ERROR;
      const struct leme_public_work meter = {.context = lower, .step = work};
      leme_public_builder_set_work(builder, &meter);
      break;
    }
    default:
      return timao_error(lower->error, "invalid_remote_expression",
                         "invalid remote literal");
    }
    if (status != LEME_PUBLIC_OK)
      return public_error(lower, status);
    status = leme_public_object_set(builder, object,
                                    LEME_PUBLIC_TEXT("literal"), value);
  }
  if (status != LEME_PUBLIC_OK)
    return public_error(lower, status);
  *out = object;
  return TIMAO_OK;
}
static enum timao_status lower_node(struct lowering *lower, uint32_t node_index,
                                    size_t parent, size_t argument,
                                    size_t depth, bool item, bool action,
                                    struct leme_public_value **out) {
  const enum timao_status status = lower_node_impl(
      lower, node_index, parent, argument, depth, item, action, out);
  const struct timao_node *node = timao_node_at(lower->program, node_index);
  if (status != TIMAO_OK && lower->error->source == NULL && node != NULL)
    timao_diagnostic_at(lower->error, lower->program->source, node->span);
  return status;
}
enum timao_status timao_lower(struct timao_execution *execution,
                              const struct timao_program *program,
                              uint32_t node, enum timao_boundary boundary,
                              struct timao_lowered **out,
                              struct timao_diagnostic *error) {
  if (out == NULL)
    return timao_error(error, "invalid_argument", "missing lowered output");
  *out = NULL;
  if (execution == NULL || execution->vm == NULL || program == NULL ||
      boundary < TIMAO_QUERY || boundary > TIMAO_WATCH_BOUNDARY)
    return timao_error(error, "invalid_argument", "invalid lowering input");
  struct timao_diagnostic fallback = {0};
  if (error == NULL)
    error = &fallback;
  timao_diagnostic_destroy(error);
  struct timao_lowered_owner *owner =
      timao_memory_alloc(execution->vm->heap.account, sizeof(*owner), error);
  if (owner == NULL)
    return TIMAO_ERROR;
  owner->view.owner = owner;
  owner->references = 1;
  owner->account = execution->vm->heap.account;
  owner->source = program->source;
  timao_source_ref(owner->source);
  struct lowering lower = {.execution = execution,
                           .program = program,
                           .owner = owner,
                           .error = error};
  const enum leme_public_status created = leme_public_builder_create_budget(
      owner->account, 1048576, &owner->builder);
  if (created != LEME_PUBLIC_OK) {
    public_error(&lower, created);
    timao_lowered_unref(&owner->view);
    return TIMAO_ERROR;
  }
  const struct leme_public_work meter = {.context = &lower, .step = work};
  leme_public_builder_set_work(owner->builder, &meter);
  struct leme_public_value *value = NULL;
  enum timao_status status = lower_node(&lower, node, SIZE_MAX, 0, 1, false,
                                        boundary == TIMAO_ACT, &value);
  owner->value = value;
  if (status == TIMAO_OK) {
    const enum leme_public_status sealed =
        leme_public_builder_seal(owner->builder, &owner->value, 1);
    if (sealed != LEME_PUBLIC_OK)
      status = public_error(&lower, sealed);
  }
  leme_public_builder_set_work(owner->builder, NULL);
  if (status != TIMAO_OK) {
    timao_lowered_unref(&owner->view);
    if (error == &fallback)
      timao_diagnostic_destroy(&fallback);
    return status;
  }
  *out = &owner->view;
  return TIMAO_OK;
}
const struct leme_public_value *
timao_lowered_value(const struct timao_lowered *lowered) {
  return lowered == NULL ? NULL : lowered->owner->value;
}
struct timao_source *timao_lowered_source(const struct timao_lowered *lowered) {
  return lowered == NULL ? NULL : lowered->owner->source;
}
void timao_lowered_ref(const struct timao_lowered *lowered) {
  if (lowered == NULL)
    return;
  if (lowered->owner->references == SIZE_MAX)
    abort();
  ++lowered->owner->references;
}
void timao_lowered_unref(const struct timao_lowered *lowered) {
  if (lowered == NULL || --lowered->owner->references != 0)
    return;
  struct timao_lowered_owner *owner = lowered->owner;
  leme_public_builder_destroy(owner->builder);
  timao_source_unref(owner->source);
  timao_memory_free(owner->map);
  timao_memory_free(owner);
}
