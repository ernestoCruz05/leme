#include "timao/bindings.h"
#include "timao/language-internal.h"
#include "timao/lower.h"
#include "timao/diagnostic.h"
#include <math.h>
#include <string.h>

static enum timao_status invoke(struct timao_execution *execution,
                                enum timao_host_op operation,
                                const struct timao_lowered *descriptor,
                                const struct timao_value *const *arguments,
                                size_t count, const struct timao_value **out,
                                struct timao_diagnostic *error) {
  if (execution->context == TIMAO_PURE)
    return timao_error(error, "invalid_argument",
                       "host operations are forbidden in pure calls");
  if (execution->vm->host.invoke == NULL)
    return timao_error(error, "unsupported", "host binding is unavailable");
  if (timao_charge(&execution->vm->meter, 1, error) != TIMAO_OK)
    return TIMAO_ERROR;
  const enum timao_context previous = execution->context;
  if (operation == TIMAO_HOST_AWAIT)
    execution->context = TIMAO_PURE;
  const enum timao_status status = execution->vm->host.invoke(
      execution->vm->host.context, execution, operation, descriptor, arguments,
      count, out, error);
  execution->context = previous;
  if (status != TIMAO_OK) {
    *out = NULL;
    if (status != TIMAO_ERROR)
      return timao_error(error, "invalid_argument",
                         "invalid host completion status");
    return status;
  }
  if (*out == NULL ||
      !timao_heap_owns(&execution->vm->heap, (*out)->allocation)) {
    *out = NULL;
    return timao_error(error, "invalid_argument",
                       "host returned a foreign or missing value");
  }
  if (operation == TIMAO_HOST_WATCH && (*out)->kind != TIMAO_WATCH) {
    *out = NULL;
    return timao_error(error, "type_error",
                       "watch host must return a local watch handle");
  }
  return TIMAO_OK;
}
enum timao_status timao_host_local(struct timao_execution *execution,
                                   enum timao_host_op operation,
                                   const struct timao_value *const *arguments,
                                   size_t count, const struct timao_value **out,
                                   struct timao_diagnostic *error) {
  *out = NULL;
  if (execution->context == TIMAO_PURE)
    return timao_error(error, "invalid_argument",
                       "host operations are forbidden in pure calls");
  const size_t arity = operation == TIMAO_HOST_AWAIT ? 3
                       : operation == TIMAO_HOST_ON  ? 2
                                                     : 1;
  if (count != arity)
    return timao_error(error, "arity_error", "wrong host argument count");
  if (timao_charge(&execution->vm->meter, 1, error) != TIMAO_OK)
    return TIMAO_ERROR;
  switch (operation) {
  case TIMAO_HOST_EMIT: {
    struct leme_public_builder *builder = NULL;
    const enum leme_public_status created = leme_public_builder_create_budget(
        execution->vm->heap.account, execution->vm->limits.memory_bytes,
        &builder);
    if (created != LEME_PUBLIC_OK)
      return timao_error(error,
                         created == LEME_PUBLIC_OOM ? "out_of_memory"
                                                    : "resource_limit",
                         "emit validation allocation failed");
    struct leme_public_value *value = NULL;
    const enum timao_status status =
        timao_export(execution, arguments[0], builder, &value, error);
    leme_public_builder_destroy(builder);
    if (status != TIMAO_OK)
      return status;
    break;
  }
  case TIMAO_HOST_LAUNCH:
    if (arguments[0]->kind != TIMAO_ARRAY)
      return timao_error(error, "type_error", "launch requires an argv array");
    if (arguments[0]->as.array.count == 0)
      return timao_error(error, "invalid_argument",
                         "launch argv must not be empty");
    for (size_t i = 0; i < arguments[0]->as.array.count; ++i) {
      if (timao_charge(&execution->vm->meter, 1, error) != TIMAO_OK)
        return TIMAO_ERROR;
      const struct timao_value *arg = arguments[0]->as.array.items[i];
      if (arg->kind != TIMAO_STRING)
        return timao_error(error, "type_error",
                           "launch arguments must be strings");
      if (i == 0 && arg->as.text.length == 0)
        return timao_error(error, "invalid_argument",
                           "launch executable must not be empty");
      for (size_t offset = 0; offset < arg->as.text.length;) {
        if (timao_charge(&execution->vm->meter, 1, error) != TIMAO_OK)
          return TIMAO_ERROR;
        const size_t bytes = arg->as.text.length - offset > 128
                                 ? 128
                                 : arg->as.text.length - offset;
        if (memchr(arg->as.text.data + offset, 0, bytes) != NULL)
          return timao_error(error, "invalid_argument",
                             "launch argv contains NUL");
        offset += bytes;
      }
    }
    break;
  case TIMAO_HOST_CANCEL:
    if (arguments[0]->kind != TIMAO_WATCH && arguments[0]->kind != TIMAO_STRING)
      return timao_error(error, "type_error",
                         "cancel requires a watch or process-local id string");
    if (arguments[0]->kind == TIMAO_STRING) {
      const struct leme_public_text id = arguments[0]->as.text;
      for (size_t offset = 0; offset < id.length;) {
        if (timao_charge(&execution->vm->meter, 1, error) != TIMAO_OK)
          return TIMAO_ERROR;
        const size_t bytes =
            id.length - offset > 128 ? 128 : id.length - offset;
        if (memchr(id.data + offset, 0, bytes) != NULL)
          return timao_error(error, "invalid_argument",
                             "local watch id contains NUL");
        offset += bytes;
      }
    }
    break;
  case TIMAO_HOST_ON:
  case TIMAO_HOST_AWAIT:
    if (arguments[0]->kind != TIMAO_WATCH ||
        arguments[1]->kind != TIMAO_CALLABLE)
      return timao_error(error, "type_error",
                         "watch and named callable required");
    if (arguments[1]->as.function.arity != 1)
      return timao_error(error, "arity_error",
                         "watch callback requires one event argument");
    if (operation == TIMAO_HOST_AWAIT &&
        (arguments[2]->kind != TIMAO_NUMBER || arguments[2]->as.number < 1 ||
         arguments[2]->as.number > 86400000 ||
         trunc(arguments[2]->as.number) != arguments[2]->as.number))
      return timao_error(
          error, "invalid_argument",
          "await timeout must be an integer from 1 to 86400000 ms");
    break;
  default:
    return timao_error(error, "invalid_argument",
                       "remote boundary passed to local host dispatcher");
  }
  enum timao_status status =
      invoke(execution, operation, NULL, arguments, count, out, error);
  if (status != TIMAO_OK)
    return status;
  if (*out == NULL)
    return timao_error(error, "invalid_argument", "host returned no result");
  if (((operation == TIMAO_HOST_EMIT || operation == TIMAO_HOST_CANCEL) &&
       (*out)->kind != TIMAO_NULL) ||
      (operation == TIMAO_HOST_ON && *out != arguments[0]) ||
      ((operation == TIMAO_HOST_AWAIT || operation == TIMAO_HOST_LAUNCH) &&
       (*out)->kind != TIMAO_OBJECT)) {
    *out = NULL;
    status = timao_error(error, "type_error",
                         "host result violates the operation contract");
  }
  return status;
}
enum timao_status timao_host_request(struct timao_execution *execution,
                                     struct timao_program *program,
                                     uint32_t expression,
                                     struct timao_environment *environment,
                                     enum timao_host_op operation,
                                     const struct timao_value **out,
                                     struct timao_diagnostic *error) {
  *out = NULL;
  if (execution->context == TIMAO_PURE)
    return timao_error(error, "invalid_argument",
                       "remote requests are forbidden in pure calls");
  const enum timao_boundary boundary =
      operation == TIMAO_HOST_QUERY ? TIMAO_QUERY
      : operation == TIMAO_HOST_ACT ? TIMAO_ACT
                                    : TIMAO_WATCH_BOUNDARY;
  struct timao_lowered *descriptor = NULL;
  struct timao_environment *previous = execution->environment;
  execution->environment = environment;
  enum timao_status status =
      timao_lower(execution, program, expression, boundary, &descriptor, error);
  execution->environment = previous;
  if (status != TIMAO_OK)
    return status;
  const struct timao_value *argument = NULL;
  status = timao_import(execution, timao_lowered_value(descriptor), &argument,
                        error);
  if (status == TIMAO_OK) {
    struct timao_heap_root root = {0};
    timao_heap_root_add(&execution->vm->heap, &root, argument->allocation);
    status = invoke(execution, operation, descriptor, &argument, 1, out, error);
    timao_heap_root_remove(&execution->vm->heap, &root);
  }
  timao_lowered_unref(descriptor);
  return status;
}
