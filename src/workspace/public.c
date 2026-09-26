#include "workspace/public.h"
#include "core/server.h"
#include "output/output.h"
#include "output/public-internal.h"
#include "protocols/workspace.h"
#include "public/model.h"
#include "public/value-internal.h"
#include "shell/view.h"

#include <stdio.h>

const char *leme_layout_public_name(enum leme_layout_kind kind) {
  switch (kind) {
  case LEME_LAYOUT_DWINDLE:
    return "dwindle";
  case LEME_LAYOUT_MASTER_STACK:
    return "master_stack";
  case LEME_LAYOUT_ACCORDION:
    return "accordion";
  }
  return NULL;
}

static enum leme_public_status tag_value(struct leme_public_builder *b,
                                         const struct leme_output *output,
                                         uint16_t number, bool navigable,
                                         struct leme_public_value **out) {
  const struct leme_server *server = output->server;
  const struct leme_tag *tag = output->tags.table[number];
  struct leme_tag_settings settings = {.layout = LEME_LAYOUT_DWINDLE};
  if (server->config != NULL)
    leme_config_tag_settings(server->config, number, &settings);
  const char *layout =
      leme_layout_public_name(tag == NULL ? settings.layout : tag->layout.kind);
  if (layout == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  int64_t view_count = 0;
  if (tag != NULL) {
    const struct leme_view *view = NULL;
    wl_list_for_each(view, &tag->views, tag_link) {
      if (view->mapped && !view->unmanaged && view->public_meta.ever_mapped &&
          leme_ownership_tag(view) == tag) {
        if (view_count == LEME_PUBLIC_SAFE_INTEGER)
          return leme_public_fail(b, LEME_PUBLIC_LIMIT);
        ++view_count;
      }
    }
  }
  char name[6] = {0};
  const int written = snprintf(name, sizeof(name), "%u", (unsigned)number);
  if (written < 0 || (size_t)written >= sizeof(name))
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  struct leme_public_value *record = NULL, *id = NULL, *reference = NULL;
  if (leme_public_object(b, 13, &record) != LEME_PUBLIC_OK ||
      leme_public_id_value(b, server->public_model, LEME_PUBLIC_TAG,
                           output->public_id, number, &id) != LEME_PUBLIC_OK ||
      leme_public_ref_value(b, server->public_model, LEME_PUBLIC_OUTPUT,
                            output->public_id, 0,
                            &reference) != LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("type"), "tag") !=
          LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("id"), id) !=
          LEME_PUBLIC_OK ||
      leme_public_put_text(b, record, LEME_PUBLIC_TEXT("instance"),
                           leme_public_model_instance(server->public_model),
                           false) != LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("output"),
                             reference) != LEME_PUBLIC_OK ||
      leme_public_put_int(b, record, LEME_PUBLIC_TEXT("number"), number) !=
          LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("name"), name) !=
          LEME_PUBLIC_OK ||
      leme_public_put_bool(b, record, LEME_PUBLIC_TEXT("active"),
                           output->tags.focused_id == number) !=
          LEME_PUBLIC_OK ||
      leme_public_put_bool(b, record, LEME_PUBLIC_TEXT("pinned"),
                           number <= output->tags.initial_tags) !=
          LEME_PUBLIC_OK ||
      leme_public_put_bool(b, record, LEME_PUBLIC_TEXT("materialized"),
                           tag != NULL) != LEME_PUBLIC_OK ||
      leme_public_put_bool(b, record, LEME_PUBLIC_TEXT("navigable"),
                           navigable) != LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("layout"), layout) !=
          LEME_PUBLIC_OK ||
      leme_public_put_int(b, record, LEME_PUBLIC_TEXT("view_count"),
                          view_count) != LEME_PUBLIC_OK ||
      leme_public_put_bool(b, record, LEME_PUBLIC_TEXT("urgent"),
                           leme_workspace_public_urgent(
                               server, output, number)) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  *out = record;
  return LEME_PUBLIC_OK;
}

enum leme_public_status
leme_tags_public_capture(struct leme_public_builder *b,
                         const struct leme_server *server,
                         struct leme_public_value **out) {
  if (out == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  *out = NULL;
  struct leme_public_output_entry *outputs = NULL;
  size_t count = 0;
  if (leme_public_outputs_ordered(b, server, &outputs, &count) !=
      LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  size_t total = 0;
  for (size_t i = 0; i < count; ++i) {
    const struct leme_tags *tags = &outputs[i].output->tags;
    if (tags->table == NULL || tags->max_tags == 0 ||
        tags->max_tags >= LEME_TAGS_RING_MAX)
      return leme_public_fail(b, LEME_PUBLIC_INVALID);
    if (tags->max_tags > SIZE_MAX - total)
      return leme_public_fail(b, LEME_PUBLIC_LIMIT);
    total += tags->max_tags;
  }
  struct leme_public_value *array = NULL;
  if (leme_public_array(b, total, &array) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  size_t index = 0;
  for (size_t i = 0; i < count; ++i) {
    const struct leme_tags *tags = &outputs[i].output->tags;
    uint16_t navigation[LEME_TAGS_RING_MAX] = {0};
    const size_t navigation_count =
        leme_tags_navigable(tags, navigation, LEME_TAGS_RING_MAX);
    if (navigation_count > LEME_TAGS_RING_MAX)
      return leme_public_fail(b, LEME_PUBLIC_LIMIT);
    for (size_t slot = 1; slot <= tags->max_tags; ++slot) {
      if (slot > UINT16_MAX)
        return leme_public_fail(b, LEME_PUBLIC_LIMIT);
      const uint16_t number = (uint16_t)slot;
      bool navigable = false;
      for (size_t n = 0; n < navigation_count; ++n)
        if (navigation[n] == number)
          navigable = true;
      struct leme_public_value *record = NULL;
      if (tag_value(b, outputs[i].output, number, navigable, &record) !=
              LEME_PUBLIC_OK ||
          leme_public_array_set(b, array, index++, record) != LEME_PUBLIC_OK)
        return leme_public_builder_status(b);
    }
    outputs[i].output = NULL;
  }
  *out = array;
  return LEME_PUBLIC_OK;
}
