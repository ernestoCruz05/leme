#include "timao/runtime-internal.h"
#include "timao/lower.h"
#include "timao/value.h"

#include <signal.h>
#include <string.h>

static uint64_t client_now(void *context) {
  struct timao_runtime *runtime = context;
  uint64_t now = 0;
  if (timao_runtime_now(&now) != LEME_PUBLIC_OK) {
    runtime->io_error = true;
    return UINT64_MAX;
  }
  return now;
}

static uint64_t deadline(struct timao_runtime *runtime) {
  const uint64_t now = client_now(runtime);
  return now > UINT64_MAX - 5000 ? UINT64_MAX : now + 5000;
}

static enum timao_status admission_error(struct timao_diagnostic *error,
                                         enum leme_public_status status) {
  return timao_error(error,
                     status == LEME_PUBLIC_OOM     ? "out_of_memory"
                     : status == LEME_PUBLIC_LIMIT ? "resource_limit"
                                                   : "not_connected",
                     "unable to submit runtime request");
}

enum timao_status timao_runtime_connect(struct timao_runtime *runtime,
                                        struct timao_diagnostic *error) {
  if (runtime->client != NULL)
    return timao_client_ready(runtime->client)
               ? TIMAO_OK
               : timao_error(error, "not_connected",
                             "transport is not ready; request was not queued");
  if (runtime->endpoint_status != LEME_PUBLIC_OK)
    return timao_error(error, "connection_error", "no valid control endpoint");
  const struct timao_client_clock clock = {.context = runtime,
                                           .now_ms = client_now};
  enum leme_public_status status =
      timao_client_create(runtime->account, NULL, &clock, &runtime->client);
  if (status != LEME_PUBLIC_OK)
    return admission_error(error, status);
  status = timao_transport_create(runtime->account, &runtime->endpoint,
                                  runtime->client, &runtime->transport);
  if (status == LEME_PUBLIC_OK)
    status = timao_client_start(runtime->client);
  if (status != LEME_PUBLIC_OK)
    return admission_error(error, status);
  const uint64_t until = deadline(runtime);
  while (!timao_client_ready(runtime->client)) {
    timao_client_tick(runtime->client);
    struct timao_client_message *notice = timao_client_take(runtime->client);
    if (notice != NULL) {
      timao_client_message_destroy(notice);
      return timao_error(error, "connection_error",
                         "initial connection or negotiation failed");
    }
    const enum timao_runtime_wait waited = timao_runtime_poll(runtime, until);
    if (waited != TIMAO_RUNTIME_PROGRESS) {
      const enum timao_client_cause cause =
          waited == TIMAO_RUNTIME_CANCELLED ? TIMAO_CLIENT_CANCELLED
          : waited == TIMAO_RUNTIME_TIMEOUT ? TIMAO_CLIENT_TIMEOUT
                                            : TIMAO_CLIENT_IO_ERROR;
      timao_client_lost(
          runtime->client,
          (struct timao_client_loss){
              .epoch = timao_client_connect_epoch(runtime->client),
              .cause = cause});
      timao_transport_step(runtime->transport, 0);
      return timao_error(error,
                         waited == TIMAO_RUNTIME_CANCELLED ? "cancelled"
                                                           : "connection_error",
                         "initial connection interrupted");
    }
  }
  return TIMAO_OK;
}

static bool repl_interrupted(const struct timao_runtime *runtime) {
  return runtime->repl && runtime->cancelled && !runtime->io_error &&
         runtime->caught_signal != SIGTERM;
}

struct timao_client_message *
timao_runtime_wait_reply(struct timao_runtime *runtime,
                         struct timao_client_ticket ticket) {
  const uint64_t until = deadline(runtime);
  for (;;) {
    timao_client_tick(runtime->client);
    struct timao_client_message *message =
        timao_client_take_reply(runtime->client, ticket);
    if (message != NULL)
      return message;
    const enum timao_runtime_wait waited = timao_runtime_poll(runtime, until);
    if (waited != TIMAO_RUNTIME_PROGRESS) {
      const enum timao_client_cause cause =
          waited == TIMAO_RUNTIME_CANCELLED && !repl_interrupted(runtime)
              ? TIMAO_CLIENT_CANCELLED
          : waited == TIMAO_RUNTIME_TIMEOUT ? TIMAO_CLIENT_TIMEOUT
                                            : TIMAO_CLIENT_IO_ERROR;
      timao_client_lost(
          runtime->client,
          (struct timao_client_loss){.epoch = ticket.epoch, .cause = cause});
      timao_transport_step(runtime->transport, 0);
      return timao_client_take_reply(runtime->client, ticket);
    }
  }
}

static void release_message(void *owner) {
  timao_client_message_destroy(owner);
}

static enum timao_client_cause
request_cause(const struct timao_runtime *runtime,
              const struct timao_client_failure *failure) {
  return repl_interrupted(runtime) && failure->cause == TIMAO_CLIENT_IO_ERROR
             ? TIMAO_CLIENT_CANCELLED
             : failure->cause;
}

static enum timao_status failure_value(
    struct timao_runtime *runtime, const struct timao_client_failure *failure,
    const struct timao_lowered *descriptor, struct timao_diagnostic *error) {
  const enum timao_client_cause cause = request_cause(runtime, failure);
  const char *code = failure->outcome_unknown          ? "outcome_unknown"
                     : cause == TIMAO_CLIENT_TIMEOUT   ? "timeout"
                     : cause == TIMAO_CLIENT_CANCELLED ? "cancelled"
                     : cause == TIMAO_CLIENT_RESOURCE  ? "resource_limit"
                                                       : "transport_error";
  const char *text = failure->outcome_unknown
                         ? "action outcome unknown; do not replay"
                         : "request failed without action effects";
  struct leme_public_builder *owner = NULL;
  struct leme_public_value *payload = NULL;
  enum leme_public_status status =
      leme_public_builder_create_budget(runtime->account, 2097152, &owner);
  if (status == LEME_PUBLIC_OK)
    status = leme_public_object(owner, 6, &payload);
  if (status == LEME_PUBLIC_OK)
    status =
        leme_public_put_cstr(owner, payload, LEME_PUBLIC_TEXT("code"), code);
  if (status == LEME_PUBLIC_OK)
    status =
        leme_public_put_cstr(owner, payload, LEME_PUBLIC_TEXT("message"), text);
  if (status == LEME_PUBLIC_OK)
    status = leme_public_put_bool(owner, payload,
                                  LEME_PUBLIC_TEXT("outcome_unknown"),
                                  failure->outcome_unknown);
  if (status == LEME_PUBLIC_OK)
    status = leme_public_put_bool(owner, payload,
                                  LEME_PUBLIC_TEXT("effects_known_none"),
                                  failure->effects_known_none);
  if (status == LEME_PUBLIC_OK)
    status =
        leme_public_put_text(owner, payload, LEME_PUBLIC_TEXT("request_id"),
                             failure->request_id, false);
  if (status == LEME_PUBLIC_OK)
    status = leme_public_put_text(owner, payload, LEME_PUBLIC_TEXT("instance"),
                                  failure->instance, false);
  const struct leme_public_value *roots[] = {payload};
  if (status == LEME_PUBLIC_OK)
    status = leme_public_builder_seal(owner, roots, 1);
  if (status != LEME_PUBLIC_OK) {
    leme_public_builder_destroy(owner);
    return timao_error(error, code, text);
  }
  return timao_diagnostic_take_remote(owner, payload, descriptor, error);
}

enum timao_status timao_runtime_reply(struct timao_runtime *runtime,
                                      struct timao_execution *execution,
                                      struct timao_client_message *message,
                                      const struct timao_lowered *descriptor,
                                      const struct timao_value **out,
                                      struct timao_diagnostic *error) {
  if (message == NULL)
    return timao_error(error, "transport_error", "missing request completion");
  const struct timao_client_failure *failure =
      timao_client_message_failure(message);
  if (failure != NULL) {
    const enum timao_status status =
        failure_value(runtime, failure, descriptor, error);
    timao_client_message_destroy(message);
    return status;
  }
  const struct leme_public_value *envelope =
      timao_client_message_value(message);
  const struct leme_public_value *remote =
      leme_public_get(envelope, LEME_PUBLIC_TEXT("error"));
  if (remote != NULL)
    return timao_diagnostic_take_owned(message, release_message, remote,
                                       descriptor, error);
  const enum timao_status status = timao_import(
      execution, leme_public_get(envelope, LEME_PUBLIC_TEXT("value")), out,
      error);
  timao_client_message_destroy(message);
  return status;
}

struct failure_reserve {
  struct leme_public_builder *owner;
  const struct leme_public_value *values[5];
};

static enum leme_public_status
reserve_failure(struct timao_runtime *runtime,
                struct timao_client_ticket ticket,
                struct failure_reserve *reserve) {
  struct leme_public_text id = {0}, instance = {0};
  if (!timao_client_request_identity(runtime->client, ticket, &id, &instance))
    return LEME_PUBLIC_INVALID;
  enum leme_public_status status = leme_public_builder_create_budget(
      runtime->account, 2097152, &reserve->owner);
  struct leme_public_value *id_value = NULL, *instance_value = NULL;
  if (status == LEME_PUBLIC_OK)
    status = leme_public_string(reserve->owner, id, false, &id_value);
  if (status == LEME_PUBLIC_OK)
    status =
        leme_public_string(reserve->owner, instance, false, &instance_value);
  static const char *const codes[] = {"outcome_unknown", "timeout", "cancelled",
                                      "transport_error", "resource_limit"};
  for (size_t i = 0; status == LEME_PUBLIC_OK && i < 5; ++i) {
    struct leme_public_value *value = NULL;
    status = leme_public_object(reserve->owner, 6, &value);
    if (status == LEME_PUBLIC_OK)
      status = leme_public_put_cstr(reserve->owner, value,
                                    LEME_PUBLIC_TEXT("code"), codes[i]);
    if (status == LEME_PUBLIC_OK)
      status = leme_public_put_cstr(
          reserve->owner, value, LEME_PUBLIC_TEXT("message"),
          i == 0 ? "action outcome unknown; do not replay"
                 : "request failed without action effects");
    if (status == LEME_PUBLIC_OK)
      status = leme_public_put_bool(
          reserve->owner, value, LEME_PUBLIC_TEXT("outcome_unknown"), i == 0);
    if (status == LEME_PUBLIC_OK)
      status =
          leme_public_put_bool(reserve->owner, value,
                               LEME_PUBLIC_TEXT("effects_known_none"), i != 0);
    if (status == LEME_PUBLIC_OK)
      status = leme_public_object_set(reserve->owner, value,
                                      LEME_PUBLIC_TEXT("request_id"), id_value);
    if (status == LEME_PUBLIC_OK)
      status = leme_public_object_set(
          reserve->owner, value, LEME_PUBLIC_TEXT("instance"), instance_value);
    reserve->values[i] = value;
  }
  if (status == LEME_PUBLIC_OK)
    status = leme_public_builder_seal(reserve->owner, reserve->values, 5);
  return status;
}

enum timao_status timao_runtime_request(struct timao_runtime *runtime,
                                        struct timao_execution *execution,
                                        enum timao_host_op operation,
                                        const struct timao_lowered *descriptor,
                                        const struct timao_value **out,
                                        struct timao_diagnostic *error) {
  if (descriptor == NULL)
    return timao_error(error, "invalid_argument", "missing request descriptor");
  if (timao_runtime_connect(runtime, error) != TIMAO_OK)
    return TIMAO_ERROR;
  struct timao_client_ticket ticket = {0};
  const enum leme_public_status status = timao_client_request(
      runtime->client,
      operation == TIMAO_HOST_QUERY ? TIMAO_CLIENT_QUERY : TIMAO_CLIENT_ACT,
      timao_lowered_value(descriptor), &ticket);
  if (status != LEME_PUBLIC_OK)
    return admission_error(error, status);
  struct failure_reserve reserve = {0};
  const enum leme_public_status reserved =
      reserve_failure(runtime, ticket, &reserve);
  if (reserved != LEME_PUBLIC_OK) {
    leme_public_builder_destroy(reserve.owner);
    timao_client_lost(runtime->client, (struct timao_client_loss){
                                           .epoch = ticket.epoch,
                                           .cause = TIMAO_CLIENT_RESOURCE});
    timao_transport_step(runtime->transport, 0);
    timao_client_message_destroy(
        timao_client_take_reply(runtime->client, ticket));
    return admission_error(error, reserved);
  }
  struct timao_client_message *message =
      timao_runtime_wait_reply(runtime, ticket);
  const struct timao_client_failure *failure =
      timao_client_message_failure(message);
  if (failure != NULL) {
    const enum timao_client_cause cause = request_cause(runtime, failure);
    const size_t index = failure->outcome_unknown          ? 0
                         : cause == TIMAO_CLIENT_TIMEOUT   ? 1
                         : cause == TIMAO_CLIENT_CANCELLED ? 2
                         : cause == TIMAO_CLIENT_RESOURCE  ? 4
                                                           : 3;
    timao_client_message_destroy(message);
    return timao_diagnostic_take_remote(reserve.owner, reserve.values[index],
                                        descriptor, error);
  }
  leme_public_builder_destroy(reserve.owner);
  return timao_runtime_reply(runtime, execution, message, descriptor, out,
                             error);
}
