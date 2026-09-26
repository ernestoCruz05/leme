
#include "control/reply.h"
#include "control/error.h"
#include "control/memory.h"
#include "ipc/frame.h"
#include "ipc/json.h"
#include "public/budget.h"
#include "public/json.h"
#include "public/value.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

const char *leme_control_code_to_string(enum leme_control_code code) {
  switch (code) {
  case LEME_CONTROL_OK:
    return "ok";
  case LEME_CONTROL_INVALID_JSON:
    return "invalid_json";
  case LEME_CONTROL_INVALID_REQUEST:
    return "invalid_request";
  case LEME_CONTROL_UNSUPPORTED_VERSION:
    return "unsupported_version";
  case LEME_CONTROL_STALE_INSTANCE:
    return "stale_instance";
  case LEME_CONTROL_UNKNOWN_OPERATOR:
    return "unknown_operator";
  case LEME_CONTROL_UNKNOWN_FIELD:
    return "unknown_field";
  case LEME_CONTROL_TYPE_ERROR:
    return "type_error";
  case LEME_CONTROL_INVALID_ARGUMENT:
    return "invalid_argument";
  case LEME_CONTROL_NOT_FOUND:
    return "not_found";
  case LEME_CONTROL_CARDINALITY:
    return "cardinality";
  case LEME_CONTROL_CONFLICTING_TARGETS:
    return "conflicting_targets";
  case LEME_CONTROL_UNSUPPORTED:
    return "unsupported";
  case LEME_CONTROL_SESSION_LOCKED:
    return "session_locked";
  case LEME_CONTROL_RESOURCE_LIMIT:
    return "resource_limit";
  case LEME_CONTROL_OUT_OF_MEMORY:
    return "out_of_memory";
  case LEME_CONTROL_ACTION_FAILED:
    return "action_failed";
  }
  return "error";
}

static enum leme_public_status meter_step_cb(void *context, size_t units) {
  struct leme_control_meter *meter = context;
  if (meter == NULL) {
    return LEME_PUBLIC_OK;
  }
  return leme_control_charge(meter, units) == LEME_CONTROL_OK
             ? LEME_PUBLIC_OK
             : LEME_PUBLIC_LIMIT;
}

static bool json_charge_cb(void *context, size_t units) {
  struct leme_control_meter *meter = context;
  if (meter == NULL) {
    return true;
  }
  return leme_control_charge(meter, units) == LEME_CONTROL_OK;
}

enum leme_control_code leme_control_reply_create_value_metered(
    struct leme_public_budget *account, size_t max_response_bytes,
    const char *id, size_t id_len, const char *instance, size_t instance_len,
    const char *revision, size_t revision_len,
    const struct leme_public_value *value, struct leme_control_meter *meter,
    struct leme_control_frame **out_frame) {
  if (out_frame == NULL) {
    return LEME_CONTROL_INVALID_ARGUMENT;
  }
  *out_frame = NULL;

  struct leme_json json;
  leme_json_init_budget(&json, account, max_response_bytes);
  leme_json_set_meter(&json, meter, meter != NULL ? json_charge_cb : NULL);
  leme_json_object_begin(&json);

  leme_json_key(&json, "type");
  leme_json_string(&json, "reply");

  leme_json_key(&json, "id");
  if (id != NULL) {
    leme_json_string_n(&json, id, id_len);
  } else {
    leme_json_null(&json);
  }

  leme_json_key(&json, "ok");
  leme_json_bool(&json, true);

  leme_json_key(&json, "instance");
  if (instance != NULL) {
    leme_json_string_n(&json, instance, instance_len);
  } else {
    leme_json_null(&json);
  }

  leme_json_key(&json, "revision");
  if (revision != NULL) {
    leme_json_string_n(&json, revision, revision_len);
  } else {
    leme_json_null(&json);
  }

  leme_json_key(&json, "value");
  if (value != NULL) {
    if (meter != NULL) {
      struct leme_public_work work = {
          .context = meter,
          .step = meter_step_cb,
      };
      if (leme_public_write_json_work(value, &json, &work) != LEME_PUBLIC_OK) {
        json.failed = true;
      }
    } else {
      if (leme_public_write_json(value, &json) != LEME_PUBLIC_OK) {
        json.failed = true;
      }
    }
  } else {
    leme_json_null(&json);
  }

  leme_json_object_end(&json);
  leme_json_append(&json, "\n", 1);

  if (json.failed) {
    enum leme_json_error err = json.error;
    leme_json_finish(&json);
    if (err == LEME_JSON_ERROR_LIMIT) {
      return LEME_CONTROL_RESOURCE_LIMIT;
    }
    return LEME_CONTROL_OUT_OF_MEMORY;
  }

  enum leme_control_code code =
      leme_control_frame_create(account, json.data, json.length, out_frame);
  leme_json_finish(&json);
  return code;
}

enum leme_control_code leme_control_reply_create_value(
    struct leme_public_budget *account, size_t max_response_bytes,
    const char *id, size_t id_len, const char *instance, size_t instance_len,
    const char *revision, size_t revision_len,
    const struct leme_public_value *value,
    struct leme_control_frame **out_frame) {
  return leme_control_reply_create_value_metered(
      account, max_response_bytes, id, id_len, instance, instance_len, revision,
      revision_len, value, NULL, out_frame);
}

static const char *phase_to_string(enum leme_control_phase phase) {
  switch (phase) {
  case LEME_CONTROL_DECODE:
    return "decode";
  case LEME_CONTROL_VALIDATE:
    return "validate";
  case LEME_CONTROL_EVALUATE:
    return "evaluate";
  case LEME_CONTROL_PREFLIGHT:
    return "preflight";
  case LEME_CONTROL_EXECUTE:
    return "execute";
  }
  return "evaluate";
}

bool leme_control_error_write_json(struct leme_json *json,
                                   const struct leme_control_error *error,
                                   struct leme_control_meter *meter) {
  if (json == NULL)
    return false;
  const enum leme_control_code code =
      error != NULL ? error->code : LEME_CONTROL_INVALID_REQUEST;
  const char *message = error != NULL && error->message[0] != '\0'
                            ? error->message
                            : leme_control_code_to_string(code);
  leme_json_object_begin(json);
  leme_json_key(json, "code");
  leme_json_string(json, leme_control_code_to_string(code));
  leme_json_key(json, "message");
  leme_json_string(json, message);
  leme_json_key(json, "phase");
  leme_json_string(json,
                   error != NULL ? phase_to_string(error->phase) : "evaluate");
  if (error != NULL && error->expr_path[0] != '\0') {
    leme_json_key(json, "expr_path");
    leme_json_string(json, error->expr_path);
  }
  if (error != NULL && error->details != NULL) {
    leme_json_key(json, "details");
    const struct leme_public_work work = {.context = meter,
                                          .step = meter_step_cb};
    if (leme_public_write_json_work(error->details, json,
                                    meter != NULL ? &work : NULL) !=
        LEME_PUBLIC_OK)
      json->failed = true;
  } else if (error != NULL &&
             (error->phase == LEME_CONTROL_PREFLIGHT ||
              error->phase == LEME_CONTROL_EXECUTE || error->effects_applied)) {
    leme_json_key(json, "details");
    leme_json_object_begin(json);
    leme_json_key(json, "effects_applied");
    leme_json_bool(json, error->effects_applied);
    leme_json_object_end(json);
  }
  leme_json_object_end(json);
  return !json->failed;
}

enum leme_control_code leme_control_reply_create_error_metered(
    struct leme_public_budget *account, size_t max_response_bytes,
    const char *id, size_t id_len, const char *instance, size_t instance_len,
    const char *revision, size_t revision_len,
    const struct leme_control_error *error, struct leme_control_meter *meter,
    struct leme_control_frame **out_frame) {
  if (out_frame == NULL) {
    return LEME_CONTROL_INVALID_ARGUMENT;
  }
  *out_frame = NULL;

  struct leme_json json;
  leme_json_init_budget(&json, account, max_response_bytes);
  leme_json_set_meter(&json, meter, meter != NULL ? json_charge_cb : NULL);
  leme_json_object_begin(&json);

  leme_json_key(&json, "type");
  leme_json_string(&json, "reply");

  leme_json_key(&json, "id");
  if (id != NULL) {
    leme_json_string_n(&json, id, id_len);
  } else {
    leme_json_null(&json);
  }

  leme_json_key(&json, "ok");
  leme_json_bool(&json, false);

  leme_json_key(&json, "instance");
  if (instance != NULL) {
    leme_json_string_n(&json, instance, instance_len);
  } else {
    leme_json_null(&json);
  }

  leme_json_key(&json, "revision");
  if (revision != NULL) {
    leme_json_string_n(&json, revision, revision_len);
  } else {
    leme_json_null(&json);
  }

  leme_json_key(&json, "error");
  if (!leme_control_error_write_json(&json, error, meter))
    json.failed = true;
  leme_json_object_end(&json);
  leme_json_append(&json, "\n", 1);

  if (json.failed) {
    enum leme_json_error err = json.error;
    leme_json_finish(&json);
    if (err == LEME_JSON_ERROR_LIMIT) {
      return LEME_CONTROL_RESOURCE_LIMIT;
    }
    return LEME_CONTROL_OUT_OF_MEMORY;
  }

  enum leme_control_code ret =
      leme_control_frame_create(account, json.data, json.length, out_frame);
  leme_json_finish(&json);
  return ret;
}

enum leme_control_code leme_control_reply_create_error(
    struct leme_public_budget *account, size_t max_response_bytes,
    const char *id, size_t id_len, const char *instance, size_t instance_len,
    const char *revision, size_t revision_len,
    const struct leme_control_error *error,
    struct leme_control_frame **out_frame) {
  return leme_control_reply_create_error_metered(
      account, max_response_bytes, id, id_len, instance, instance_len, revision,
      revision_len, error, NULL, out_frame);
}
