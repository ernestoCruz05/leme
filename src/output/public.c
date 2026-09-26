#include "output/public-internal.h"
#include "output/output.h"
#include "core/server.h"
#include "protocols/session.h"
#include "public/model.h"
#include "public/value-internal.h"

#include <stdalign.h>
#include <stdlib.h>

struct mode_entry {
  int32_t width, height, refresh;
  bool preferred;
};

static int compare_outputs(const void *lhs, const void *rhs) {
  const struct leme_public_output_entry *left = lhs;
  const struct leme_public_output_entry *right = rhs;
  return left->serial < right->serial   ? -1
         : left->serial > right->serial ? 1
                                        : 0;
}

enum leme_public_status leme_public_outputs_ordered(
    struct leme_public_builder *b, const struct leme_server *server,
    struct leme_public_output_entry **out, size_t *count) {
  if (out == NULL || count == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  *out = NULL;
  *count = 0;
  if (server == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  if (!leme_public_model_available(server->public_model))
    return leme_public_fail(b, LEME_PUBLIC_UNAVAILABLE);
  if (leme_session_locked(server))
    return leme_public_fail(b, LEME_PUBLIC_LOCKED);
  if (server->outputs.next == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  const struct leme_output *output = NULL;
  size_t length = 0;
  wl_list_for_each(output, &server->outputs, link) {
    if (output->public_id.serial == 0 || output->wlr_output == NULL)
      return leme_public_fail(b, LEME_PUBLIC_INVALID);
    if (length == SIZE_MAX)
      return leme_public_fail(b, LEME_PUBLIC_LIMIT);
    ++length;
  }
  if (length == 0)
    return LEME_PUBLIC_OK;
  struct leme_public_output_entry *entries = leme_public_allocate(
      b, length, sizeof(*entries), alignof(struct leme_public_output_entry));
  if (entries == NULL)
    return leme_public_builder_status(b);
  size_t index = 0;
  wl_list_for_each(output, &server->outputs, link) {
    entries[index++] = (struct leme_public_output_entry){
        .output = output, .serial = output->public_id.serial};
  }
  qsort(entries, length, sizeof(*entries), compare_outputs);
  *out = entries;
  *count = length;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status box_value(struct leme_public_builder *b,
                                         struct leme_box box,
                                         struct leme_public_value **out) {
  struct leme_public_value *record = NULL;
  if (leme_public_object(b, 4, &record) != LEME_PUBLIC_OK ||
      leme_public_put_int(b, record, LEME_PUBLIC_TEXT("x"), box.x) !=
          LEME_PUBLIC_OK ||
      leme_public_put_int(b, record, LEME_PUBLIC_TEXT("y"), box.y) !=
          LEME_PUBLIC_OK ||
      leme_public_put_int(b, record, LEME_PUBLIC_TEXT("width"), box.width) !=
          LEME_PUBLIC_OK ||
      leme_public_put_int(b, record, LEME_PUBLIC_TEXT("height"), box.height) !=
          LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  *out = record;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status mode_value(struct leme_public_builder *b,
                                          struct mode_entry mode,
                                          bool available,
                                          struct leme_public_value **out) {
  if (!available && (mode.width <= 0 || mode.height <= 0))
    return leme_public_null(b, out);
  struct leme_public_value *record = NULL;
  if (leme_public_object(b, available ? 4u : 3u, &record) != LEME_PUBLIC_OK ||
      leme_public_put_int(b, record, LEME_PUBLIC_TEXT("width"), mode.width) !=
          LEME_PUBLIC_OK ||
      leme_public_put_int(b, record, LEME_PUBLIC_TEXT("height"), mode.height) !=
          LEME_PUBLIC_OK ||
      leme_public_put_int(b, record, LEME_PUBLIC_TEXT("refresh_mhz"),
                          mode.refresh) != LEME_PUBLIC_OK ||
      (available &&
       leme_public_put_bool(b, record, LEME_PUBLIC_TEXT("preferred"),
                            mode.preferred) != LEME_PUBLIC_OK))
    return leme_public_builder_status(b);
  *out = record;
  return LEME_PUBLIC_OK;
}

static int compare_modes(const void *lhs, const void *rhs) {
  const struct mode_entry *left = lhs;
  const struct mode_entry *right = rhs;
  if (left->width != right->width)
    return left->width < right->width ? -1 : 1;
  if (left->height != right->height)
    return left->height < right->height ? -1 : 1;
  if (left->refresh != right->refresh)
    return left->refresh < right->refresh ? -1 : 1;
  return left->preferred == right->preferred ? 0 : left->preferred ? 1 : -1;
}

static enum leme_public_status modes_value(struct leme_public_builder *b,
                                           const struct wlr_output *output,
                                           struct leme_public_value **out) {
  size_t count = 0;
  const struct wlr_output_mode *mode = NULL;
  wl_list_for_each(mode, &output->modes, link) {
    if (count == SIZE_MAX)
      return leme_public_fail(b, LEME_PUBLIC_LIMIT);
    ++count;
  }
  struct leme_public_value *array = NULL;
  if (leme_public_array(b, count, &array) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  if (count != 0) {
    struct mode_entry *entries = leme_public_allocate(
        b, count, sizeof(*entries), alignof(struct mode_entry));
    if (entries == NULL)
      return leme_public_builder_status(b);
    size_t index = 0;
    wl_list_for_each(mode, &output->modes, link) {
      entries[index++] = (struct mode_entry){.width = mode->width,
                                             .height = mode->height,
                                             .refresh = mode->refresh,
                                             .preferred = mode->preferred};
    }
    qsort(entries, count, sizeof(*entries), compare_modes);
    for (size_t i = 0; i < count; ++i) {
      struct leme_public_value *item = NULL;
      if (mode_value(b, entries[i], true, &item) != LEME_PUBLIC_OK ||
          leme_public_array_set(b, array, i, item) != LEME_PUBLIC_OK)
        return leme_public_builder_status(b);
    }
  }
  *out = array;
  return LEME_PUBLIC_OK;
}

static const char *transform_name(enum wl_output_transform transform) {
  switch (transform) {
  case WL_OUTPUT_TRANSFORM_NORMAL:
    return "normal";
  case WL_OUTPUT_TRANSFORM_90:
    return "90";
  case WL_OUTPUT_TRANSFORM_180:
    return "180";
  case WL_OUTPUT_TRANSFORM_270:
    return "270";
  case WL_OUTPUT_TRANSFORM_FLIPPED:
    return "flipped";
  case WL_OUTPUT_TRANSFORM_FLIPPED_90:
    return "flipped-90";
  case WL_OUTPUT_TRANSFORM_FLIPPED_180:
    return "flipped-180";
  case WL_OUTPUT_TRANSFORM_FLIPPED_270:
    return "flipped-270";
  }
  return NULL;
}

static enum leme_public_status output_value(struct leme_public_builder *b,
                                            const struct leme_output *output,
                                            struct leme_public_value **out) {
  const struct leme_server *server = output->server;
  const struct wlr_output *device = output->wlr_output;
  const char *transform = transform_name(device->transform);
  if (transform == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  struct leme_public_value *record = NULL, *id = NULL, *geometry = NULL,
                           *usable = NULL;
  struct leme_public_value *current = NULL, *modes = NULL, *tag = NULL;
  const bool in_layout =
      device->enabled && server->output_layout != NULL &&
      wlr_output_layout_get(server->output_layout, output->wlr_output) != NULL;
  if ((in_layout ? box_value(b, leme_output_full_box(output), &geometry)
                 : leme_public_null(b, &geometry)) != LEME_PUBLIC_OK ||
      (in_layout ? box_value(b, leme_output_usable_box(output), &usable)
                 : leme_public_null(b, &usable)) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  struct mode_entry mode = {.width = device->width,
                            .height = device->height,
                            .refresh = device->refresh};
  if ((mode.width <= 0 || mode.height <= 0) && device->current_mode != NULL) {
    mode = (struct mode_entry){.width = device->current_mode->width,
                               .height = device->current_mode->height,
                               .refresh = device->current_mode->refresh};
  }
  const uint16_t active = output->tags.focused_id;
  if ((active == 0 || active > output->tags.max_tags
           ? leme_public_null(b, &tag)
           : leme_public_ref_value(b, server->public_model, LEME_PUBLIC_TAG,
                                   output->public_id, active, &tag)) !=
          LEME_PUBLIC_OK ||
      leme_public_object(b, 18, &record) != LEME_PUBLIC_OK ||
      leme_public_id_value(b, server->public_model, LEME_PUBLIC_OUTPUT,
                           output->public_id, 0, &id) != LEME_PUBLIC_OK ||
      mode_value(b, mode, false, &current) != LEME_PUBLIC_OK ||
      modes_value(b, device, &modes) != LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("type"), "output") !=
          LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("id"), id) !=
          LEME_PUBLIC_OK ||
      leme_public_put_text(b, record, LEME_PUBLIC_TEXT("instance"),
                           leme_public_model_instance(server->public_model),
                           false) != LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("name"), device->name) !=
          LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("description"),
                           device->description) != LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("make"), device->make) !=
          LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("model"),
                           device->model) != LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("serial"),
                           device->serial) != LEME_PUBLIC_OK ||
      leme_public_put_bool(b, record, LEME_PUBLIC_TEXT("enabled"),
                           device->enabled) != LEME_PUBLIC_OK ||
      leme_public_put_bool(b, record, LEME_PUBLIC_TEXT("power_on"),
                           output->power_on) != LEME_PUBLIC_OK ||
      leme_public_put_bool(b, record, LEME_PUBLIC_TEXT("focused"),
                           server->focused_output == output) !=
          LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("geometry"),
                             geometry) != LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("usable_geometry"),
                             usable) != LEME_PUBLIC_OK ||
      leme_public_put_number(b, record, LEME_PUBLIC_TEXT("scale"),
                             (double)device->scale) != LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("transform"),
                           transform) != LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("current_mode"),
                             current) != LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("modes"), modes) !=
          LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("active_tag"), tag) !=
          LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  *out = record;
  return LEME_PUBLIC_OK;
}

enum leme_public_status
leme_outputs_public_capture(struct leme_public_builder *b,
                            const struct leme_server *server,
                            struct leme_public_value **out) {
  if (out == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  *out = NULL;
  struct leme_public_output_entry *entries = NULL;
  size_t count = 0;
  if (leme_public_outputs_ordered(b, server, &entries, &count) !=
      LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  struct leme_public_value *array = NULL;
  if (leme_public_array(b, count, &array) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  for (size_t i = 0; i < count; ++i) {
    struct leme_public_value *record = NULL;
    if (output_value(b, entries[i].output, &record) != LEME_PUBLIC_OK ||
        leme_public_array_set(b, array, i, record) != LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
    entries[i].output = NULL;
  }
  *out = array;
  return LEME_PUBLIC_OK;
}
