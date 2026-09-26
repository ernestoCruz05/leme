#include "config/public-internal.h"
#include "config/live-internal.h"
#include "public/value-internal.h"
#include "public/model.h"
#include "public/schema.h"
#include "public/server.h"
#include "core/server.h"
#include "protocols/session.h"

#include <stdint.h>
#include <string.h>

static enum leme_public_status
capture_settings(struct leme_public_builder *b,
                 const struct leme_config *config,
                 struct leme_public_value **out) {
  struct leme_public_value *scalars = NULL, *rules = NULL, *base = NULL,
                           *animation = NULL, *wrapper = NULL;
  if (leme_config_public_scalars(b, config, &scalars) != LEME_PUBLIC_OK ||
      leme_config_public_rules(b, config, &rules) != LEME_PUBLIC_OK ||
      leme_config_public_merge(b, scalars, rules, &base) != LEME_PUBLIC_OK ||
      leme_config_public_animation(b, config, &animation) != LEME_PUBLIC_OK ||
      leme_public_object(b, 1, &wrapper) != LEME_PUBLIC_OK ||
      leme_public_object_set(b, wrapper, LEME_PUBLIC_TEXT("animation"),
                             animation) != LEME_PUBLIC_OK ||
      leme_config_public_merge(b, base, wrapper, out) != LEME_PUBLIC_OK) {
    return leme_public_builder_status(b);
  }
  return LEME_PUBLIC_OK;
}

static enum leme_public_status
capture_overrides(struct leme_public_builder *b,
                  const struct leme_server *server,
                  struct leme_public_value **out) {
  const struct leme_config_store *store = server->config_store;
  size_t count = store != NULL ? store->override_count : 0;
  struct leme_public_value *arr = NULL;
  if (leme_public_array(b, count, &arr) != LEME_PUBLIC_OK) {
    return leme_public_builder_status(b);
  }
  if (store == NULL || count == 0) {
    *out = arr;
    return LEME_PUBLIC_OK;
  }
  for (size_t i = 0; i < count; ++i) {
    const struct leme_scoped_override *ov = &store->overrides[i];
    struct leme_public_value *obj = NULL;
    if (leme_public_object(b, 3, &obj) != LEME_PUBLIC_OK) {
      return leme_public_builder_status(b);
    }
    if (ov->has_target) {
      if (server->public_model == NULL) {
        return leme_public_fail(b, LEME_PUBLIC_UNAVAILABLE);
      }
      struct leme_public_value *target_val = NULL;
      if (leme_public_ref_value(b, server->public_model, ov->target.kind,
                                ov->target.id, ov->target.tag_number,
                                &target_val) != LEME_PUBLIC_OK ||
          leme_public_object_set(b, obj, LEME_PUBLIC_TEXT("target"),
                                 target_val) != LEME_PUBLIC_OK) {
        return leme_public_builder_status(b);
      }
    } else {
      if (leme_public_put_null(b, obj, LEME_PUBLIC_TEXT("target")) !=
          LEME_PUBLIC_OK) {
        return leme_public_builder_status(b);
      }
    }
    struct leme_public_value *path_arr = NULL;
    if (leme_public_array(b, ov->path_count, &path_arr) != LEME_PUBLIC_OK) {
      return leme_public_builder_status(b);
    }
    for (size_t p = 0; p < ov->path_count; ++p) {
      struct leme_public_value *str = NULL;
      if (leme_public_string(
              b, (struct leme_public_text){ov->path[p], strlen(ov->path[p])},
              false, &str) != LEME_PUBLIC_OK ||
          leme_public_array_set(b, path_arr, p, str) != LEME_PUBLIC_OK) {
        return leme_public_builder_status(b);
      }
    }
    if (leme_public_object_set(b, obj, LEME_PUBLIC_TEXT("path"), path_arr) !=
        LEME_PUBLIC_OK) {
      return leme_public_builder_status(b);
    }
    struct leme_public_value *cloned_val = NULL;
    if (leme_public_clone(b, ov->value, &cloned_val) != LEME_PUBLIC_OK ||
        leme_public_object_set(b, obj, LEME_PUBLIC_TEXT("value"),
                               cloned_val) != LEME_PUBLIC_OK) {
      return leme_public_builder_status(b);
    }
    if (leme_public_array_set(b, arr, i, obj) != LEME_PUBLIC_OK) {
      return leme_public_builder_status(b);
    }
  }
  *out = arr;
  return LEME_PUBLIC_OK;
}

enum leme_public_status
leme_config_public_capture(struct leme_public_builder *b,
                           const struct leme_server *server,
                           struct leme_public_value **out) {
  if (out == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  *out = NULL;
  if (server == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  if (leme_session_locked(server))
    return leme_public_fail(b, LEME_PUBLIC_LOCKED);
  if (server->config == NULL)
    return leme_public_fail(b, LEME_PUBLIC_UNAVAILABLE);
  const struct leme_config *baseline =
      server->config_store != NULL ? server->config_store->baseline
                                   : server->config;
  const struct leme_config *effective =
      server->config_store != NULL ? server->config_store->effective
                                   : server->config;
  const struct leme_public_features features = leme_public_server_features(server);
  struct leme_public_value *loaded_settings = NULL;
  struct leme_public_value *effective_settings = NULL;
  struct leme_public_value *metadata = NULL;
  struct leme_public_value *diagnostics = NULL;
  struct leme_public_value *overrides = NULL;
  struct leme_public_value *record = NULL;

  if (capture_settings(b, baseline, &loaded_settings) != LEME_PUBLIC_OK) {
    return leme_public_builder_status(b);
  }
  if (baseline == effective) {
    effective_settings = loaded_settings;
  } else if (capture_settings(b, effective, &effective_settings) !=
             LEME_PUBLIC_OK) {
    return leme_public_builder_status(b);
  }
  if (leme_config_public_diagnostics(b, baseline, &diagnostics) !=
          LEME_PUBLIC_OK ||
      leme_public_settings_schema_value(b, effective_settings, &features,
                                        &metadata) != LEME_PUBLIC_OK ||
      capture_overrides(b, server, &overrides) != LEME_PUBLIC_OK ||
      leme_public_object(b, 6, &record) != LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("path"),
                           baseline->path) != LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("loaded"),
                             loaded_settings) != LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("effective"),
                             effective_settings) != LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("overrides"),
                             overrides) != LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("diagnostics"),
                             diagnostics) != LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("settings_schema"),
                             metadata) != LEME_PUBLIC_OK) {
    return leme_public_builder_status(b);
  }
  const enum leme_public_status status = leme_public_schema_validate(
      leme_public_root_schema(LEME_PUBLIC_CONFIG), record);
  if (status != LEME_PUBLIC_OK) {
    return leme_public_fail(b, status);
  }
  *out = record;
  return LEME_PUBLIC_OK;
}

enum leme_public_status
leme_config_public_text(struct leme_public_builder *b, const char *text,
                        struct leme_public_value **out) {
  if (out == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  *out = NULL;
  if (text == NULL)
    return leme_public_null(b, out);
  if (leme_public_mutable(b) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  const size_t length = strnlen(text, b->maximum);
  if (length == b->maximum)
    return leme_public_fail(b, LEME_PUBLIC_LIMIT);
  return leme_public_string(b, (struct leme_public_text){text, length}, true,
                            out);
}

enum leme_public_status
leme_config_public_redacted(struct leme_public_builder *b,
                            struct leme_public_value **out) {
  if (out == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  *out = NULL;
  struct leme_public_value *value = NULL;
  if (leme_public_object(b, 1, &value) != LEME_PUBLIC_OK ||
      leme_public_put_bool(b, value, LEME_PUBLIC_TEXT("redacted"), true) !=
          LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  *out = value;
  return LEME_PUBLIC_OK;
}

static bool merge_slot(struct leme_public_text key) {
  return (key.length == 4 && memcmp(key.data, "tags", 4) == 0) ||
         (key.length == 7 && memcmp(key.data, "pointer", 7) == 0);
}

static enum leme_public_status
merge_records(struct leme_public_builder *b,
              const struct leme_public_value *lhs,
              const struct leme_public_value *rhs, bool allow_slots,
              struct leme_public_value **out) {
  if (lhs == NULL || rhs == NULL || lhs->owner != b || rhs->owner != b ||
      leme_public_kind(lhs) != LEME_PUBLIC_OBJECT ||
      leme_public_kind(rhs) != LEME_PUBLIC_OBJECT)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  const size_t left_count = leme_public_length(lhs),
               right_count = leme_public_length(rhs);
  if (right_count > SIZE_MAX - left_count)
    return leme_public_fail(b, LEME_PUBLIC_LIMIT);
  size_t count = left_count + right_count;
  for (size_t i = 0; i < right_count; ++i) {
    const struct leme_public_text key = leme_public_key_at(rhs, i);
    if (leme_public_get(lhs, key) == NULL)
      continue;
    if (!allow_slots || !merge_slot(key))
      return leme_public_fail(b, LEME_PUBLIC_INVALID);
    --count;
  }
  struct leme_public_value *record = NULL;
  if (leme_public_object(b, count, &record) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  for (size_t i = 0; i < left_count; ++i) {
    const struct leme_public_text key = leme_public_key_at(lhs, i);
    const struct leme_public_value *value = leme_public_member_at(lhs, i);
    const struct leme_public_value *other = leme_public_get(rhs, key);
    if (other != NULL) {
      struct leme_public_value *merged = NULL;
      if (merge_records(b, value, other, false, &merged) != LEME_PUBLIC_OK)
        return leme_public_builder_status(b);
      value = merged;
    }
    if (leme_public_object_set(b, record, key, value) != LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
  }
  for (size_t i = 0; i < right_count; ++i) {
    const struct leme_public_text key = leme_public_key_at(rhs, i);
    if (leme_public_get(lhs, key) != NULL)
      continue;
    if (leme_public_object_set(b, record, key, leme_public_member_at(rhs, i)) !=
        LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
  }
  *out = record;
  return LEME_PUBLIC_OK;
}

enum leme_public_status leme_config_public_merge(
    struct leme_public_builder *b, const struct leme_public_value *lhs,
    const struct leme_public_value *rhs, struct leme_public_value **out) {
  if (out == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  *out = NULL;
  struct leme_public_value *record = NULL;
  if (merge_records(b, lhs, rhs, true, &record) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  const enum leme_public_status status = leme_public_settings_validate(record);
  if (status != LEME_PUBLIC_OK)
    return leme_public_fail(b, status);
  *out = record;
  return LEME_PUBLIC_OK;
}

enum leme_public_status
leme_config_public_count(struct leme_public_builder *b,
                         struct leme_public_value *record,
                         struct leme_public_text key, size_t count) {
  if ((uintmax_t)count > (uintmax_t)LEME_PUBLIC_SAFE_INTEGER)
    return leme_public_fail(b, LEME_PUBLIC_LIMIT);
  return leme_public_put_int(b, record, key, (int64_t)count);
}
