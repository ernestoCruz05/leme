#include "timao/language-internal.h"
#include "timao/diagnostic.h"

struct import_frame {
  const struct leme_public_value *input;
  const struct timao_value *value;
  struct timao_value *items;
  struct timao_heap_root root;
  size_t count;
  size_t cursor;
  enum leme_public_kind kind;
  bool entered;
};

struct export_frame {
  const struct timao_value *input;
  struct leme_public_value *value;
  size_t count;
  size_t cursor;
  bool entered;
};

static enum timao_status public_error(enum leme_public_status status,
                                      struct timao_diagnostic *error) {
  timao_error(error,
              status == LEME_PUBLIC_OOM     ? "out_of_memory"
              : status == LEME_PUBLIC_LIMIT ? "resource_limit"
                                            : "type_error",
              "JSON conversion failed");
  return TIMAO_ERROR;
}

static enum timao_status import_object(struct timao_vm *vm,
                                       struct import_frame *frame,
                                       struct timao_diagnostic *error);

static enum timao_status import_begin(struct timao_vm *vm,
                                      struct import_frame *frame,
                                      struct timao_diagnostic *error) {
  if (frame->input == NULL)
    return public_error(LEME_PUBLIC_INVALID, error);
  if (timao_charge(&vm->meter, 1, error) != TIMAO_OK)
    return TIMAO_ERROR;
  frame->kind = leme_public_kind(frame->input);
  switch (frame->kind) {
  case LEME_PUBLIC_NULL:
    return timao_value_null(vm, &frame->value, error);
  case LEME_PUBLIC_BOOLEAN: {
    bool value = false;
    if (leme_public_as_bool(frame->input, &value) != LEME_PUBLIC_OK)
      return public_error(LEME_PUBLIC_INVALID, error);
    return timao_value_boolean(vm, value, &frame->value, error);
  }
  case LEME_PUBLIC_NUMBER: {
    double value = 0;
    if (leme_public_as_number(frame->input, &value) != LEME_PUBLIC_OK)
      return public_error(LEME_PUBLIC_INVALID, error);
    return timao_value_number(vm, value, &frame->value, error);
  }
  case LEME_PUBLIC_STRING: {
    struct leme_public_text text = {0};
    if (leme_public_as_text(frame->input, &text) != LEME_PUBLIC_OK)
      return public_error(LEME_PUBLIC_INVALID, error);
    return timao_value_string(vm, text, &frame->value, error);
  }
  case LEME_PUBLIC_ARRAY:
  case LEME_PUBLIC_OBJECT:
    frame->count = leme_public_length(frame->input);
    if (frame->kind == LEME_PUBLIC_OBJECT && frame->count == 0)
      return timao_value_object(vm, NULL, 0, &frame->value, error);
    if (frame->kind == LEME_PUBLIC_OBJECT) {
      if (import_object(vm, frame, error) != TIMAO_OK)
        return TIMAO_ERROR;
    } else if (timao_array_builder(vm, frame->count, &frame->items, error) !=
               TIMAO_OK)
      return TIMAO_ERROR;
    timao_heap_root_add(&vm->heap, &frame->root, frame->items->allocation);
    return TIMAO_OK;
  }
  return public_error(LEME_PUBLIC_INVALID, error);
}

static enum timao_status import_object(struct timao_vm *vm,
                                       struct import_frame *frame,
                                       struct timao_diagnostic *error) {
  if (frame->count > SIZE_MAX / sizeof(struct timao_member))
    return public_error(LEME_PUBLIC_LIMIT, error);
  struct timao_member *members = timao_memory_alloc(
      vm->heap.account, frame->count * sizeof(*members), error);
  if (members == NULL)
    return TIMAO_ERROR;
  enum timao_status status = TIMAO_ERROR;
  for (size_t i = 0; i < frame->count; ++i) {
    if (timao_charge(&vm->meter, 1, error) != TIMAO_OK)
      goto done;
    members[i].key = leme_public_key_at(frame->input, i);
  }
  status =
      timao_object_builder(vm, members, frame->count, &frame->items, error);
done:
  timao_memory_free(members);
  return status;
}

static enum timao_status export_begin(struct timao_execution *execution,
                                      struct leme_public_builder *builder,
                                      struct export_frame *frame,
                                      struct timao_diagnostic *error) {
  const struct timao_value *input = frame->input;
  if (input == NULL ||
      !timao_heap_owns(&execution->vm->heap, input->allocation))
    return timao_error(error, "invalid_argument", "foreign JSON value");
  if (timao_charge(&execution->vm->meter, 1, error) != TIMAO_OK)
    return TIMAO_ERROR;
  enum leme_public_status status = LEME_PUBLIC_INVALID;
  switch (input->kind) {
  case TIMAO_NULL:
    status = leme_public_null(builder, &frame->value);
    break;
  case TIMAO_BOOLEAN:
    status = leme_public_boolean(builder, input->as.boolean, &frame->value);
    break;
  case TIMAO_NUMBER:
    status = leme_public_number(builder, input->as.number, &frame->value);
    break;
  case TIMAO_STRING:
    status = leme_public_string(builder, input->as.text, false, &frame->value);
    break;
  case TIMAO_WATCH:
  case TIMAO_CALLABLE:
    return timao_error(error, "type_error", "local handles are not JSON data");
  case TIMAO_ARRAY:
    frame->count = input->as.array.count;
    status = leme_public_array(builder, frame->count, &frame->value);
    break;
  case TIMAO_OBJECT:
    frame->count = input->as.object.count;
    status = leme_public_object(builder, frame->count, &frame->value);
    break;
  }
  return status == LEME_PUBLIC_OK ? TIMAO_OK : public_error(status, error);
}

static enum leme_public_status conversion_work(void *context, size_t units) {
  struct timao_execution *execution = context;
  return timao_charge(&execution->vm->meter, units, NULL) == TIMAO_OK
             ? LEME_PUBLIC_OK
             : LEME_PUBLIC_LIMIT;
}

enum timao_status timao_import(struct timao_execution *execution,
                               const struct leme_public_value *input,
                               const struct timao_value **out,
                               struct timao_diagnostic *error) {
  if (out == NULL)
    return timao_error(error, "invalid_argument", "missing import output");
  *out = NULL;
  if (execution == NULL || execution->vm == NULL || input == NULL)
    return timao_error(error, "invalid_argument", "invalid import input");
  struct timao_vm *vm = execution->vm;
  if (timao_charge(&vm->meter, 0, error) != TIMAO_OK)
    return TIMAO_ERROR;
  struct import_frame *frames = timao_memory_alloc(
      vm->heap.account, LEME_PUBLIC_MAX_DEPTH * sizeof(*frames), error);
  if (frames == NULL)
    return TIMAO_ERROR;
  frames[0].input = input;
  size_t depth = 1;
  enum timao_status status = TIMAO_OK;
  while (depth != 0) {
    struct import_frame *frame = &frames[depth - 1];
    if (!frame->entered) {
      status = import_begin(vm, frame, error);
      if (status != TIMAO_OK)
        break;
      frame->entered = true;
    }
    if (frame->cursor < frame->count) {
      if (depth == LEME_PUBLIC_MAX_DEPTH) {
        status = public_error(LEME_PUBLIC_LIMIT, error);
        break;
      }
      const struct leme_public_value *child =
          frame->kind == LEME_PUBLIC_ARRAY
              ? leme_public_at(frame->input, frame->cursor)
              : leme_public_member_at(frame->input, frame->cursor);
      frames[depth++] = (struct import_frame){.input = child};
      continue;
    }
    if (frame->items != NULL)
      frame->value = frame->items;
    if (depth == 1) {
      *out = frame->value;
    } else {
      struct import_frame *parent = &frames[depth - 2];
      if (parent->kind == LEME_PUBLIC_ARRAY)
        parent->items->as.array.items[parent->cursor] = frame->value;
      else
        parent->items->as.object.members[parent->cursor].value = frame->value;
      ++parent->cursor;
    }
    if (frame->root.object != NULL)
      timao_heap_root_remove(&vm->heap, &frame->root);
    --depth;
  }
  while (depth != 0) {
    struct import_frame *frame = &frames[--depth];
    if (frame->root.object != NULL)
      timao_heap_root_remove(&vm->heap, &frame->root);
  }
  timao_memory_free(frames);
  return status;
}

enum timao_status timao_export(struct timao_execution *execution,
                               const struct timao_value *input,
                               struct leme_public_builder *builder,
                               struct leme_public_value **out,
                               struct timao_diagnostic *error) {
  if (out == NULL)
    return timao_error(error, "invalid_argument", "missing export output");
  *out = NULL;
  if (execution == NULL || execution->vm == NULL || builder == NULL)
    return timao_error(error, "invalid_argument", "invalid export input");
  struct timao_vm *vm = execution->vm;
  if (input == NULL || !timao_heap_owns(&vm->heap, input->allocation))
    return timao_error(error, "invalid_argument", "foreign JSON value");
  if (timao_charge(&vm->meter, 0, error) != TIMAO_OK)
    return TIMAO_ERROR;
  struct timao_heap_root root = {0};
  timao_heap_root_add(&vm->heap, &root, input->allocation);
  struct export_frame *frames = timao_memory_alloc(
      vm->heap.account, LEME_PUBLIC_MAX_DEPTH * sizeof(*frames), error);
  if (frames == NULL) {
    timao_heap_root_remove(&vm->heap, &root);
    return TIMAO_ERROR;
  }
  frames[0].input = input;
  const struct leme_public_work work = {.context = execution,
                                        .step = conversion_work};
  leme_public_builder_set_work(builder, &work);
  size_t depth = 1;
  enum timao_status status = TIMAO_OK;
  while (depth != 0) {
    struct export_frame *frame = &frames[depth - 1];
    if (!frame->entered) {
      status = export_begin(execution, builder, frame, error);
      if (status != TIMAO_OK)
        break;
      frame->entered = true;
    }
    if (frame->cursor < frame->count) {
      if (depth == LEME_PUBLIC_MAX_DEPTH) {
        status = public_error(LEME_PUBLIC_LIMIT, error);
        break;
      }
      const struct timao_value *child =
          frame->input->kind == TIMAO_ARRAY
              ? frame->input->as.array.items[frame->cursor]
              : frame->input->as.object.members[frame->cursor].value;
      frames[depth++] = (struct export_frame){.input = child};
      continue;
    }
    if (depth == 1) {
      *out = frame->value;
      --depth;
      continue;
    }
    struct export_frame *parent = &frames[depth - 2];
    const enum leme_public_status attached =
        parent->input->kind == TIMAO_ARRAY
            ? leme_public_array_set(builder, parent->value, parent->cursor,
                                    frame->value)
            : leme_public_object_set(
                  builder, parent->value,
                  parent->input->as.object.members[parent->cursor].key,
                  frame->value);
    if (attached != LEME_PUBLIC_OK) {
      status = public_error(attached, error);
      break;
    }
    ++parent->cursor;
    --depth;
  }
  leme_public_builder_set_work(builder, NULL);
  timao_memory_free(frames);
  timao_heap_root_remove(&vm->heap, &root);
  if (status != TIMAO_OK && vm->meter.failure != NULL)
    status = timao_error(error, vm->meter.failure, "JSON conversion stopped");
  return status;
}

enum timao_status timao_export_json(struct timao_vm *vm,
                                    const struct timao_value *input,
                                    struct leme_public_budget *account,
                                    struct leme_public_builder **owner,
                                    const struct leme_public_value **out,
                                    struct timao_diagnostic *error) {
  if (owner != NULL)
    *owner = NULL;
  if (out != NULL)
    *out = NULL;
  if (vm == NULL || vm->active || account == NULL || owner == NULL ||
      out == NULL)
    return timao_error(error, "invalid_argument",
                       "invalid or reentrant result export");
  timao_diagnostic_destroy(error);
  timao_meter_init(&vm->meter, vm->limits.steps, vm->host.context,
                   vm->host.cancelled);
  struct leme_public_builder *builder = NULL;
  enum leme_public_status made = leme_public_builder_create_budget(
      account, vm->limits.memory_bytes, &builder);
  if (made != LEME_PUBLIC_OK)
    return public_error(made, error);
  vm->active = true;
  struct timao_execution execution = {.vm = vm};
  struct leme_public_value *json = NULL;
  enum timao_status status =
      timao_export(&execution, input, builder, &json, error);
  if (status == TIMAO_OK) {
    const struct leme_public_work work = {.context = &execution,
                                          .step = conversion_work};
    const struct leme_public_value *roots[] = {json};
    leme_public_builder_set_work(builder, &work);
    made = leme_public_builder_seal(builder, roots, 1);
    leme_public_builder_set_work(builder, NULL);
    if (made != LEME_PUBLIC_OK)
      status = vm->meter.failure != NULL
                   ? timao_error(error, vm->meter.failure,
                                 "JSON result export stopped")
                   : public_error(made, error);
  }
  vm->active = false;
  if (status != TIMAO_OK) {
    leme_public_builder_destroy(builder);
    return status;
  }
  *owner = builder;
  *out = json;
  return TIMAO_OK;
}
