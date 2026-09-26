#include "timao/runtime-internal.h"
#include "timao/launch.h"
#include "timao/value.h"
#include <errno.h>
#include <string.h>

void timao_runtime_launch_tick(struct timao_runtime *runtime, uint64_t now) {
  for (size_t i = 0; i < 16; ++i) {
    struct timao_launch *launch = runtime->launches[i];
    if (launch == NULL)
      continue;
    const enum timao_launch_state state = timao_launch_step(
        launch, now,
        runtime->stopping || runtime->cancelled || runtime->io_error);
    if (launch != runtime->active_launch && state != TIMAO_LAUNCH_PENDING &&
        timao_launch_destroy(launch))
      runtime->launches[i] = NULL;
  }
}

static enum leme_public_status
reserve_unknown(struct timao_runtime *runtime,
                struct leme_public_builder **owner,
                const struct leme_public_value **payload) {
  enum leme_public_status status =
      leme_public_builder_create_budget(runtime->account, 65536, owner);
  if (status != LEME_PUBLIC_OK)
    return status;
  struct leme_public_value *object = NULL;
  status = leme_public_object(*owner, 3, &object);
  if (status == LEME_PUBLIC_OK)
    status = leme_public_put_cstr(*owner, object, LEME_PUBLIC_TEXT("code"),
                                  "launch_outcome_unknown");
  if (status == LEME_PUBLIC_OK)
    status = leme_public_put_cstr(
        *owner, object, LEME_PUBLIC_TEXT("message"),
        "launch handshake ended without an authoritative result; not retried");
  if (status == LEME_PUBLIC_OK)
    status = leme_public_put_cstr(*owner, object, LEME_PUBLIC_TEXT("phase"),
                                  "execute");
  *payload = object;
  if (status == LEME_PUBLIC_OK)
    status = leme_public_builder_seal(*owner, payload, 1);
  return status;
}

enum timao_status timao_runtime_launch_host(struct timao_runtime *runtime,
                                            struct timao_execution *execution,
                                            const struct timao_value *arguments,
                                            const struct timao_value **out,
                                            struct timao_diagnostic *error) {
  size_t slot = 0;
  while (slot < 16 && runtime->launches[slot] != NULL)
    ++slot;
  if (slot == 16)
    return timao_error(error, "resource_limit", "launch helper limit reached");
  struct leme_public_builder *owner = NULL, *unknown_owner = NULL;
  struct leme_public_value *argv = NULL;
  const struct leme_public_value *unknown = NULL;
  const struct timao_value *accepted = NULL, *receipt = NULL;
  uint64_t accepted_pin = 0, receipt_pin = 0, now = 0;
  sigset_t mask = {0};
  enum timao_status status =
      timao_runtime_export(runtime, execution, arguments, &owner, &argv, error);
  if (status != TIMAO_OK)
    goto done;
  status = timao_execution_charge(execution, 16, error);
  if (status != TIMAO_OK)
    goto done;
  enum leme_public_status made =
      reserve_unknown(runtime, &unknown_owner, &unknown);
  if (made != LEME_PUBLIC_OK) {
    status = timao_error(
        error, made == LEME_PUBLIC_OOM ? "out_of_memory" : "resource_limit",
        "unable to reserve launch diagnostic");
    goto done;
  }
  status = timao_value_boolean(runtime->vm, true, &accepted, error);
  if (status == TIMAO_OK)
    status = timao_pin(runtime->vm, accepted, &accepted_pin, error);
  if (status == TIMAO_OK) {
    const struct timao_member member = {.key = LEME_PUBLIC_TEXT("accepted"),
                                        .value = accepted};
    status = timao_value_object(runtime->vm, &member, 1, &receipt, error);
  }
  if (status == TIMAO_OK)
    status = timao_pin(runtime->vm, receipt, &receipt_pin, error);
  if (status != TIMAO_OK)
    goto done;
  if (timao_runtime_now(&now) != LEME_PUBLIC_OK ||
      timao_runtime_signal_mask(runtime->signals, &mask) < 0) {
    status = timao_error(error, "io_error",
                         "unable to prepare launch clock or signals");
    goto done;
  }
  const struct timao_launch_options options = {.now_ms = now,
                                               .child_mask = &mask};
  made = timao_launch_start(runtime->account, argv, &options,
                            &runtime->launches[slot]);
  if (made != LEME_PUBLIC_OK) {
    status = timao_error(error,
                         made == LEME_PUBLIC_OOM     ? "out_of_memory"
                         : made == LEME_PUBLIC_LIMIT ? "resource_limit"
                                                     : "launch_failed",
                         "unable to prepare detached launch");
    goto done;
  }
  runtime->active_launch = runtime->launches[slot];
  const uint64_t deadline = now > UINT64_MAX - 5000 ? UINT64_MAX : now + 5000;
  enum timao_launch_state state = TIMAO_LAUNCH_PENDING;
  while (state == TIMAO_LAUNCH_PENDING) {
    const bool cancelled = timao_runtime_cancelled(runtime);
    if (timao_runtime_now(&now) != LEME_PUBLIC_OK)
      runtime->io_error = true;
    state = timao_launch_step(runtime->active_launch, now,
                              cancelled || runtime->io_error);
    if (state != TIMAO_LAUNCH_PENDING)
      break;
    const enum timao_runtime_wait waited =
        timao_runtime_poll(runtime, deadline);
    if (waited == TIMAO_RUNTIME_IO_ERROR)
      runtime->io_error = true;
  }
  if (state == TIMAO_LAUNCH_ACCEPTED) {
    *out = receipt;
    status = TIMAO_OK;
  } else if (state == TIMAO_LAUNCH_FAILED)
    status = timao_error(error, "launch_failed",
                         strerror(timao_launch_error(runtime->active_launch)));
  else {
    status = timao_diagnostic_take_remote(unknown_owner, unknown, NULL, error);
    unknown_owner = NULL;
  }
  runtime->active_launch = NULL;
  timao_runtime_launch_tick(runtime, now);
done:
  timao_unpin(runtime->vm, receipt_pin);
  timao_unpin(runtime->vm, accepted_pin);
  leme_public_builder_destroy(unknown_owner);
  leme_public_builder_destroy(owner);
  return status;
}
