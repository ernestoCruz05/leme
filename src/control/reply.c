
#include "control/reply.h"
#include "control/error.h"
#include "ipc/frame.h"
#include "ipc/json.h"
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

enum leme_control_code
leme_control_reply_create_value(size_t max_response_bytes, const char *id,
                                size_t id_len, const char *instance,
                                size_t instance_len,
                                const struct leme_public_value *value,
                                struct leme_control_frame **out_frame) {
  if (out_frame == NULL) {
    return LEME_CONTROL_INVALID_ARGUMENT;
  }
  *out_frame = NULL;

  struct leme_json json;
  leme_json_init_limit(&json, max_response_bytes);
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

  leme_json_key(&json, "value");
  if (value != NULL) {
    if (leme_public_write_json(value, &json) != LEME_PUBLIC_OK) {
      json.failed = true;
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
      leme_control_frame_create(json.data, json.length, out_frame);
  leme_json_finish(&json);
  return code;
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
                                   const struct leme_control_error *error) {
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
  if (error != NULL && error->has_byte_offset) {
    leme_json_key(json, "offset");
    leme_json_integer(json, error->byte_offset > INT64_MAX
                                ? INT64_MAX
                                : (int64_t)error->byte_offset);
  }
  if (error != NULL && error->details != NULL) {
    leme_json_key(json, "details");
    if (leme_public_write_json(error->details, json) != LEME_PUBLIC_OK)
      json->failed = true;
  } else if (error != NULL && error->has_affected) {
    leme_json_key(json, "details");
    leme_json_object_begin(json);
    leme_json_key(json, "affected");
    leme_json_integer(json, error->affected > INT64_MAX
                                ? INT64_MAX
                                : (int64_t)error->affected);
    leme_json_object_end(json);
  }
  leme_json_object_end(json);
  return !json->failed;
}

enum leme_control_code
leme_control_reply_create_error(size_t max_response_bytes, const char *id,
                                size_t id_len, const char *instance,
                                size_t instance_len,
                                const struct leme_control_error *error,
                                struct leme_control_frame **out_frame) {
  if (out_frame == NULL) {
    return LEME_CONTROL_INVALID_ARGUMENT;
  }
  *out_frame = NULL;

  struct leme_json json;
  leme_json_init_limit(&json, max_response_bytes);
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

  leme_json_key(&json, "error");
  if (!leme_control_error_write_json(&json, error))
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
      leme_control_frame_create(json.data, json.length, out_frame);
  leme_json_finish(&json);
  return ret;
}
