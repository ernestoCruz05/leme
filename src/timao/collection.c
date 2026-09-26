#include "timao/collection.h"
#include "timao/language-internal.h"
#include "timao/diagnostic.h"
#include "timao/scalar.h"
#include <string.h>

enum timao_status timao_collection_fields(struct timao_execution *execution,
                                          struct timao_program *program,
                                          uint32_t call,
                                          struct timao_diagnostic *error) {
  const bool select = timao_node_is(
      timao_node_at(program, program->nodes[call].first), "select");
  const uint32_t first = timao_child(program, call, 2);
  for (uint32_t current = first; current != 0;
       current = select ? program->nodes[current].next : 0) {
    if (timao_charge(&execution->vm->meter, 1, error) != TIMAO_OK)
      return TIMAO_ERROR;
    if (program->nodes[current].kind != TIMAO_TOKEN_FIELD)
      return timao_error(error, "type_error",
                         "collection selector must be a field path");
    for (uint32_t previous = first; previous != current;
         previous = program->nodes[previous].next) {
      if (timao_charge(&execution->vm->meter, 1, error) != TIMAO_OK)
        return TIMAO_ERROR;
      const struct leme_public_text a = program->nodes[previous].text,
                                    b = program->nodes[current].text;
      int order = 0;
      if (timao_text_order(execution->vm, a, b, &order, error) != TIMAO_OK)
        return TIMAO_ERROR;
      if (order == 0)
        return timao_error(error, "invalid_argument",
                           "duplicate selected field");
    }
  }
  return TIMAO_OK;
}
enum timao_status timao_project(struct timao_execution *execution,
                                struct timao_program *program, uint32_t call,
                                const struct timao_value *collection,
                                const struct timao_value **out,
                                struct timao_diagnostic *error) {
  const size_t count = program->nodes[call].count - 2;
  struct timao_member *members = timao_memory_alloc(
      execution->vm->heap.account, count * sizeof(*members), error);
  if (members == NULL)
    return TIMAO_ERROR;
  struct timao_value *array = NULL;
  enum timao_status status = timao_array_builder(
      execution->vm, collection->as.array.count, &array, error);
  if (status != TIMAO_OK) {
    timao_memory_free(members);
    return status;
  }
  for (size_t i = 0; i < collection->as.array.count; ++i) {
    uint32_t field = timao_child(program, call, 2);
    for (size_t j = 0; j < count; ++j) {
      const struct leme_public_text path = program->nodes[field].text;
      members[j].key =
          (struct leme_public_text){path.data + 1, path.length - 1};
      status = timao_local_field(execution, collection->as.array.items[i], path,
                                 &members[j].value, error);
      if (status != TIMAO_OK)
        goto done;
      field = program->nodes[field].next;
    }
    status = timao_value_object(execution->vm, members, count,
                                &array->as.array.items[i], error);
    if (status != TIMAO_OK)
      goto done;
  }
  *out = array;
done:
  timao_memory_free(members);
  return status;
}
struct sort_item {
  const struct timao_value *value;
  const struct timao_value *key;
};
static enum timao_status compare(struct timao_execution *execution,
                                 const struct timao_value *left,
                                 const struct timao_value *right,
                                 bool descending, int *out,
                                 struct timao_diagnostic *error) {
  if (timao_charge(&execution->vm->meter, 1, error) != TIMAO_OK)
    return TIMAO_ERROR;
  if (left->kind == TIMAO_NULL || right->kind == TIMAO_NULL) {
    *out = left->kind == right->kind ? 0 : left->kind == TIMAO_NULL ? 1 : -1;
    return TIMAO_OK;
  }
  if (left->kind != right->kind)
    return timao_error(error, "type_error",
                       "sort keys have incompatible types");
  int order = 0;
  if (left->kind == TIMAO_NUMBER)
    order = left->as.number < right->as.number   ? -1
            : left->as.number > right->as.number ? 1
                                                 : 0;
  else if (timao_text_order(execution->vm, left->as.text, right->as.text,
                            &order, error) != TIMAO_OK)
    return TIMAO_ERROR;
  *out = descending ? -order : order;
  return TIMAO_OK;
}
enum timao_status timao_sort(struct timao_execution *execution,
                             const struct timao_value *collection,
                             struct leme_public_text path,
                             const struct timao_value *direction,
                             const struct timao_value **out,
                             struct timao_diagnostic *error) {
  if (direction->kind != TIMAO_STRING)
    return timao_error(error, "type_error", "sort order must be a string");
  const bool descending = direction->as.text.length == 4 &&
                          memcmp(direction->as.text.data, "desc", 4) == 0;
  if (!descending && !(direction->as.text.length == 3 &&
                       memcmp(direction->as.text.data, "asc", 3) == 0))
    return timao_error(error, "invalid_argument",
                       "sort order must be asc or desc");
  const size_t count = collection->as.array.count;
  if (count == 0)
    return timao_value_array(execution->vm, NULL, 0, out, error);
  if (count > SIZE_MAX / sizeof(struct sort_item))
    return timao_error(error, "resource_limit", "sort storage overflow");
  struct sort_item *items = timao_memory_alloc(execution->vm->heap.account,
                                               count * sizeof(*items), error);
  if (items == NULL)
    return TIMAO_ERROR;
  struct sort_item *scratch = timao_memory_alloc(
      execution->vm->heap.account, count * sizeof(*scratch), error);
  enum timao_status status = TIMAO_ERROR;
  if (scratch == NULL)
    goto done;
  enum timao_value_kind common = TIMAO_NULL;
  for (size_t i = 0; i < count; ++i) {
    items[i].value = collection->as.array.items[i];
    if (timao_local_field(execution, items[i].value, path, &items[i].key,
                          error) != TIMAO_OK)
      goto done;
    const enum timao_value_kind kind = items[i].key->kind;
    if (kind != TIMAO_NULL && kind != TIMAO_STRING && kind != TIMAO_NUMBER) {
      timao_error(error, "type_error",
                  "sort keys must be numbers, strings or null");
      goto done;
    }
    if (kind != TIMAO_NULL) {
      if (common != TIMAO_NULL && common != kind) {
        timao_error(error, "type_error", "mixed sort key types");
        goto done;
      }
      common = kind;
    }
  }
  for (size_t width = 1; width < count;) {
    for (size_t start = 0; start < count;) {
      const size_t middle = count - start < width ? count : start + width;
      const size_t end = count - middle < width ? count : middle + width;
      size_t left = start, right = middle;
      for (size_t pos = start; pos < end; ++pos) {
        if (timao_charge(&execution->vm->meter, 1, error) != TIMAO_OK)
          goto done;
        int order = 0;
        if (left < middle && right < end &&
            compare(execution, items[left].key, items[right].key, descending,
                    &order, error) != TIMAO_OK)
          goto done;
        scratch[pos] = right == end || (left < middle && order <= 0)
                           ? items[left++]
                           : items[right++];
      }
      start = end;
    }
    struct sort_item *swap = items;
    items = scratch;
    scratch = swap;
    if (width > count / 2)
      break;
    width *= 2;
  }
  struct timao_value *result = NULL;
  if (timao_array_builder(execution->vm, count, &result, error) != TIMAO_OK)
    goto done;
  for (size_t i = 0; i < count; ++i) {
    if (timao_charge(&execution->vm->meter, 1, error) != TIMAO_OK)
      goto done;
    result->as.array.items[i] = items[i].value;
  }
  *out = result;
  status = TIMAO_OK;
done:
  timao_memory_free(items);
  timao_memory_free(scratch);
  return status;
}
