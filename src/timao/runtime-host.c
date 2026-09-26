#include "timao/runtime-internal.h"
#include "timao/value.h"

struct seal_context {
  struct timao_execution *execution;
  struct timao_diagnostic *error;
};

static enum leme_public_status seal_work(void *context, size_t units) {
  struct seal_context *work = context;
  return timao_execution_charge(work->execution, units, work->error) == TIMAO_OK
             ? LEME_PUBLIC_OK
             : LEME_PUBLIC_LIMIT;
}

static enum timao_status resource_error(struct timao_diagnostic *error,
                                        enum leme_public_status status) {
  return timao_error(
      error, status == LEME_PUBLIC_OOM ? "out_of_memory" : "resource_limit",
      "unable to export host value");
}

enum timao_status timao_runtime_export(struct timao_runtime *runtime,
                                       struct timao_execution *execution,
                                       const struct timao_value *value,
                                       struct leme_public_builder **owner_out,
                                       struct leme_public_value **json_out,
                                       struct timao_diagnostic *error) {
  *owner_out = NULL;
  *json_out = NULL;
  struct leme_public_builder *owner = NULL;
  enum leme_public_status made =
      leme_public_builder_create_budget(runtime->account, 67108864, &owner);
  if (made != LEME_PUBLIC_OK)
    return resource_error(error, made);
  struct leme_public_value *json = NULL;
  enum timao_status status =
      timao_export(execution, value, owner, &json, error);
  if (status == TIMAO_OK) {
    struct seal_context context = {.execution = execution, .error = error};
    const struct leme_public_work work = {.context = &context,
                                          .step = seal_work};
    const struct leme_public_value *roots[] = {json};
    leme_public_builder_set_work(owner, &work);
    made = leme_public_builder_seal(owner, roots, 1);
    leme_public_builder_set_work(owner, NULL);
    if (made != LEME_PUBLIC_OK)
      status = error != NULL && error->code[0] != '\0'
                   ? TIMAO_ERROR
                   : resource_error(error, made);
  }
  if (status != TIMAO_OK) {
    leme_public_builder_destroy(owner);
    return status;
  }
  *owner_out = owner;
  *json_out = json;
  return TIMAO_OK;
}

static enum timao_status emit_value(struct timao_runtime *runtime,
                                    struct timao_execution *execution,
                                    const struct timao_value *value,
                                    const struct timao_value **out,
                                    struct timao_diagnostic *error) {
  struct leme_public_builder *owner = NULL;
  struct leme_public_value *json = NULL;
  enum timao_status status =
      timao_runtime_export(runtime, execution, value, &owner, &json, error);
  if (status == TIMAO_OK)
    status = timao_runtime_emit(runtime, json, error);
  leme_public_builder_destroy(owner);
  if (status != TIMAO_OK)
    return status;
  return timao_value_null(timao_execution_vm(execution), out, error);
}

enum timao_status timao_runtime_host(
    void *context, struct timao_execution *execution,
    enum timao_host_op operation, const struct timao_lowered *descriptor,
    const struct timao_value *const *arguments, size_t count,
    const struct timao_value **out, struct timao_diagnostic *error) {
  if (out == NULL)
    return timao_error(error, "invalid_argument", "missing host result");
  *out = NULL;
  struct timao_runtime *runtime = context;
  if (runtime == NULL || timao_execution_vm(execution) != runtime->vm ||
      count == 0 || arguments == NULL)
    return timao_error(error, "invalid_argument", "invalid runtime host call");
  if (timao_execution_charge(execution, 0, error) != TIMAO_OK)
    return TIMAO_ERROR;
  if (operation == TIMAO_HOST_WATCH || operation == TIMAO_HOST_ON ||
      operation == TIMAO_HOST_CANCEL || operation == TIMAO_HOST_AWAIT)
    return timao_runtime_watch_host(runtime, execution, operation, descriptor,
                                    arguments, count, out, error);
  if ((operation == TIMAO_HOST_QUERY || operation == TIMAO_HOST_ACT) &&
      count == 1)
    return timao_runtime_request(runtime, execution, operation, descriptor, out,
                                 error);
  if (operation == TIMAO_HOST_LAUNCH && count == 1)
    return timao_runtime_launch_host(runtime, execution, arguments[0], out,
                                     error);
  if (operation == TIMAO_HOST_EMIT && count == 1)
    return emit_value(runtime, execution, arguments[0], out, error);
  return timao_error(error, "unsupported",
                     "runtime host operation unavailable");
}
