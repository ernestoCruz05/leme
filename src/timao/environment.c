#include "timao/environment.h"
#include "timao/language-internal.h"
#include "timao/diagnostic.h"
#include "timao/scalar.h"
#include <string.h>

static struct timao_heap_object *environment_edge(const void *data,
                                                  size_t index) {
  const struct timao_environment *environment = data;
  if (index == 0)
    return environment->parent == NULL ? NULL : environment->parent->allocation;
  const struct timao_value *value = environment->bindings[index - 1].value;
  return value == NULL ? NULL : value->allocation;
}
static enum timao_status equal(struct timao_vm *vm,
                               struct leme_public_text left,
                               struct leme_public_text right, bool *out,
                               struct timao_diagnostic *error) {
  *out = false;
  if (left.length != right.length)
    return TIMAO_OK;
  int order = 0;
  if (timao_text_order(vm, left, right, &order, error) != TIMAO_OK)
    return TIMAO_ERROR;
  *out = order == 0;
  return TIMAO_OK;
}
enum timao_status timao_environment_create(struct timao_vm *vm,
                                           struct timao_environment *parent,
                                           const struct timao_member *bindings,
                                           size_t count,
                                           struct timao_environment **out,
                                           struct timao_diagnostic *error) {
  *out = NULL;
  if (count > (SIZE_MAX - sizeof(struct timao_environment)) / sizeof(*bindings))
    return timao_error(error, "resource_limit", "environment size overflow");
  size_t size = sizeof(struct timao_environment) + count * sizeof(*bindings);
  for (size_t i = 0; i < count; ++i) {
    if (timao_charge(&vm->meter, 1, error) != TIMAO_OK)
      return TIMAO_ERROR;
    if (bindings[i].key.length > SIZE_MAX - size)
      return timao_error(error, "resource_limit", "binding name overflow");
    size += bindings[i].key.length;
  }
  struct timao_heap_object *allocation = NULL;
  if (timao_heap_alloc(&vm->heap, size, count + 1, environment_edge, NULL,
                       &allocation, error) != TIMAO_OK)
    return TIMAO_ERROR;
  struct timao_environment *environment = timao_heap_data(allocation);
  environment->allocation = allocation;
  environment->parent = parent;
  environment->count = count;
  environment->bindings = (struct timao_member *)(environment + 1);
  char *keys = (char *)(environment->bindings + count);
  for (size_t i = 0; i < count; ++i) {
    environment->bindings[i] = (struct timao_member){
        .key = {keys, bindings[i].key.length}, .value = bindings[i].value};
    for (size_t p = 0; p < bindings[i].key.length;) {
      if (timao_charge(&vm->meter, 1, error) != TIMAO_OK)
        return TIMAO_ERROR;
      const size_t chunk =
          bindings[i].key.length - p > 128 ? 128 : bindings[i].key.length - p;
      memcpy(keys + p, bindings[i].key.data + p, chunk);
      p += chunk;
    }
    keys += bindings[i].key.length;
  }
  *out = environment;
  return TIMAO_OK;
}
enum timao_status timao_environment_get(
    struct timao_vm *vm, const struct timao_environment *environment,
    struct leme_public_text name, const struct timao_value **out,
    struct timao_diagnostic *error) {
  *out = NULL;
  for (const struct timao_environment *scope = environment; scope != NULL;
       scope = scope->parent) {
    for (size_t i = scope->count; i != 0; --i) {
      if (timao_charge(&vm->meter, 1, error) != TIMAO_OK)
        return TIMAO_ERROR;
      bool matches = false;
      if (equal(vm, scope->bindings[i - 1].key, name, &matches, error) !=
          TIMAO_OK)
        return TIMAO_ERROR;
      if (matches) {
        *out = scope->bindings[i - 1].value;
        return TIMAO_OK;
      }
    }
  }
  return timao_error(error, "unbound_name",
                     "name is not bound in this lexical scope");
}
enum timao_status timao_environment_replace(
    struct timao_vm *vm, const struct timao_environment *environment,
    struct leme_public_text name, const struct timao_value *value,
    struct timao_environment **out, struct timao_diagnostic *error) {
  *out = NULL;
  const size_t count = environment == NULL ? 0 : environment->count;
  if (count >= SIZE_MAX / sizeof(struct timao_member))
    return timao_error(error, "resource_limit", "definition table overflow");
  struct timao_member *bindings = timao_memory_alloc(
      vm->heap.account, (count + 1) * sizeof(*bindings), error);
  if (bindings == NULL)
    return TIMAO_ERROR;
  size_t used = 0;
  for (size_t i = 0; i < count; ++i) {
    bool matches = false;
    if (timao_charge(&vm->meter, 1, error) != TIMAO_OK ||
        equal(vm, environment->bindings[i].key, name, &matches, error) !=
            TIMAO_OK) {
      timao_memory_free(bindings);
      return TIMAO_ERROR;
    }
    if (!matches)
      bindings[used++] = environment->bindings[i];
  }
  if (value != NULL)
    bindings[used++] = (struct timao_member){.key = name, .value = value};
  const enum timao_status status =
      timao_environment_create(vm, NULL, bindings, used, out, error);
  timao_memory_free(bindings);
  return status;
}
