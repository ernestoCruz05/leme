#include "timao/scalar.h"
#include "timao/language-internal.h"
#include "timao/diagnostic.h"
#include <math.h>
#include <string.h>

enum timao_status timao_text_order(struct timao_vm *vm,
                                   struct leme_public_text left,
                                   struct leme_public_text right, int *out,
                                   struct timao_diagnostic *error) {
  const size_t length = left.length < right.length ? left.length : right.length;
  for (size_t i = 0; i < length;) {
    if (timao_charge(&vm->meter, 1, error) != TIMAO_OK)
      return TIMAO_ERROR;
    const size_t count = length - i > 128 ? 128 : length - i;
    const int cmp = memcmp(left.data + i, right.data + i, count);
    if (cmp != 0) {
      *out = cmp < 0 ? -1 : 1;
      return TIMAO_OK;
    }
    i += count;
  }
  *out = left.length < right.length ? -1 : left.length > right.length ? 1 : 0;
  return TIMAO_OK;
}
enum timao_status timao_get(struct timao_vm *vm,
                            const struct timao_value *object,
                            struct leme_public_text key,
                            const struct timao_value **out,
                            struct timao_diagnostic *error) {
  *out = NULL;
  if (object->kind != TIMAO_OBJECT)
    return timao_error(error, "type_error", "get requires an object");
  for (size_t i = 0; i < object->as.object.count; ++i) {
    if (timao_charge(&vm->meter, 1, error) != TIMAO_OK)
      return TIMAO_ERROR;
    int order = 0;
    if (timao_text_order(vm, object->as.object.members[i].key, key, &order,
                         error) != TIMAO_OK)
      return TIMAO_ERROR;
    if (order == 0) {
      *out = object->as.object.members[i].value;
      return TIMAO_OK;
    }
  }
  return timao_error(error, "unknown_field", "unknown local object key");
}
static enum timao_status json_data(struct timao_vm *vm,
                                   const struct timao_value *value,
                                   size_t depth,
                                   struct timao_diagnostic *error) {
  if (depth > LEME_PUBLIC_MAX_DEPTH)
    return timao_error(error, "resource_limit", "comparison depth exceeded");
  if (timao_charge(&vm->meter, 1, error) != TIMAO_OK)
    return TIMAO_ERROR;
  if (value->kind == TIMAO_CALLABLE || value->kind == TIMAO_WATCH)
    return timao_error(error, "type_error",
                       "local handles cannot be compared as JSON");
  const size_t count = value->kind == TIMAO_ARRAY    ? value->as.array.count
                       : value->kind == TIMAO_OBJECT ? value->as.object.count
                                                     : 0;
  for (size_t i = 0; i < count; ++i)
    if (json_data(vm,
                  value->kind == TIMAO_ARRAY
                      ? value->as.array.items[i]
                      : value->as.object.members[i].value,
                  depth + 1, error) != TIMAO_OK)
      return TIMAO_ERROR;
  return TIMAO_OK;
}
static enum timao_status compare_equal(struct timao_vm *vm,
                                       const struct timao_value *left,
                                       const struct timao_value *right,
                                       bool *out,
                                       struct timao_diagnostic *error) {
  *out = false;
  if (timao_charge(&vm->meter, 1, error) != TIMAO_OK)
    return TIMAO_ERROR;
  if (left->kind != right->kind)
    return TIMAO_OK;
  switch (left->kind) {
  case TIMAO_NULL:
    *out = true;
    break;
  case TIMAO_BOOLEAN:
    *out = left->as.boolean == right->as.boolean;
    break;
  case TIMAO_NUMBER:
    *out = left->as.number == right->as.number;
    break;
  case TIMAO_STRING: {
    int order = 0;
    if (timao_text_order(vm, left->as.text, right->as.text, &order, error) !=
        TIMAO_OK)
      return TIMAO_ERROR;
    *out = order == 0;
    break;
  }
  case TIMAO_ARRAY:
    if (left->as.array.count != right->as.array.count)
      return TIMAO_OK;
    for (size_t i = 0; i < left->as.array.count; ++i) {
      bool equal = false;
      if (compare_equal(vm, left->as.array.items[i], right->as.array.items[i],
                        &equal, error) != TIMAO_OK)
        return TIMAO_ERROR;
      if (!equal)
        return TIMAO_OK;
    }
    *out = true;
    break;
  case TIMAO_OBJECT:
    if (left->as.object.count != right->as.object.count)
      return TIMAO_OK;
    for (size_t i = 0; i < left->as.object.count; ++i) {
      const struct timao_member *member = &left->as.object.members[i];
      const struct timao_value *other = NULL;
      for (size_t j = 0; j < right->as.object.count; ++j) {
        if (timao_charge(&vm->meter, 1, error) != TIMAO_OK)
          return TIMAO_ERROR;
        int order = 0;
        if (timao_text_order(vm, member->key, right->as.object.members[j].key,
                             &order, error) != TIMAO_OK)
          return TIMAO_ERROR;
        if (order == 0) {
          other = right->as.object.members[j].value;
          break;
        }
      }
      if (other == NULL)
        return TIMAO_OK;
      bool equal = false;
      if (compare_equal(vm, member->value, other, &equal, error) != TIMAO_OK)
        return TIMAO_ERROR;
      if (!equal)
        return TIMAO_OK;
    }
    *out = true;
    break;
  case TIMAO_CALLABLE:
  case TIMAO_WATCH:
    return timao_error(error, "type_error", "local handles cannot be compared");
  }
  return TIMAO_OK;
}
enum timao_status timao_equal(struct timao_vm *vm,
                              const struct timao_value *left,
                              const struct timao_value *right, bool *out,
                              struct timao_diagnostic *error) {
  if (json_data(vm, left, 1, error) != TIMAO_OK ||
      json_data(vm, right, 1, error) != TIMAO_OK)
    return TIMAO_ERROR;
  return compare_equal(vm, left, right, out, error);
}
enum timao_status timao_scalar(struct timao_execution *execution,
                               const struct leme_control_operator *op,
                               const struct timao_value *const *args,
                               size_t count, const struct timao_value **out,
                               struct timao_diagnostic *error) {
  struct timao_vm *vm = execution->vm;
  *out = NULL;
  if (op == NULL || count < op->min_args || count > op->max_args)
    return timao_error(error, "arity_error", "invalid pure operator arguments");
  switch (op->opcode) {
  case LEME_CONTROL_OP_LIST:
    return timao_value_array(vm, args, count, out, error);
  case LEME_CONTROL_OP_OBJECT: {
    if (count % 2 != 0)
      return timao_error(error, "arity_error", "object needs key/value pairs");
    if (count == 0)
      return timao_value_object(vm, NULL, 0, out, error);
    struct timao_member *members = timao_memory_alloc(
        vm->heap.account, (count / 2) * sizeof(*members), error);
    if (members == NULL)
      return TIMAO_ERROR;
    for (size_t i = 0; i < count / 2; ++i) {
      if (args[i * 2]->kind != TIMAO_STRING) {
        timao_memory_free(members);
        return timao_error(error, "type_error", "object keys must be strings");
      }
      members[i] = (struct timao_member){.key = args[i * 2]->as.text,
                                         .value = args[i * 2 + 1]};
    }
    const enum timao_status status =
        timao_value_object(vm, members, count / 2, out, error);
    timao_memory_free(members);
    return status;
  }
  case LEME_CONTROL_OP_GET:
    if (args[1]->kind != TIMAO_STRING)
      return timao_error(error, "type_error", "get key must be a string");
    return timao_get(vm, args[0], args[1]->as.text, out, error);
  case LEME_CONTROL_OP_NOT:
    if (args[0]->kind != TIMAO_BOOLEAN)
      return timao_error(error, "type_error", "not requires a boolean");
    return timao_value_boolean(vm, !args[0]->as.boolean, out, error);
  case LEME_CONTROL_OP_EQ:
  case LEME_CONTROL_OP_NE: {
    bool equal = false;
    if (timao_equal(vm, args[0], args[1], &equal, error) != TIMAO_OK)
      return TIMAO_ERROR;
    return timao_value_boolean(
        vm, op->opcode == LEME_CONTROL_OP_EQ ? equal : !equal, out, error);
  }
  case LEME_CONTROL_OP_CONTAINS:
    if (args[0]->kind != TIMAO_ARRAY)
      return timao_error(error, "type_error", "contains requires an array");
    for (size_t i = 0; i < args[0]->as.array.count; ++i) {
      bool equal = false;
      if (timao_equal(vm, args[0]->as.array.items[i], args[1], &equal, error) !=
          TIMAO_OK)
        return TIMAO_ERROR;
      if (equal)
        return timao_value_boolean(vm, true, out, error);
    }
    return timao_value_boolean(vm, false, out, error);
  case LEME_CONTROL_OP_LT:
  case LEME_CONTROL_OP_LE:
  case LEME_CONTROL_OP_GT:
  case LEME_CONTROL_OP_GE: {
    int order = 0;
    if (args[0]->kind == TIMAO_NUMBER && args[1]->kind == TIMAO_NUMBER)
      order = args[0]->as.number < args[1]->as.number   ? -1
              : args[0]->as.number > args[1]->as.number ? 1
                                                        : 0;
    else if (args[0]->kind == TIMAO_STRING && args[1]->kind == TIMAO_STRING) {
      if (timao_text_order(vm, args[0]->as.text, args[1]->as.text, &order,
                           error) != TIMAO_OK)
        return TIMAO_ERROR;
    } else
      return timao_error(error, "type_error",
                         "ordering requires two numbers or two strings");
    const bool result = op->opcode == LEME_CONTROL_OP_LT   ? order < 0
                        : op->opcode == LEME_CONTROL_OP_LE ? order <= 0
                        : op->opcode == LEME_CONTROL_OP_GT ? order > 0
                                                           : order >= 0;
    return timao_value_boolean(vm, result, out, error);
  }
  case LEME_CONTROL_OP_ADD:
  case LEME_CONTROL_OP_SUB:
  case LEME_CONTROL_OP_MUL:
  case LEME_CONTROL_OP_DIV: {
    if (args[0]->kind != TIMAO_NUMBER ||
        (count == 2 && args[1]->kind != TIMAO_NUMBER))
      return timao_error(error, "type_error", "arithmetic requires numbers");
    const double a = args[0]->as.number,
                 b = count == 2 ? args[1]->as.number : 0.0;
    if (op->opcode == LEME_CONTROL_OP_DIV && b == 0.0)
      return timao_error(error, "invalid_argument", "division by zero");
    const double result = op->opcode == LEME_CONTROL_OP_ADD ? a + b
                          : op->opcode == LEME_CONTROL_OP_SUB
                              ? (count == 1 ? -a : a - b)
                          : op->opcode == LEME_CONTROL_OP_MUL ? a * b
                                                              : a / b;
    return timao_value_number(vm, result, out, error);
  }
  case LEME_CONTROL_OP_COUNT:
  case LEME_CONTROL_OP_FIRST:
  case LEME_CONTROL_OP_LIMIT: {
    if (args[0]->kind != TIMAO_ARRAY)
      return timao_error(error, "type_error",
                         "collection operator requires an array");
    const size_t length = args[0]->as.array.count;
    if (op->opcode == LEME_CONTROL_OP_COUNT)
      return timao_value_number(vm, (double)length, out, error);
    if (op->opcode == LEME_CONTROL_OP_FIRST) {
      if (length == 0)
        return timao_value_null(vm, out, error);
      *out = args[0]->as.array.items[0];
      return TIMAO_OK;
    }
    if (args[1]->kind != TIMAO_NUMBER || args[1]->as.number < 0 ||
        trunc(args[1]->as.number) != args[1]->as.number)
      return timao_error(error, "invalid_argument",
                         "limit requires a nonnegative integer");
    const size_t limit = args[1]->as.number >= (double)length
                             ? length
                             : (size_t)args[1]->as.number;
    return timao_value_array(vm, args[0]->as.array.items, limit, out, error);
  }
  default:
    return timao_error(
        error, "unsupported",
        "operator requires an explicit remote or collection context");
  }
}
