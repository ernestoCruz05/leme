#include "control/watch-event.h"
#include "control/reply.h"
#include "ipc/json.h"
#include "public/json.h"

#include <inttypes.h>
#include <stdio.h>

static bool json_work(void *context, size_t units) {
  return leme_control_charge(context, units) == LEME_CONTROL_OK;
}

static enum leme_public_status value_work(void *context, size_t units) {
  return json_work(context, units) ? LEME_PUBLIC_OK : LEME_PUBLIC_LIMIT;
}

enum leme_control_code leme_control_watch_event_create(
    struct leme_public_budget *account, size_t maximum,
    const struct leme_control_watch_event *event,
    struct leme_control_meter *meter, struct leme_control_frame **out) {
  if (out == NULL)
    return LEME_CONTROL_INVALID_ARGUMENT;
  *out = NULL;
  if (event == NULL ||
      (event->kind != LEME_WATCH_SNAPSHOT && event->kind != LEME_WATCH_CHANGE &&
       event->kind != LEME_WATCH_RESET && event->kind != LEME_WATCH_SUSPENDED &&
       event->kind != LEME_WATCH_ERROR) ||
      event->subscription == 0 || event->sequence == 0 ||
      event->instance.data == NULL || event->instance.length == 0 ||
      (event->kind == LEME_WATCH_ERROR       ? event->error == NULL
       : event->kind == LEME_WATCH_SUSPENDED ? false
                                             : event->value == NULL))
    return LEME_CONTROL_INVALID_ARGUMENT;
  if (maximum >= SIZE_MAX - 1)
    return LEME_CONTROL_RESOURCE_LIMIT;

  char subscription[32] = {0};
  char sequence[21] = {0};
  const int sub_length = snprintf(subscription, sizeof(subscription),
                                  "sub:%" PRIu64, event->subscription);
  const int sequence_length =
      snprintf(sequence, sizeof(sequence), "%" PRIu64, event->sequence);
  if (sub_length < 0 || (size_t)sub_length >= sizeof(subscription) ||
      sequence_length < 0 || (size_t)sequence_length >= sizeof(sequence))
    return LEME_CONTROL_RESOURCE_LIMIT;

  struct leme_json json = {0};
  leme_json_init_budget(&json, account, maximum + 1);
  leme_json_set_meter(&json, meter, meter != NULL ? json_work : NULL);
  leme_json_object_begin(&json);
  leme_json_key(&json, "type");
  leme_json_string(&json, "event");
  leme_json_key(&json, "event");
  const bool terminal = event->kind == LEME_WATCH_ERROR;
  const bool suspended = event->kind == LEME_WATCH_SUSPENDED;
  leme_json_string(&json, terminal                             ? "error"
                          : suspended                          ? "suspended"
                          : event->kind == LEME_WATCH_SNAPSHOT ? "snapshot"
                          : event->kind == LEME_WATCH_RESET    ? "reset"
                                                               : "change");
  leme_json_key(&json, "subscription");
  leme_json_string_n(&json, subscription, (size_t)sub_length);
  leme_json_key(&json, "sequence");
  leme_json_string_n(&json, sequence, (size_t)sequence_length);
  leme_json_key(&json, "instance");
  leme_json_string_n(&json, event->instance.data, event->instance.length);
  leme_json_key(&json, "revision");
  if (!terminal && !suspended && event->sensitive &&
      event->revision.data != NULL)
    leme_json_string_n(&json, event->revision.data, event->revision.length);
  else
    leme_json_null(&json);
  bool serialized = false;
  if (terminal) {
    leme_json_key(&json, "error");
    serialized = leme_control_error_write_json(&json, event->error, meter);
  } else if (suspended) {
    leme_json_key(&json, "reason");
    leme_json_string(&json, "session_locked");
    serialized = !json.failed;
  } else {
    if (event->kind == LEME_WATCH_RESET) {
      leme_json_key(&json, "reason");
      leme_json_string(&json, "unlock");
    }
    leme_json_key(&json, "value");
    const struct leme_public_work work = {.context = meter, .step = value_work};
    serialized = leme_public_write_json_work(event->value, &json,
                                             meter != NULL ? &work : NULL) ==
                 LEME_PUBLIC_OK;
  }
  leme_json_object_end(&json);

  enum leme_control_code code = LEME_CONTROL_RESOURCE_LIMIT;
  if (!serialized || json.failed || json.length > maximum)
    goto cleanup;
  leme_json_append(&json, "\n", 1);
  if (json.failed ||
      leme_control_charge(meter, json.length / 128 + 1) != LEME_CONTROL_OK)
    goto cleanup;
  code = leme_control_frame_create(account, json.data, json.length, out);
  if (code == LEME_CONTROL_OK) {
    leme_control_frame_set_metadata(*out,
                                    terminal ? LEME_CONTROL_FRAME_TERMINAL_EVENT
                                             : LEME_CONTROL_FRAME_EVENT,
                                    event->sensitive && !suspended, NULL, 0);
    leme_control_frame_set_subscription(*out, event->subscription);
  }

cleanup:
  if (json.error == LEME_JSON_ERROR_OOM)
    code = LEME_CONTROL_OUT_OF_MEMORY;
  leme_json_finish(&json);
  return code;
}
