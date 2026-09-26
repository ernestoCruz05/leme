#include "shell/public.h"
#include "core/server.h"
#include "output/output.h"
#include "protocols/session.h"
#include "public/model.h"
#include "public/value-internal.h"
#include "shell/view.h"
#include "shell/xwayland.h"

#include <stdalign.h>
#include <stdlib.h>
#include <wlr/xwayland/xwayland.h>

struct view_entry {
  const struct leme_view *view;
  uint64_t serial;
};

static bool published(const struct leme_view *view) {
  return view != NULL && !view->unmanaged && view->public_meta.ever_mapped &&
         view->public_meta.id.serial != 0;
}

static const struct leme_view *parent_view(const struct leme_view *view) {
  const struct leme_view *xparent = leme_xwayland_public_parent(view);
  const struct wlr_xdg_toplevel *native_parent =
      view->kind == LEME_VIEW_XDG && view->xdg_toplevel != NULL
          ? view->xdg_toplevel->parent
          : NULL;
  const struct leme_view *candidate = NULL;
  wl_list_for_each(candidate, &view->server->views, link) {
    if (candidate == view || !published(candidate))
      continue;
    if ((xparent != NULL && candidate == xparent) ||
        (native_parent != NULL && candidate->kind == LEME_VIEW_XDG &&
         candidate->xdg_toplevel == native_parent))
      return candidate;
  }
  return NULL;
}

static bool visible(const struct leme_view *view) {
  if (!leme_ownership_scene_visible(view))
    return false;
  const struct leme_output *output = leme_ownership_effective_output(view);
  if (output == NULL || output->wlr_output == NULL ||
      !output->wlr_output->enabled || !output->power_on)
    return false;
  if (leme_view_is_scratchpad(view))
    return leme_view_is_shown_scratchpad(view);
  const struct leme_tag *tag = leme_ownership_tag(view);
  if (tag == NULL)
    return leme_view_is_sticky(view);
  if (tag->owner == NULL || tag->owner->focused_is_candidate ||
      tag->id != tag->owner->focused_id)
    return false;
  const struct leme_view *candidate = NULL;
  wl_list_for_each(candidate, &tag->views, tag_link) {
    if (candidate->mapped && candidate->fullscreen)
      return candidate == view;
  }
  return true;
}

static enum leme_public_status
put_view_ref(struct leme_public_builder *b, struct leme_public_value *record,
             struct leme_public_text key, const struct leme_public_model *model,
             const struct leme_view *view) {
  if (!published(view))
    return leme_public_put_null(b, record, key);
  struct leme_public_value *reference = NULL;
  const enum leme_public_status status = leme_public_ref_value(
      b, model, LEME_PUBLIC_VIEW, view->public_meta.id, 0, &reference);
  return status != LEME_PUBLIC_OK
             ? status
             : leme_public_object_set(b, record, key, reference);
}

static enum leme_public_status
put_output_ref(struct leme_public_builder *b, struct leme_public_value *record,
               struct leme_public_text key,
               const struct leme_public_model *model,
               const struct leme_output *output, uint16_t tag_number) {
  if (output == NULL)
    return leme_public_put_null(b, record, key);
  struct leme_public_value *reference = NULL;
  const enum leme_public_status status = leme_public_ref_value(
      b, model, tag_number == 0 ? LEME_PUBLIC_OUTPUT : LEME_PUBLIC_TAG,
      output->public_id, tag_number, &reference);
  return status != LEME_PUBLIC_OK
             ? status
             : leme_public_object_set(b, record, key, reference);
}

static enum leme_public_status owner_value(struct leme_public_builder *b,
                                           const struct leme_view *view,
                                           struct leme_public_value **out) {
  const struct leme_public_model *model = view->server->public_model;
  struct leme_public_value *owner = NULL;
  if (leme_public_object(b, 3, &owner) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  const bool assigned =
      view->mapped && leme_ownership_kind(view) != LEME_VIEW_OWNER_NONE;
  const bool sticky = assigned && leme_view_is_sticky(view);
  const bool scratchpad = assigned && leme_view_is_scratchpad(view);
  if (assigned && !sticky && !scratchpad && leme_ownership_tag(view) == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  const char *kind = !assigned    ? "unassigned"
                     : sticky     ? "sticky"
                     : scratchpad ? "scratchpad"
                                  : "tag";
  if (leme_public_put_cstr(b, owner, LEME_PUBLIC_TEXT("kind"), kind) !=
          LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, owner, LEME_PUBLIC_TEXT("scratchpad_name"),
                           scratchpad ? view->scratchpad_name : NULL) !=
          LEME_PUBLIC_OK ||
      put_view_ref(b, owner, LEME_PUBLIC_TEXT("root"), model,
                   sticky ? leme_sticky_group_root(view) : NULL) !=
          LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  *out = owner;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status geometry_value(struct leme_public_builder *b,
                                              const struct leme_view *view,
                                              struct leme_public_value **out) {
  if (!view->mapped || leme_ownership_kind(view) == LEME_VIEW_OWNER_NONE)
    return leme_public_null(b, out);
  struct leme_public_value *box = NULL;
  if (leme_public_object(b, 4, &box) != LEME_PUBLIC_OK ||
      leme_public_put_int(b, box, LEME_PUBLIC_TEXT("x"), view->box.x) !=
          LEME_PUBLIC_OK ||
      leme_public_put_int(b, box, LEME_PUBLIC_TEXT("y"), view->box.y) !=
          LEME_PUBLIC_OK ||
      leme_public_put_int(b, box, LEME_PUBLIC_TEXT("width"), view->box.width) !=
          LEME_PUBLIC_OK ||
      leme_public_put_int(b, box, LEME_PUBLIC_TEXT("height"),
                          view->box.height) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  *out = box;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status put_order(struct leme_public_builder *b,
                                         struct leme_public_value *record,
                                         struct leme_public_text key,
                                         uint64_t order) {
  if (order == 0)
    return leme_public_put_null(b, record, key);
  if (order > (uint64_t)LEME_PUBLIC_SAFE_INTEGER)
    return leme_public_fail(b, LEME_PUBLIC_LIMIT);
  return leme_public_put_int(b, record, key, (int64_t)order);
}

static enum leme_public_status view_value(struct leme_public_builder *b,
                                          const struct leme_view *view,
                                          struct leme_public_value **out) {
  *out = NULL;
  if (view->kind != LEME_VIEW_XDG && view->kind != LEME_VIEW_XWAYLAND)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  const struct leme_public_model *model = view->server->public_model;
  const struct leme_output *output =
      view->mapped ? leme_ownership_effective_output(view) : NULL;
  const struct leme_tag *tag = view->mapped ? leme_ownership_tag(view) : NULL;
  const struct wlr_xwayland_surface *xsurface =
      view->kind == LEME_VIEW_XWAYLAND ? view->xwayland_surface : NULL;
  struct leme_public_value *record = NULL, *id = NULL, *owner = NULL,
                           *geometry = NULL;
  if (leme_public_object(b, 21, &record) != LEME_PUBLIC_OK ||
      leme_public_id_value(b, model, LEME_PUBLIC_VIEW, view->public_meta.id, 0,
                           &id) != LEME_PUBLIC_OK ||
      owner_value(b, view, &owner) != LEME_PUBLIC_OK ||
      geometry_value(b, view, &geometry) != LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("type"), "view") !=
          LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("id"), id) !=
          LEME_PUBLIC_OK ||
      leme_public_put_text(b, record, LEME_PUBLIC_TEXT("instance"),
                           leme_public_model_instance(model),
                           false) != LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("kind"),
                           view->kind == LEME_VIEW_XDG
                               ? "wayland"
                               : "xwayland") != LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("title"),
                           leme_view_title(view)) != LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("app_id"),
                           leme_view_identity(view)) != LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("x11_class"),
                           xsurface == NULL ? NULL : xsurface->class) !=
          LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("x11_instance"),
                           xsurface == NULL ? NULL : xsurface->instance) !=
          LEME_PUBLIC_OK ||
      leme_public_put_bool(b, record, LEME_PUBLIC_TEXT("mapped"),
                           view->mapped) != LEME_PUBLIC_OK ||
      leme_public_put_bool(b, record, LEME_PUBLIC_TEXT("visible"),
                           visible(view)) != LEME_PUBLIC_OK ||
      leme_public_put_bool(b, record, LEME_PUBLIC_TEXT("focused"),
                           view->mapped && view == view->server->focused_view &&
                               view->server->focused_layer == NULL) !=
          LEME_PUBLIC_OK ||
      leme_public_put_bool(b, record, LEME_PUBLIC_TEXT("floating"),
                           view->floating) != LEME_PUBLIC_OK ||
      leme_public_put_bool(b, record, LEME_PUBLIC_TEXT("fullscreen"),
                           view->fullscreen) != LEME_PUBLIC_OK ||
      leme_public_put_bool(b, record, LEME_PUBLIC_TEXT("urgent"),
                           view->public_meta.urgent) != LEME_PUBLIC_OK ||
      put_order(b, record, LEME_PUBLIC_TEXT("last_focused"),
                view->public_meta.last_focused) != LEME_PUBLIC_OK ||
      put_order(b, record, LEME_PUBLIC_TEXT("urgent_since"),
                view->public_meta.urgent_since) != LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("geometry"),
                             geometry) != LEME_PUBLIC_OK ||
      put_output_ref(b, record, LEME_PUBLIC_TEXT("output"), model, output, 0) !=
          LEME_PUBLIC_OK ||
      put_output_ref(b, record, LEME_PUBLIC_TEXT("tag"), model,
                     tag == NULL ? NULL : output,
                     tag == NULL ? 0 : tag->id) != LEME_PUBLIC_OK ||
      put_view_ref(b, record, LEME_PUBLIC_TEXT("parent"), model,
                   parent_view(view)) != LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("owner"), owner) !=
          LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  *out = record;
  return LEME_PUBLIC_OK;
}

static int compare_views(const void *lhs, const void *rhs) {
  const struct view_entry *left = lhs;
  const struct view_entry *right = rhs;
  return left->serial < right->serial   ? -1
         : left->serial > right->serial ? 1
                                        : 0;
}

enum leme_public_status
leme_views_public_capture(struct leme_public_builder *b,
                          const struct leme_server *server,
                          struct leme_public_value **out) {
  if (out == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  *out = NULL;
  if (server == NULL || server->views.next == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  if (!leme_public_model_available(server->public_model))
    return leme_public_fail(b, LEME_PUBLIC_UNAVAILABLE);
  if (leme_session_locked(server))
    return leme_public_fail(b, LEME_PUBLIC_LOCKED);
  size_t count = 0;
  const struct leme_view *view = NULL;
  wl_list_for_each(view, &server->views, link) {
    if (!published(view))
      continue;
    if (count == SIZE_MAX)
      return leme_public_fail(b, LEME_PUBLIC_LIMIT);
    ++count;
  }
  struct leme_public_value *array = NULL;
  if (leme_public_array(b, count, &array) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  struct view_entry *entries = NULL;
  if (count != 0) {
    entries = leme_public_allocate(b, count, sizeof(*entries),
                                   alignof(struct view_entry));
    if (entries == NULL)
      return leme_public_builder_status(b);
    size_t index = 0;
    wl_list_for_each(view, &server->views, link) {
      if (published(view))
        entries[index++] = (struct view_entry){
            .view = view, .serial = view->public_meta.id.serial};
    }
    qsort(entries, count, sizeof(*entries), compare_views);
  }
  for (size_t i = 0; i < count; ++i) {
    struct leme_public_value *record = NULL;
    if (view_value(b, entries[i].view, &record) != LEME_PUBLIC_OK ||
        leme_public_array_set(b, array, i, record) != LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
    entries[i].view = NULL;
  }
  *out = array;
  return LEME_PUBLIC_OK;
}
