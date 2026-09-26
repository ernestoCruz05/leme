#include "timao/language-internal.h"
#include "timao/diagnostic.h"
#include "timao/scalar.h"

#include <string.h>
#include <math.h>

static struct timao_heap_object *value_edge(const void *data, size_t index) {
  const struct timao_value *value = data;
  const struct timao_value *child = value->kind == TIMAO_OBJECT
                                        ? value->as.object.members[index].value
                                        : value->as.array.items[index];
  return child == NULL ? NULL : child->allocation;
}

static struct timao_value *allocate_value(struct timao_vm *vm, size_t extra,
                                          size_t edges,
                                          struct timao_diagnostic *error) {
  if (extra > SIZE_MAX - sizeof(struct timao_value)) {
    timao_error(error, "resource_limit", "value size overflow");
    return NULL;
  }
  struct timao_heap_object *allocation = NULL;
  if (timao_heap_alloc(&vm->heap, sizeof(struct timao_value) + extra, edges,
                       value_edge, NULL, &allocation, error) != TIMAO_OK)
    return NULL;
  struct timao_value *value = timao_heap_data(allocation);
  value->allocation = allocation;
  return value;
}

enum timao_status timao_value_string(struct timao_vm *vm,
                                     struct leme_public_text input,
                                     const struct timao_value **out,
                                     struct timao_diagnostic *error) {
  if (out == NULL)
    return timao_error(error, "invalid_argument", "missing value output");
  *out = NULL;
  if (vm == NULL || (input.length != 0 && input.data == NULL))
    return timao_error(error, "invalid_argument", "invalid string input");
  if (timao_charge(&vm->meter, 1, error) != TIMAO_OK)
    return TIMAO_ERROR;
  struct timao_value *value = allocate_value(vm, input.length, 0, error);
  if (value == NULL)
    return TIMAO_ERROR;
  char *text = (char *)(value + 1);
  for (size_t offset = 0; offset < input.length;) {
    if (timao_charge(&vm->meter, 1, error) != TIMAO_OK)
      return TIMAO_ERROR;
    const size_t remaining = input.length - offset;
    const size_t count = remaining > 128 ? 128 : remaining;
    memcpy(text + offset, input.data + offset, count);
    offset += count;
  }
  value->kind = TIMAO_STRING;
  value->as.text = (struct leme_public_text){text, input.length};
  *out = value;
  return TIMAO_OK;
}

enum timao_status timao_array_builder(struct timao_vm *vm, size_t count,
                                      struct timao_value **out,
                                      struct timao_diagnostic *error) {
  *out = NULL;
  if (count > SIZE_MAX / sizeof(const struct timao_value *))
    return timao_error(error, "resource_limit", "array capacity overflow");
  if (timao_charge(&vm->meter, 1 + count / 128, error) != TIMAO_OK)
    return TIMAO_ERROR;
  struct timao_value *value = allocate_value(
      vm, count * sizeof(const struct timao_value *), count, error);
  if (value == NULL)
    return TIMAO_ERROR;
  value->kind = TIMAO_ARRAY;
  value->as.array.count = count;
  value->as.array.items = (const struct timao_value **)(value + 1);
  *out = value;
  return TIMAO_OK;
}

enum timao_status timao_value_array(struct timao_vm *vm,
                                    const struct timao_value *const *items,
                                    size_t count,
                                    const struct timao_value **out,
                                    struct timao_diagnostic *error) {
  if (out == NULL)
    return timao_error(error, "invalid_argument", "missing value output");
  *out = NULL;
  if (vm == NULL || (count != 0 && items == NULL))
    return timao_error(error, "invalid_argument", "invalid array input");
  if (count > SIZE_MAX / sizeof(*items))
    return timao_error(error, "resource_limit", "array size overflow");
  if (timao_charge(&vm->meter, 1, error) != TIMAO_OK)
    return TIMAO_ERROR;
  for (size_t i = 0; i < count; ++i) {
    if (timao_charge(&vm->meter, 1, error) != TIMAO_OK)
      return TIMAO_ERROR;
    if (items[i] == NULL || !timao_heap_owns(&vm->heap, items[i]->allocation))
      return timao_error(error, "invalid_argument", "foreign array element");
  }
  struct timao_value *value =
      allocate_value(vm, count * sizeof(*items), count, error);
  if (value == NULL)
    return TIMAO_ERROR;
  value->kind = TIMAO_ARRAY;
  value->as.array.count = count;
  value->as.array.items = (const struct timao_value **)(value + 1);
  for (size_t i = 0; i < count; ++i) {
    if (timao_charge(&vm->meter, 1, error) != TIMAO_OK)
      return TIMAO_ERROR;
    value->as.array.items[i] = items[i];
  }
  *out = value;
  return TIMAO_OK;
}

enum timao_value_kind timao_value_kind(const struct timao_value *value) {
  return value == NULL ? TIMAO_NULL : value->kind;
}
size_t timao_value_length(const struct timao_value *value) {
  if (value == NULL)
    return 0;
  return value->kind == TIMAO_ARRAY    ? value->as.array.count
         : value->kind == TIMAO_OBJECT ? value->as.object.count
         : value->kind == TIMAO_STRING ? value->as.text.length
                                       : 0;
}
enum timao_status timao_value_as_boolean(const struct timao_value *value,
                                         bool *out) {
  if (out == NULL)
    return TIMAO_ERROR;
  *out = false;
  if (value == NULL || value->kind != TIMAO_BOOLEAN)
    return TIMAO_ERROR;
  *out = value->as.boolean;
  return TIMAO_OK;
}
enum timao_status timao_value_as_number(const struct timao_value *value,
                                        double *out) {
  if (out == NULL)
    return TIMAO_ERROR;
  *out = 0;
  if (value == NULL || value->kind != TIMAO_NUMBER)
    return TIMAO_ERROR;
  *out = value->as.number;
  return TIMAO_OK;
}
enum timao_status timao_value_watch_token(const struct timao_value *value,
                                          uint64_t *out) {
  if (out == NULL)
    return TIMAO_ERROR;
  *out = 0;
  if (value == NULL || value->kind != TIMAO_WATCH)
    return TIMAO_ERROR;
  *out = value->as.token;
  return TIMAO_OK;
}
enum timao_status timao_value_callable_arity(const struct timao_value *value,
                                             size_t *out) {
  if (out == NULL)
    return TIMAO_ERROR;
  *out = 0;
  if (value == NULL || value->kind != TIMAO_CALLABLE)
    return TIMAO_ERROR;
  *out = value->as.function.arity;
  return TIMAO_OK;
}
enum timao_status timao_value_member_at(const struct timao_value *value,
                                        size_t index,
                                        struct timao_member *out) {
  if (out == NULL)
    return TIMAO_ERROR;
  *out = (struct timao_member){0};
  if (value == NULL || value->kind != TIMAO_OBJECT ||
      index >= value->as.object.count)
    return TIMAO_ERROR;
  *out = value->as.object.members[index];
  return TIMAO_OK;
}

static struct timao_value *primitive(struct timao_vm *vm,
                                     enum timao_value_kind kind,
                                     struct timao_diagnostic *error) {
  if (vm == NULL) {
    timao_error(error, "invalid_argument", "missing VM");
    return NULL;
  }
  if (timao_charge(&vm->meter, 1, error) != TIMAO_OK)
    return NULL;
  struct timao_value *value = allocate_value(vm, 0, 0, error);
  if (value != NULL)
    value->kind = kind;
  return value;
}
enum timao_status timao_value_null(struct timao_vm *vm,
                                   const struct timao_value **out,
                                   struct timao_diagnostic *error) {
  if (out == NULL)
    return timao_error(error, "invalid_argument", "missing value output");
  *out = primitive(vm, TIMAO_NULL, error);
  return *out == NULL ? TIMAO_ERROR : TIMAO_OK;
}
enum timao_status timao_value_boolean(struct timao_vm *vm, bool input,
                                      const struct timao_value **out,
                                      struct timao_diagnostic *error) {
  if (out == NULL)
    return timao_error(error, "invalid_argument", "missing value output");
  *out = NULL;
  struct timao_value *value = primitive(vm, TIMAO_BOOLEAN, error);
  if (value == NULL)
    return TIMAO_ERROR;
  value->as.boolean = input;
  *out = value;
  return TIMAO_OK;
}
enum timao_status timao_value_number(struct timao_vm *vm, double input,
                                     const struct timao_value **out,
                                     struct timao_diagnostic *error) {
  if (out == NULL)
    return timao_error(error, "invalid_argument", "missing value output");
  *out = NULL;
  if (!isfinite(input) || fabs(input) > 9007199254740991.0)
    return timao_error(error, "invalid_argument",
                       "number exceeds finite public range");
  struct timao_value *value = primitive(vm, TIMAO_NUMBER, error);
  if (value == NULL)
    return TIMAO_ERROR;
  value->as.number = input == 0.0 ? 0.0 : input;
  *out = value;
  return TIMAO_OK;
}
enum timao_status timao_watch_value(struct timao_execution *execution,
                                    uint64_t token,
                                    const struct timao_value **out,
                                    struct timao_diagnostic *error) {
  if (out == NULL)
    return timao_error(error, "invalid_argument", "missing watch output");
  *out = NULL;
  if (execution == NULL || token == 0)
    return timao_error(error, "invalid_argument", "invalid local watch token");
  struct timao_value *value = primitive(execution->vm, TIMAO_WATCH, error);
  if (value == NULL)
    return TIMAO_ERROR;
  value->as.token = token;
  *out = value;
  return TIMAO_OK;
}
static enum timao_status object_value(struct timao_vm *vm,
                                      const struct timao_member *members,
                                      size_t count, bool partial,
                                      struct timao_value **out,
                                      struct timao_diagnostic *error) {
  if (out == NULL)
    return timao_error(error, "invalid_argument", "missing object output");
  *out = NULL;
  if (vm == NULL || (count != 0 && members == NULL))
    return timao_error(error, "invalid_argument", "missing object members");
  if (count > SIZE_MAX / sizeof(*members))
    return timao_error(error, "resource_limit", "object size overflow");
  size_t size = count * sizeof(*members);
  for (size_t i = 0; i < count; ++i) {
    if (timao_charge(&vm->meter, 1, error) != TIMAO_OK)
      return TIMAO_ERROR;
    if ((!partial && members[i].value == NULL) ||
        (members[i].value != NULL &&
         !timao_heap_owns(&vm->heap, members[i].value->allocation)) ||
        (members[i].key.length != 0 && members[i].key.data == NULL))
      return timao_error(error, "invalid_argument", "invalid object member");
    if (members[i].key.length > SIZE_MAX - size)
      return timao_error(error, "resource_limit", "object key overflow");
    size += members[i].key.length;
    for (size_t j = 0; j < i; ++j) {
      if (timao_charge(&vm->meter, 1, error) != TIMAO_OK)
        return TIMAO_ERROR;
      if (members[i].key.length != members[j].key.length)
        continue;
      int order = 0;
      if (timao_text_order(vm, members[i].key, members[j].key, &order, error) !=
          TIMAO_OK)
        return TIMAO_ERROR;
      if (order == 0)
        return timao_error(error, "invalid_argument", "duplicate object key");
    }
  }
  struct timao_value *value = allocate_value(vm, size, count, error);
  if (value == NULL)
    return TIMAO_ERROR;
  value->kind = TIMAO_OBJECT;
  value->as.object.count = count;
  value->as.object.members = (struct timao_member *)(value + 1);
  char *keys = (char *)(value->as.object.members + count);
  for (size_t i = 0; i < count; ++i) {
    value->as.object.members[i] = (struct timao_member){
        .key = {keys, members[i].key.length}, .value = members[i].value};
    for (size_t p = 0; p < members[i].key.length;) {
      if (timao_charge(&vm->meter, 1, error) != TIMAO_OK)
        return TIMAO_ERROR;
      const size_t chunk =
          members[i].key.length - p > 128 ? 128 : members[i].key.length - p;
      memcpy(keys + p, members[i].key.data + p, chunk);
      p += chunk;
    }
    keys += members[i].key.length;
  }
  *out = value;
  return TIMAO_OK;
}

enum timao_status timao_object_builder(struct timao_vm *vm,
                                       const struct timao_member *members,
                                       size_t count, struct timao_value **out,
                                       struct timao_diagnostic *error) {
  return object_value(vm, members, count, true, out, error);
}

enum timao_status timao_value_object(struct timao_vm *vm,
                                     const struct timao_member *members,
                                     size_t count,
                                     const struct timao_value **out,
                                     struct timao_diagnostic *error) {
  if (out == NULL)
    return timao_error(error, "invalid_argument", "missing object output");
  *out = NULL;
  struct timao_value *value = NULL;
  const enum timao_status status =
      object_value(vm, members, count, false, &value, error);
  if (status == TIMAO_OK)
    *out = value;
  return status;
}

enum timao_status timao_value_text(const struct timao_value *value,
                                   struct leme_public_text *out) {
  if (out == NULL)
    return TIMAO_ERROR;
  *out = (struct leme_public_text){0};
  if (value == NULL || value->kind != TIMAO_STRING)
    return TIMAO_ERROR;
  *out = value->as.text;
  return TIMAO_OK;
}

enum timao_status timao_value_at(const struct timao_value *value, size_t index,
                                 const struct timao_value **out) {
  if (out == NULL)
    return TIMAO_ERROR;
  *out = NULL;
  if (value == NULL || value->kind != TIMAO_ARRAY ||
      index >= value->as.array.count)
    return TIMAO_ERROR;
  *out = value->as.array.items[index];
  return TIMAO_OK;
}
