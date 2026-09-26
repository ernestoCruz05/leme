#include "timao/runtime-internal.h"
#include "timao/lower.h"
#include "timao/value.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static struct timao_runtime_watch *find(struct timao_runtime *runtime,
                                        uint64_t id) {
  for (size_t i = 0; i < 32; ++i)
    if (id != 0 && runtime->watches[i].id == id)
      return &runtime->watches[i];
  return NULL;
}

static void release(struct timao_runtime *runtime,
                    struct timao_runtime_watch *watch) {
  if (watch == NULL || watch->id == 0)
    return;
  timao_client_cancel(runtime->client, watch->id);
  timao_unpin(runtime->vm, watch->handler_pin);
  timao_unpin(runtime->vm, watch->handle_pin);
  timao_lowered_unref(watch->descriptor);
  *watch = (struct timao_runtime_watch){0};
}

void timao_runtime_cancel_all(struct timao_runtime *runtime) {
  for (size_t i = 0; i < 32; ++i)
    release(runtime, &runtime->watches[i]);
}

bool timao_runtime_has_handlers(const struct timao_runtime *runtime) {
  if (runtime == NULL)
    return false;
  for (size_t i = 0; i < 32; ++i)
    if (runtime->watches[i].id != 0 &&
        (runtime->watches[i].handler != NULL || runtime->watches[i].printer))
      return true;
  return false;
}

enum timao_status timao_runtime_default_watch(struct timao_runtime *runtime,
                                              uint64_t id,
                                              struct timao_diagnostic *error) {
  struct timao_runtime_watch *watch = find(runtime, id);
  if (watch == NULL || watch->handler != NULL || watch->printer)
    return TIMAO_OK;
  if (watch->awaiting || runtime->output_mode == TIMAO_OUTPUT_RAW)
    return timao_error(error, "type_error",
                       "default watch events require JSON or human output and "
                       "an unattached watch");
  watch->printer = true;
  timao_client_watch_interest(runtime->client, id, true);
  return TIMAO_OK;
}

static enum timao_status register_watch(struct timao_runtime *runtime,
                                        struct timao_execution *execution,
                                        const struct timao_lowered *descriptor,
                                        const struct timao_value **out,
                                        struct timao_diagnostic *error) {
  if (descriptor == NULL)
    return timao_error(error, "invalid_argument", "missing watch descriptor");
  if (timao_runtime_connect(runtime, error) != TIMAO_OK)
    return TIMAO_ERROR;
  struct timao_runtime_watch *watch = NULL;
  for (size_t i = 0; i < 32; ++i)
    if (runtime->watches[i].id == 0) {
      watch = &runtime->watches[i];
      break;
    }
  if (watch == NULL)
    return timao_error(error, "resource_limit", "runtime watch limit reached");
  struct timao_client_ticket ticket = {0};
  const enum leme_public_status admitted = timao_client_watch(
      runtime->client, timao_lowered_value(descriptor), &watch->id, &ticket);
  if (admitted != LEME_PUBLIC_OK)
    return timao_error(error,
                       admitted == LEME_PUBLIC_OOM     ? "out_of_memory"
                       : admitted == LEME_PUBLIC_LIMIT ? "resource_limit"
                                                       : "not_connected",
                       "unable to register watch");
  watch->descriptor = descriptor;
  timao_lowered_ref(descriptor);
  enum timao_status status =
      timao_watch_value(execution, watch->id, &watch->handle, error);
  if (status == TIMAO_OK)
    status = timao_pin(runtime->vm, watch->handle, &watch->handle_pin, error);
  if (status != TIMAO_OK) {
    release(runtime, watch);
    return status;
  }
  struct timao_client_message *message =
      timao_runtime_wait_reply(runtime, ticket);
  if (message == NULL ||
      timao_client_message_kind(message) != TIMAO_CLIENT_REGISTERED) {
    status = timao_runtime_reply(runtime, execution, message, descriptor, out,
                                 error);
    if (status == TIMAO_OK)
      status = timao_error(error, "protocol_error",
                           "missing watch registration acknowledgement");
    release(runtime, watch);
    return status;
  }
  timao_client_message_destroy(message);
  *out = watch->handle;
  return TIMAO_OK;
}

static struct timao_runtime_watch *from_value(struct timao_runtime *runtime,
                                              const struct timao_value *value) {
  uint64_t id = 0;
  if (timao_value_watch_token(value, &id) == TIMAO_OK)
    return find(runtime, id);
  struct leme_public_text text = {0};
  if (timao_value_text(value, &text) != TIMAO_OK)
    return NULL;
  for (size_t i = 0; i < 32; ++i) {
    char local[32] = {0};
    const int count = snprintf(local, sizeof(local), "watch:%" PRIu64,
                               runtime->watches[i].id);
    if (runtime->watches[i].id != 0 && count > 0 &&
        (size_t)count < sizeof(local) && text.length == (size_t)count &&
        memcmp(text.data, local, text.length) == 0)
      return &runtime->watches[i];
  }
  return NULL;
}

static enum timao_status await_watch(struct timao_runtime *runtime,
                                     struct timao_execution *execution,
                                     struct timao_runtime_watch *watch,
                                     const struct timao_value *const *arguments,
                                     const struct timao_value **out,
                                     struct timao_diagnostic *error) {
  double milliseconds = 0;
  if (watch->handler != NULL || watch->printer || watch->awaiting ||
      timao_value_as_number(arguments[2], &milliseconds) != TIMAO_OK ||
      milliseconds < 1 || milliseconds > 86400000)
    return timao_error(
        error, "invalid_argument",
        "await requires an unattached watch and bounded timeout");
  uint64_t now = 0;
  if (timao_runtime_now(&now) != LEME_PUBLIC_OK) {
    release(runtime, watch);
    return timao_error(error, "io_error", "monotonic clock failed");
  }
  const uint64_t delay = (uint64_t)milliseconds;
  const uint64_t until = now > UINT64_MAX - delay ? UINT64_MAX : now + delay;
  watch->awaiting = true;
  timao_client_watch_interest(runtime->client, watch->id, true);
  enum timao_status status = TIMAO_ERROR;
  for (;;) {
    if (timao_execution_charge(execution, 0, error) != TIMAO_OK)
      break;
    if (timao_runtime_now(&now) != LEME_PUBLIC_OK) {
      timao_error(error, "io_error", "monotonic clock failed");
      break;
    }
    if (now >= until) {
      timao_error(error, "timeout", "await deadline expired");
      break;
    }
    timao_client_tick(runtime->client);
    struct timao_client_message *message =
        timao_client_take_watch(runtime->client, watch->id);
    if (message == NULL) {
      const enum timao_runtime_wait waited = timao_runtime_poll(runtime, until);
      if (waited == TIMAO_RUNTIME_CANCELLED ||
          waited == TIMAO_RUNTIME_IO_ERROR) {
        timao_error(
            error, waited == TIMAO_RUNTIME_CANCELLED ? "cancelled" : "io_error",
            "await interrupted");
        break;
      }
      continue;
    }
    const struct leme_public_value *event = timao_client_message_value(message);
    if (timao_client_message_failure(message) != NULL ||
        leme_public_get(event, LEME_PUBLIC_TEXT("error")) != NULL) {
      status = timao_runtime_reply(runtime, execution, message,
                                   watch->descriptor, out, error);
      break;
    }
    if (leme_public_get(event, LEME_PUBLIC_TEXT("value")) != NULL) {
      const struct timao_value *imported = NULL, *match = NULL;
      uint64_t pin = 0;
      status = timao_import(execution, event, &imported, error);
      if (status == TIMAO_OK)
        status = timao_pin(runtime->vm, imported, &pin, error);
      if (status == TIMAO_OK)
        status = timao_call(execution, arguments[1], &imported, 1, TIMAO_PURE,
                            &match, error);
      if (status == TIMAO_OK) {
        if (timao_runtime_now(&now) != LEME_PUBLIC_OK)
          status = timao_error(error, "io_error", "monotonic clock failed");
        else if (now >= until)
          status = timao_error(error, "timeout", "await deadline expired");
      }
      bool accepted = false;
      if (status == TIMAO_OK &&
          timao_value_as_boolean(match, &accepted) != TIMAO_OK)
        status = timao_error(error, "type_error",
                             "await predicate must return boolean");
      timao_client_message_destroy(message);
      timao_unpin(runtime->vm, pin);
      if (status != TIMAO_OK || accepted) {
        if (status == TIMAO_OK)
          *out = imported;
        break;
      }
      status = TIMAO_ERROR;
    } else
      timao_client_message_destroy(message);
  }
  release(runtime, watch);
  return status;
}

enum timao_status timao_runtime_watch_host(
    struct timao_runtime *runtime, struct timao_execution *execution,
    enum timao_host_op operation, const struct timao_lowered *descriptor,
    const struct timao_value *const *arguments, size_t count,
    const struct timao_value **out, struct timao_diagnostic *error) {
  if (operation == TIMAO_HOST_WATCH && count == 1)
    return register_watch(runtime, execution, descriptor, out, error);
  struct timao_runtime_watch *watch = from_value(runtime, arguments[0]);
  if (operation == TIMAO_HOST_CANCEL && count == 1) {
    release(runtime, watch);
    return timao_value_null(runtime->vm, out, error);
  }
  if (watch == NULL)
    return timao_error(error, "invalid_argument", "unknown local watch");
  if (operation == TIMAO_HOST_AWAIT && count == 3)
    return await_watch(runtime, execution, watch, arguments, out, error);
  if (operation == TIMAO_HOST_ON && count == 2) {
    if (watch->handler != NULL || watch->printer || watch->awaiting)
      return timao_error(error, "invalid_argument", "watch already attached");
    if (timao_pin(runtime->vm, arguments[1], &watch->handler_pin, error) !=
        TIMAO_OK)
      return TIMAO_ERROR;
    watch->handler = arguments[1];
    timao_client_watch_interest(runtime->client, watch->id, true);
    *out = arguments[0];
    return TIMAO_OK;
  }
  return timao_error(error, "invalid_argument", "invalid watch operation");
}

int timao_runtime_dispatch(struct timao_runtime *runtime) {
  if (runtime == NULL || runtime->executing)
    return 1;
  if (runtime->stopping)
    return 0;
  runtime->executing = true;
  int result = 0;
  for (size_t turn = 0; turn < 32; ++turn) {
    if (timao_runtime_cancelled(runtime)) {
      if (runtime->diagnostic.code[0] == '\0')
        timao_error(&runtime->diagnostic,
                    runtime->io_error ? "io_error" : "cancelled",
                    "handler dispatch interrupted");
      result = 1;
      break;
    }
    uint64_t handlers[32] = {0};
    size_t count = 0;
    for (size_t i = 0; i < 32; ++i)
      if (runtime->watches[i].handler != NULL || runtime->watches[i].printer)
        handlers[count++] = runtime->watches[i].id;
    struct timao_client_message *message =
        timao_client_take_handlers(runtime->client, handlers, count);
    if (message == NULL)
      break;
    const uint64_t id = timao_client_message_watch(message);
    struct timao_runtime_watch *watch = find(runtime, id);
    const struct timao_client_failure *failure =
        timao_client_message_failure(message);
    if (watch == NULL) {
      const bool exhausted =
          failure != NULL && failure->cause == TIMAO_CLIENT_RESOURCE;
      timao_client_message_destroy(message);
      if (exhausted) {
        timao_error(&runtime->diagnostic, "resource_limit",
                    "client resources exhausted");
        result = 1;
        break;
      }
      continue;
    }
    const struct leme_public_value *event = timao_client_message_value(message);
    if (failure != NULL ||
        leme_public_get(event, LEME_PUBLIC_TEXT("error")) != NULL) {
      timao_runtime_reply(runtime, NULL, message, watch->descriptor, NULL,
                          &runtime->diagnostic);
      release(runtime, watch);
      result = 1;
      break;
    }
    const struct timao_value *ignored = NULL;
    const enum timao_status status =
        watch->printer
            ? timao_runtime_emit(runtime, event, &runtime->diagnostic)
            : timao_invoke_handler_json(runtime->vm, watch->handler, event,
                                        &ignored, &runtime->diagnostic);
    timao_client_message_destroy(message);
    if (status != TIMAO_OK) {
      release(runtime, find(runtime, id));
      result = 1;
      break;
    }
  }
  runtime->executing = false;
  if (result != 0 && !runtime->repl)
    timao_runtime_cancel_all(runtime);
  return result;
}
