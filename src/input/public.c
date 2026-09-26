#include "input/public.h"
#include "config/config.h"
#include "core/server.h"
#include "protocols/session.h"
#include "public/model.h"
#include "public/value-internal.h"

#include <stdalign.h>
#include <stdlib.h>
#include <string.h>

struct input_entry {
  uint64_t serial;
  struct leme_public_value *value;
};
struct input_capture {
  struct leme_public_builder *builder;
  const struct leme_server *server;
  struct input_entry *entries;
  size_t count, capacity;
};

static enum leme_public_status cstr_value(struct leme_public_builder *b,
                                          const char *text,
                                          struct leme_public_value **out) {
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

static enum leme_public_status layout_label(struct leme_public_builder *b,
                                            const char *name,
                                            const char *variant,
                                            struct leme_public_value **out) {
  if (name == NULL || variant == NULL || variant[0] == '\0')
    return cstr_value(b, name, out);
  if (leme_public_mutable(b) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  const size_t name_length = strnlen(name, b->maximum);
  const size_t variant_length = strnlen(variant, b->maximum);
  if (name_length == b->maximum || variant_length == b->maximum ||
      name_length > SIZE_MAX - variant_length ||
      name_length + variant_length > SIZE_MAX - 2)
    return leme_public_fail(b, LEME_PUBLIC_LIMIT);
  const size_t length = name_length + variant_length + 2;
  char *label = leme_public_allocate(b, length, sizeof(*label), alignof(char));
  if (label == NULL)
    return leme_public_builder_status(b);
  memcpy(label, name, name_length);
  label[name_length] = '(';
  memcpy(label + name_length + 1, variant, variant_length);
  label[length - 1] = ')';
  return leme_public_string(b, (struct leme_public_text){label, length}, true,
                            out);
}

enum leme_public_status
leme_input_public_layout(struct leme_public_builder *b,
                         const struct leme_config *config, const char *active,
                         const char *variant, struct leme_public_value **out) {
  if (out == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  *out = NULL;
  const size_t count = config == NULL ? 0 : config->keyboard_layout_count;
  if (count != 0 && config->keyboard_layouts == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  struct leme_public_value *record = NULL, *available = NULL, *current = NULL;
  if (leme_public_object(b, 2, &record) != LEME_PUBLIC_OK ||
      leme_public_array(b, count, &available) != LEME_PUBLIC_OK ||
      layout_label(b, active, variant, &current) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  for (size_t i = 0; i < count; ++i) {
    struct leme_public_value *label = NULL;
    if (config->keyboard_layouts[i].name == NULL)
      return leme_public_fail(b, LEME_PUBLIC_INVALID);
    if (layout_label(b, config->keyboard_layouts[i].name,
                     config->keyboard_layouts[i].variant,
                     &label) != LEME_PUBLIC_OK ||
        leme_public_array_set(b, available, i, label) != LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
  }
  if (leme_public_object_set(b, record, LEME_PUBLIC_TEXT("active"), current) !=
          LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("available"),
                             available) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  *out = record;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status
input_capabilities(struct leme_public_builder *b,
                   const struct leme_public_input_info *info,
                   struct leme_public_value **out) {
  const bool adaptive = !info->keyboard && info->adaptive;
  const bool flat = !info->keyboard && info->flat;
  struct leme_public_value *record = NULL, *profiles = NULL;
  if (leme_public_object(b, 6, &record) != LEME_PUBLIC_OK ||
      leme_public_array(b, (size_t)adaptive + (size_t)flat, &profiles) !=
          LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  size_t index = 0;
  if (adaptive) {
    struct leme_public_value *name = NULL;
    if (cstr_value(b, "adaptive", &name) != LEME_PUBLIC_OK ||
        leme_public_array_set(b, profiles, index++, name) != LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
  }
  if (flat) {
    struct leme_public_value *name = NULL;
    if (cstr_value(b, "flat", &name) != LEME_PUBLIC_OK ||
        leme_public_array_set(b, profiles, index, name) != LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
  }
  if (leme_public_object_set(b, record, LEME_PUBLIC_TEXT("accel_profiles"),
                             profiles) != LEME_PUBLIC_OK ||
      leme_public_put_bool(b, record, LEME_PUBLIC_TEXT("accel_speed"),
                           !info->keyboard && info->has_accel) !=
          LEME_PUBLIC_OK ||
      leme_public_put_bool(b, record, LEME_PUBLIC_TEXT("natural_scroll"),
                           !info->keyboard && info->has_natural_scroll) !=
          LEME_PUBLIC_OK ||
      leme_public_put_bool(b, record, LEME_PUBLIC_TEXT("left_handed"),
                           !info->keyboard && info->has_left_handed) !=
          LEME_PUBLIC_OK ||
      leme_public_put_bool(b, record, LEME_PUBLIC_TEXT("tap"),
                           !info->keyboard && info->has_tap) !=
          LEME_PUBLIC_OK ||
      leme_public_put_bool(b, record, LEME_PUBLIC_TEXT("keyboard_layout"),
                           info->keyboard) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  *out = record;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status optional_bool(struct leme_public_builder *b,
                                             struct leme_public_value *record,
                                             struct leme_public_text key,
                                             bool available, bool value) {
  return available ? leme_public_put_bool(b, record, key, value)
                   : leme_public_put_null(b, record, key);
}

static enum leme_public_status
input_settings(struct leme_public_builder *b, const struct leme_server *server,
               const struct leme_public_input_info *info,
               struct leme_public_value **out) {
  const char *profile = NULL;
  if (!info->keyboard && info->accel_profile != NULL) {
    if ((info->flat && strcmp(info->accel_profile, "flat") == 0) ||
        (info->adaptive && strcmp(info->accel_profile, "adaptive") == 0))
      profile = info->accel_profile;
  }
  struct leme_public_value *record = NULL, *layout = NULL;
  if (leme_public_object(b, 6, &record) != LEME_PUBLIC_OK ||
      (info->keyboard
           ? leme_input_public_layout(b, server->config, info->keyboard_active,
                                      info->keyboard_variant, &layout)
           : leme_public_null(b, &layout)) != LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("accel_profile"),
                           profile) != LEME_PUBLIC_OK ||
      (!info->keyboard && info->has_accel
           ? leme_public_put_number(b, record, LEME_PUBLIC_TEXT("accel_speed"),
                                    info->accel_speed)
           : leme_public_put_null(b, record,
                                  LEME_PUBLIC_TEXT("accel_speed"))) !=
          LEME_PUBLIC_OK ||
      optional_bool(b, record, LEME_PUBLIC_TEXT("natural_scroll"),
                    !info->keyboard && info->has_natural_scroll,
                    info->natural_scroll) != LEME_PUBLIC_OK ||
      optional_bool(b, record, LEME_PUBLIC_TEXT("left_handed"),
                    !info->keyboard && info->has_left_handed,
                    info->left_handed) != LEME_PUBLIC_OK ||
      optional_bool(b, record, LEME_PUBLIC_TEXT("tap"),
                    !info->keyboard && info->has_tap,
                    info->tap) != LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("keyboard_layout"),
                             layout) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  *out = record;
  return LEME_PUBLIC_OK;
}

enum leme_public_status leme_input_public_record(
    struct leme_public_builder *b, const struct leme_server *server,
    const struct leme_public_input_info *info, struct leme_public_value **out) {
  if (out == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  *out = NULL;
  if (server == NULL || info == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  if (leme_session_locked(server))
    return leme_public_fail(b, LEME_PUBLIC_LOCKED);
  if (info->has_ids &&
      (info->vendor > UINT16_MAX || info->product > UINT16_MAX))
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  if (!info->keyboard && info->has_accel &&
      (info->accel_speed < -1 || info->accel_speed > 1))
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  struct leme_public_value *record = NULL, *id = NULL, *capabilities = NULL,
                           *settings = NULL;
  if (leme_public_object(b, 11, &record) != LEME_PUBLIC_OK ||
      leme_public_id_value(b, server->public_model, LEME_PUBLIC_INPUT, info->id,
                           0, &id) != LEME_PUBLIC_OK ||
      input_capabilities(b, info, &capabilities) != LEME_PUBLIC_OK ||
      input_settings(b, server, info, &settings) != LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("type"), "input") !=
          LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("id"), id) !=
          LEME_PUBLIC_OK ||
      leme_public_put_text(b, record, LEME_PUBLIC_TEXT("instance"),
                           leme_public_model_instance(server->public_model),
                           false) != LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("name"), info->name) !=
          LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("kind"),
                           info->keyboard ? "keyboard" : "pointer") !=
          LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("seat"), info->seat) !=
          LEME_PUBLIC_OK ||
      (info->has_ids
           ? leme_public_put_int(b, record, LEME_PUBLIC_TEXT("vendor"),
                                 info->vendor)
           : leme_public_put_null(b, record, LEME_PUBLIC_TEXT("vendor"))) !=
          LEME_PUBLIC_OK ||
      (info->has_ids
           ? leme_public_put_int(b, record, LEME_PUBLIC_TEXT("product"),
                                 info->product)
           : leme_public_put_null(b, record, LEME_PUBLIC_TEXT("product"))) !=
          LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("backend"),
                           info->libinput ? "libinput" : "other") !=
          LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("capabilities"),
                             capabilities) != LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("settings"),
                             settings) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  *out = record;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status
count_input(void *context, const struct leme_public_input_info *info) {
  size_t *count = context;
  if (info == NULL)
    return LEME_PUBLIC_INVALID;
  if (*count == SIZE_MAX)
    return LEME_PUBLIC_LIMIT;
  ++*count;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status
capture_input(void *context, const struct leme_public_input_info *info) {
  struct input_capture *capture = context;
  if (capture->count >= capture->capacity)
    return LEME_PUBLIC_INVALID;
  struct input_entry *entry = &capture->entries[capture->count];
  const enum leme_public_status status = leme_input_public_record(
      capture->builder, capture->server, info, &entry->value);
  if (status != LEME_PUBLIC_OK)
    return status;
  entry->serial = info->id.serial;
  ++capture->count;
  return LEME_PUBLIC_OK;
}

static int compare_inputs(const void *lhs, const void *rhs) {
  const struct input_entry *left = lhs;
  const struct input_entry *right = rhs;
  return left->serial < right->serial   ? -1
         : left->serial > right->serial ? 1
                                        : 0;
}

enum leme_public_status
leme_inputs_public_capture(struct leme_public_builder *b,
                           const struct leme_server *server,
                           struct leme_public_value **out) {
  if (out == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  *out = NULL;
  size_t count = 0;
  enum leme_public_status status =
      leme_input_public_keyboards(server, count_input, &count);
  if (status == LEME_PUBLIC_OK)
    status = leme_input_public_pointers(server, count_input, &count);
  if (status != LEME_PUBLIC_OK)
    return leme_public_fail(b, status);
  struct leme_public_value *array = NULL;
  if (leme_public_array(b, count, &array) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  if (count != 0) {
    struct input_entry *entries = leme_public_allocate(
        b, count, sizeof(*entries), alignof(struct input_entry));
    if (entries == NULL)
      return leme_public_builder_status(b);
    struct input_capture capture = {
        .builder = b, .server = server, .entries = entries, .capacity = count};
    status = leme_input_public_keyboards(server, capture_input, &capture);
    if (status == LEME_PUBLIC_OK)
      status = leme_input_public_pointers(server, capture_input, &capture);
    if (status != LEME_PUBLIC_OK)
      return leme_public_fail(b, status);
    if (capture.count != count)
      return leme_public_fail(b, LEME_PUBLIC_INVALID);
    qsort(entries, count, sizeof(*entries), compare_inputs);
    for (size_t i = 0; i < count; ++i) {
      if (leme_public_array_set(b, array, i, entries[i].value) !=
          LEME_PUBLIC_OK)
        return leme_public_builder_status(b);
    }
  }
  *out = array;
  return LEME_PUBLIC_OK;
}
