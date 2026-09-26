#ifndef TIMAO_ENVIRONMENT_H
#define TIMAO_ENVIRONMENT_H

#include "timao/value.h"
#include "timao/heap.h"

struct timao_environment {
  struct timao_heap_object *allocation;
  struct timao_environment *parent;
  struct timao_member *bindings;
  size_t count;
};

enum timao_status timao_environment_create(struct timao_vm *vm,
                                           struct timao_environment *parent,
                                           const struct timao_member *bindings,
                                           size_t count,
                                           struct timao_environment **out,
                                           struct timao_diagnostic *error);
enum timao_status timao_environment_get(
    struct timao_vm *vm, const struct timao_environment *environment,
    struct leme_public_text name, const struct timao_value **out,
    struct timao_diagnostic *error);
enum timao_status timao_environment_replace(
    struct timao_vm *vm, const struct timao_environment *environment,
    struct leme_public_text name, const struct timao_value *value,
    struct timao_environment **out, struct timao_diagnostic *error);

#endif
