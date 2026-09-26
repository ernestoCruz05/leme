#include "timao/client-codec.h"

#include <string.h>

static const struct leme_public_value *member(const struct leme_public_value *v,
                                              const char *name) {
  return leme_public_get(v, (struct leme_public_text){name, strlen(name)});
}

static bool text_is(const struct leme_public_value *v, const char *expected) {
  struct leme_public_text text = {0};
  const size_t length = strlen(expected);
  return leme_public_as_text(v, &text) == LEME_PUBLIC_OK &&
         text.length == length && memcmp(text.data, expected, length) == 0;
}

static bool token(const struct leme_public_value *v, size_t maximum) {
  struct leme_public_text text = {0};
  return leme_public_as_text(v, &text) == LEME_PUBLIC_OK && text.length > 0 &&
         text.length <= maximum && memchr(text.data, '\0', text.length) == NULL;
}

static bool null_value(const struct leme_public_value *v) {
  return v != NULL && leme_public_kind(v) == LEME_PUBLIC_NULL;
}

bool timao_client_decimal(const struct leme_public_value *v, uint64_t *out) {
  struct leme_public_text text = {0};
  if (leme_public_as_text(v, &text) != LEME_PUBLIC_OK || text.length == 0 ||
      text.length > 20 || (text.length > 1 && text.data[0] == '0'))
    return false;
  uint64_t number = 0;
  for (size_t i = 0; i < text.length; ++i) {
    const unsigned char ch = (unsigned char)text.data[i];
    if (ch < '0' || ch > '9')
      return false;
    const uint64_t digit = (uint64_t)(ch - '0');
    if (number > (UINT64_MAX - digit) / 10)
      return false;
    number = number * 10 + digit;
  }
  *out = number;
  return true;
}

static bool error_shape(const struct leme_public_value *v) {
  const struct leme_public_value *phase = member(v, "phase");
  const struct leme_public_value *path = member(v, "expr_path");
  const struct leme_public_value *details = member(v, "details");
  return v != NULL && leme_public_kind(v) == LEME_PUBLIC_OBJECT &&
         token(member(v, "code"), SIZE_MAX) &&
         leme_public_kind(member(v, "message")) == LEME_PUBLIC_STRING &&
         (text_is(phase, "decode") || text_is(phase, "validate") ||
          text_is(phase, "evaluate") || text_is(phase, "preflight") ||
          text_is(phase, "execute")) &&
         (path == NULL || leme_public_kind(path) == LEME_PUBLIC_STRING) &&
         (details == NULL || leme_public_kind(details) == LEME_PUBLIC_OBJECT);
}

static bool record_shape(const struct leme_public_value *v) {
  const struct leme_public_value *revision = member(v, "revision");
  const struct leme_public_value *value = member(v, "value");
  const struct leme_public_value *error = member(v, "error");
  uint64_t number = 0;
  if (v == NULL || leme_public_kind(v) != LEME_PUBLIC_OBJECT ||
      !token(member(v, "instance"), SIZE_MAX) ||
      (!null_value(revision) && !timao_client_decimal(revision, &number)))
    return false;
  if (text_is(member(v, "type"), "reply")) {
    bool ok = false;
    const struct leme_public_value *id = member(v, "id");
    if (leme_public_as_bool(member(v, "ok"), &ok) != LEME_PUBLIC_OK ||
        (!token(id, 128) && (ok || !null_value(id))))
      return false;
    return ok ? value != NULL && error == NULL
              : value == NULL && error_shape(error);
  }
  if (!text_is(member(v, "type"), "event") ||
      !token(member(v, "subscription"), 128) ||
      !timao_client_decimal(member(v, "sequence"), &number) || number == 0)
    return false;
  const struct leme_public_value *event = member(v, "event");
  if (text_is(event, "error"))
    return null_value(revision) && value == NULL && error_shape(error);
  if (error != NULL)
    return false;
  if (text_is(event, "suspended"))
    return null_value(revision) && value == NULL &&
           text_is(member(v, "reason"), "session_locked");
  if (text_is(event, "reset"))
    return value != NULL && text_is(member(v, "reason"), "unlock");
  return value != NULL &&
         (text_is(event, "snapshot") || text_is(event, "change"));
}

enum leme_public_status
timao_client_limits_resolve(const struct timao_client_limits *limits,
                            struct timao_client_limits *out) {
  if (out == NULL)
    return LEME_PUBLIC_INVALID;
  const struct timao_client_limits input =
      limits == NULL ? (struct timao_client_limits){0} : *limits;
  const struct timao_client_limits defaults = {.total_bytes = 67108864,
                                               .queued_bytes = 2097152,
                                               .watches = 32,
                                               .handshake_ms = 5000,
                                               .request_ms = 5000,
                                               .shutdown_ms = 250};
  if (input.total_bytes > defaults.total_bytes ||
      input.queued_bytes > defaults.queued_bytes ||
      input.watches > defaults.watches ||
      input.handshake_ms > defaults.handshake_ms ||
      input.request_ms > defaults.request_ms ||
      input.shutdown_ms > defaults.shutdown_ms)
    return LEME_PUBLIC_INVALID;
  *out = (struct timao_client_limits){
      .total_bytes =
          input.total_bytes != 0 ? input.total_bytes : defaults.total_bytes,
      .queued_bytes =
          input.queued_bytes != 0 ? input.queued_bytes : defaults.queued_bytes,
      .watches = input.watches != 0 ? input.watches : defaults.watches,
      .handshake_ms =
          input.handshake_ms != 0 ? input.handshake_ms : defaults.handshake_ms,
      .request_ms =
          input.request_ms != 0 ? input.request_ms : defaults.request_ms,
      .shutdown_ms =
          input.shutdown_ms != 0 ? input.shutdown_ms : defaults.shutdown_ms};
  return LEME_PUBLIC_OK;
}

static enum leme_public_status failure(struct leme_control_error *error,
                                       enum leme_public_status status) {
  if (error != NULL)
    *error = (struct leme_control_error){
        .code = status == LEME_PUBLIC_OOM     ? LEME_CONTROL_OUT_OF_MEMORY
                : status == LEME_PUBLIC_LIMIT ? LEME_CONTROL_RESOURCE_LIMIT
                                              : LEME_CONTROL_INVALID_ARGUMENT,
        .phase = LEME_CONTROL_DECODE};
  return status;
}

enum leme_public_status timao_client_record_decode(
    struct leme_public_budget *account, struct leme_public_text bytes,
    const struct timao_client_limits *limits,
    struct leme_control_document **out, struct leme_control_error *error) {
  if (error != NULL)
    *error = (struct leme_control_error){0};
  if (out == NULL)
    return failure(error, LEME_PUBLIC_INVALID);
  *out = NULL;
  struct timao_client_limits selected = {0};
  if (account == NULL || bytes.data == NULL ||
      timao_client_limits_resolve(limits, &selected) != LEME_PUBLIC_OK)
    return failure(error, LEME_PUBLIC_INVALID);
  struct leme_control_limits decode_limits = leme_control_limits_default();
  decode_limits.request_bytes = decode_limits.response_bytes;
  decode_limits.retained_bytes = selected.queued_bytes;
  if (bytes.length > decode_limits.response_bytes)
    return failure(error, LEME_PUBLIC_LIMIT);
  struct leme_public_budget *owner = NULL;
  const enum leme_public_status status =
      leme_public_budget_child(account, selected.total_bytes, &owner);
  if (status != LEME_PUBLIC_OK)
    return failure(error, status);
  struct leme_control_meter meter = {.remaining = 16777216};
  const enum leme_control_code code =
      leme_control_decode(owner, bytes, &decode_limits, &meter, out, error);
  leme_public_budget_unref(owner);
  if (code != LEME_CONTROL_OK)
    return code == LEME_CONTROL_OUT_OF_MEMORY    ? LEME_PUBLIC_OOM
           : code == LEME_CONTROL_RESOURCE_LIMIT ? LEME_PUBLIC_LIMIT
                                                 : LEME_PUBLIC_INVALID;
  if (record_shape(leme_control_document_value(*out)))
    return LEME_PUBLIC_OK;
  leme_control_document_destroy(*out);
  *out = NULL;
  if (error != NULL)
    *error = (struct leme_control_error){.code = LEME_CONTROL_INVALID_REQUEST,
                                         .phase = LEME_CONTROL_DECODE,
                                         .message = "invalid inbound record"};
  return LEME_PUBLIC_INVALID;
}
