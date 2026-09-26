#include "public/json.h"

#include "ipc/json.h"
#include "public/value-internal.h"

static enum leme_public_status writer_status(const struct leme_json *json) {
  if (!json->failed)
    return LEME_PUBLIC_OK;
  switch (json->error) {
  case LEME_JSON_ERROR_OOM:
    return LEME_PUBLIC_OOM;
  case LEME_JSON_ERROR_LIMIT:
    return LEME_PUBLIC_LIMIT;
  case LEME_JSON_ERROR_NONE:
  case LEME_JSON_ERROR_INVALID:
    return LEME_PUBLIC_INVALID;
  }
  return LEME_PUBLIC_INVALID;
}

static enum leme_public_status reject(struct leme_json *json,
                                      enum leme_json_error error) {
  if (!json->failed) {
    json->failed = true;
    json->error = error;
  }
  return writer_status(json);
}

static enum leme_public_status
write_value(const struct leme_public_value *value, struct leme_json *json,
            const struct leme_public_work *work, size_t depth) {
  if (json->failed)
    return writer_status(json);
  if (value == NULL)
    return reject(json, LEME_JSON_ERROR_INVALID);
  if (depth > LEME_PUBLIC_MAX_DEPTH)
    return reject(json, LEME_JSON_ERROR_LIMIT);
  if (work != NULL && work->step != NULL) {
    enum leme_public_status st = work->step(work->context, 1);
    if (st != LEME_PUBLIC_OK)
      return reject(json, LEME_JSON_ERROR_LIMIT);
  }
  switch (leme_public_kind(value)) {
  case LEME_PUBLIC_NULL:
    leme_json_null(json);
    break;
  case LEME_PUBLIC_BOOLEAN: {
    bool boolean = false;
    if (leme_public_as_bool(value, &boolean) != LEME_PUBLIC_OK)
      return reject(json, LEME_JSON_ERROR_INVALID);
    leme_json_bool(json, boolean);
    break;
  }
  case LEME_PUBLIC_NUMBER: {
    int64_t integer = 0;
    if (leme_public_as_integer(value, &integer) == LEME_PUBLIC_OK) {
      leme_json_integer(json, integer);
    } else {
      double number = 0.0;
      if (leme_public_as_number(value, &number) != LEME_PUBLIC_OK)
        return reject(json, LEME_JSON_ERROR_INVALID);
      leme_json_real(json, number);
    }
    break;
  }
  case LEME_PUBLIC_STRING: {
    struct leme_public_text text = {0};
    if (leme_public_as_text(value, &text) != LEME_PUBLIC_OK)
      return reject(json, LEME_JSON_ERROR_INVALID);
    if (work != NULL && work->step != NULL && text.length > 0) {
      enum leme_public_status st = work->step(work->context, text.length);
      if (st != LEME_PUBLIC_OK)
        return reject(json, LEME_JSON_ERROR_LIMIT);
    }
    leme_json_string_n(json, text.data, text.length);
    break;
  }
  case LEME_PUBLIC_ARRAY:
    leme_json_array_begin(json);
    for (size_t i = 0; i < leme_public_length(value) && !json->failed; ++i) {
      const enum leme_public_status status =
          write_value(leme_public_at(value, i), json, work, depth + 1);
      if (status != LEME_PUBLIC_OK)
        return status;
    }
    leme_json_array_end(json);
    break;
  case LEME_PUBLIC_OBJECT:
    leme_json_object_begin(json);
    for (size_t i = 0; i < leme_public_length(value) && !json->failed; ++i) {
      const struct leme_public_text key = leme_public_key_at(value, i);
      leme_json_key_n(json, key.data, key.length);
      const enum leme_public_status status =
          write_value(leme_public_member_at(value, i), json, work, depth + 1);
      if (status != LEME_PUBLIC_OK)
        return status;
    }
    leme_json_object_end(json);
    break;
  }
  return writer_status(json);
}

enum leme_public_status
leme_public_write_json_work(const struct leme_public_value *value,
                            struct leme_json *json,
                            const struct leme_public_work *work) {
  if (json == NULL)
    return LEME_PUBLIC_INVALID;
  if (value == NULL || !value->owner->sealed)
    return reject(json, LEME_JSON_ERROR_INVALID);
  return write_value(value, json, work, 1);
}

enum leme_public_status
leme_public_write_json(const struct leme_public_value *value,
                       struct leme_json *json) {
  return leme_public_write_json_work(value, json, NULL);
}
