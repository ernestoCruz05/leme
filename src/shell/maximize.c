#include "shell/view.h"

#include "core/server.h"
#include "desktop/desktop.h"
#include "desktop/strip.h"
#include "input/input.h"
#include "output/output.h"
#include "protocols/capture.h"
#include "protocols/publication.h"
#include "protocols/session.h"
#include "public/server.h"
#include "render/render.h"
#include "shell/layer.h"
#include "shell/container.h"
#include "shell/policy.h"
#include "workspace/tag.h"

#include <wlr/xwayland/xwayland.h>

bool leme_view_is_placed(const struct leme_view *view) {
  return view != NULL && view->placement.kind != LEME_VIEW_PLACEMENT_NORMAL;
}

bool leme_view_is_maximized(const struct leme_view *view) {
  return view != NULL && view->placement.kind == LEME_VIEW_PLACEMENT_MAXIMIZED;
}

static bool leme_placement_owned(const struct leme_view *view) {
  if (view == NULL || view->server == NULL || !view->mapped ||
      view->unmanaged || view->detached ||
      leme_view_geometry_owner(view) != LEME_VIEW_GEOMETRY_FLOATING) {
    return false;
  }
  const struct leme_tag *tag = leme_ownership_tag(view);
  return tag != NULL && tag->layout.kind == LEME_LAYOUT_DESKTOP &&
         leme_view_output(view) != NULL;
}

struct leme_box leme_placement_area(const struct leme_tag *tag,
                                    const struct leme_output *output) {
  if (output == NULL) {
    return (struct leme_box){0};
  }
  if (tag == NULL || tag->owner == NULL || output != tag->owner->output ||
      (tag != NULL && !output->tags.focused_is_candidate &&
       output->tags.focused_id == tag->id)) {
    return leme_desk_strip_content_box(output);
  }
  return output->usable_box;
}

static bool leme_placement_fits(const struct leme_view *view,
                                struct leme_box content) {
  int min_width = 0;
  int min_height = 0;
  int max_width = 0;
  int max_height = 0;
  if (view->kind == LEME_VIEW_XDG && view->xdg_toplevel != NULL) {
    const struct wlr_xdg_toplevel_state *state = &view->xdg_toplevel->current;
    min_width = state->min_width;
    min_height = state->min_height;
    max_width = state->max_width;
    max_height = state->max_height;
  } else if (view->kind == LEME_VIEW_XWAYLAND &&
             view->xwayland_surface != NULL &&
             view->xwayland_surface->size_hints != NULL) {
    const xcb_size_hints_t *hints = view->xwayland_surface->size_hints;
    const uint32_t min_flag = XCB_ICCCM_SIZE_HINT_P_MIN_SIZE;
    const uint32_t max_flag = XCB_ICCCM_SIZE_HINT_P_MAX_SIZE;
    if ((hints->flags & min_flag) != 0) {
      min_width = hints->min_width;
      min_height = hints->min_height;
    }
    if ((hints->flags & max_flag) != 0) {
      max_width = hints->max_width;
      max_height = hints->max_height;
    }
  }
  return !leme_view_policy_fixed_size(min_width, min_height, max_width,
                                      max_height) &&
         content.width >= min_width && content.height >= min_height &&
         (max_width <= 0 || content.width <= max_width) &&
         (max_height <= 0 || content.height <= max_height);
}

bool leme_view_placement_box(const struct leme_view *view,
                             const struct leme_output *output,
                             enum leme_view_placement placement,
                             struct leme_box *box) {
  if (!leme_placement_owned(view) || view->fullscreen || output == NULL ||
      box == NULL ||
      (output != leme_view_output(view) && !leme_desk_output_active(output))) {
    return false;
  }
  const struct leme_box area = leme_placement_box(
      leme_placement_area(leme_ownership_tag(view), output), placement);
  if (area.width < 1 || area.height < 1) {
    return false;
  }
  const struct leme_box content = leme_render_view_content_box(view, area);
  if (content.width <= 0 || content.height <= 0 ||
      !leme_placement_fits(view, content)) {
    return false;
  }
  *box = area;
  return true;
}

bool leme_view_can_place(const struct leme_view *view,
                         enum leme_view_placement placement) {
  struct leme_box box = {0};
  return leme_view_placement_box(view, leme_view_output(view), placement, &box);
}

bool leme_view_can_maximize(const struct leme_view *view) {
  if (view != NULL && view->container != NULL) {
    return leme_container_can_place(view->container,
                                    LEME_VIEW_PLACEMENT_MAXIMIZED);
  }
  return leme_view_can_place(view, LEME_VIEW_PLACEMENT_MAXIMIZED);
}

struct leme_box leme_view_normal_box(const struct leme_view *view) {
  if (view == NULL) {
    return (struct leme_box){0};
  }
  if (!leme_view_is_placed(view) || !view->placement.restore_valid) {
    return view->fullscreen ? view->saved_box : view->box;
  }
  return leme_render_view_frame_box_for_state(view, view->placement.restore_box,
                                              false);
}

static void leme_placement_invalidate(struct leme_view *view) {
  leme_view_sync_wm_state(view);
  leme_public_server_invalidate(view->server);
  leme_publication_invalidate(view->server);
}

static void leme_placement_apply(struct leme_view *view, struct leme_box box,
                                 bool animate) {
  const bool resized =
      view->box.width != box.width || view->box.height != box.height;
  if (animate && (resized || view->box.x != box.x || view->box.y != box.y)) {
    leme_render_view_move(view, view->box, box);
  } else if (!animate) {
    leme_render_view_finish_move(view);
  }
  view->box = box;
  if (resized && view->kind == LEME_VIEW_XDG) {
    leme_view_configure(view, leme_render_view_content_box(view, box));
  }
  leme_render_view_set_box(view, box);
}

bool leme_view_unplace_for_resize(struct leme_view *view) {
  if (!leme_placement_owned(view) || view->fullscreen ||
      leme_view_is_minimized(view) || leme_view_is_maximized(view)) {
    return false;
  }
  if (!leme_view_is_placed(view)) {
    return true;
  }
  leme_render_view_finish_animation(view);
  leme_placement_forget(&view->placement);
  leme_placement_apply(view, view->box, false);
  leme_placement_invalidate(view);
  leme_capture_sync(view->server);
  return true;
}

static void leme_placement_restore(struct leme_view *view, bool animate) {
  if (!leme_view_is_placed(view)) {
    return;
  }
  struct leme_box normal = leme_view_normal_box(view);
  const struct leme_output *output = leme_view_output(view);
  if (output != NULL) {
    normal = leme_view_policy_clamp_box(normal, output->usable_box);
  }
  leme_placement_forget(&view->placement);
  if (view->fullscreen) {
    view->saved_box = normal;
  } else if (view->mapped) {
    leme_placement_apply(view, normal, animate);
  }
  leme_placement_invalidate(view);
}

void leme_view_restore_placement(struct leme_view *view) {
  leme_placement_restore(
      view, view != NULL && !leme_input_pointer_grab_active(view->server));
}

void leme_view_reconcile_placement(struct leme_server *server) {
  if (server == NULL || server->views.next == NULL) {
    return;
  }
  struct leme_view *view = NULL;
  wl_list_for_each(view, &server->views, link) {
    if (!leme_view_is_placed(view) ||
        (view->mapped && leme_ownership_kind(view) == LEME_VIEW_OWNER_NONE)) {
      continue;
    }
    struct leme_box area = {0};
    if (!leme_placement_owned(view) ||
        (!view->fullscreen &&
         !leme_view_placement_box(view, leme_view_output(view),
                                  view->placement.kind, &area))) {
      leme_placement_restore(view, false);
      continue;
    }
    if (view->fullscreen) {
      continue;
    }
    if (view->box.x != area.x || view->box.y != area.y ||
        view->box.width != area.width || view->box.height != area.height) {
      leme_placement_apply(view, area, false);
    }
  }
  leme_container_reconcile_geometry(server);
  leme_capture_sync(server);
}

bool leme_view_set_placement(struct leme_view *view,
                             enum leme_view_placement placement) {
  if (view == NULL || view->server == NULL || !view->mapped ||
      view->unmanaged || view->fullscreen ||
      leme_view_geometry_owner(view) == LEME_VIEW_GEOMETRY_CONTAINER) {
    return false;
  }
  struct leme_server *server = view->server;
  if (leme_session_locked(server) || leme_layer_keyboard_is_exclusive(server) ||
      leme_input_interacting(server)) {
    return false;
  }
  struct leme_box area = {0};
  if (placement != LEME_VIEW_PLACEMENT_NORMAL &&
      !leme_view_placement_box(view, leme_view_output(view), placement,
                               &area)) {
    return false;
  }
  if (view->placement.kind == placement) {
    leme_view_sync_wm_state(view);
    return true;
  }
  struct leme_placement_state state = view->placement;
  if (!leme_placement_set(&state, placement,
                          leme_render_view_content_box(view, view->box))) {
    return false;
  }
  if (placement != LEME_VIEW_PLACEMENT_NORMAL) {
    if (!leme_view_is_placed(view)) {
      leme_view_remember_desktop_box(view, view->box);
    }
    view->placement = state;
    leme_placement_apply(view, area, true);
    leme_placement_invalidate(view);
  } else {
    leme_view_restore_placement(view);
  }
  leme_capture_sync(server);
  return true;
}

bool leme_view_set_maximized(struct leme_view *view, bool maximized) {
  if (view != NULL && view->container != NULL) {
    return leme_container_set_placement(
        view->container,
        maximized ? LEME_VIEW_PLACEMENT_MAXIMIZED : LEME_VIEW_PLACEMENT_NORMAL);
  }
  return leme_view_set_placement(view, maximized ? LEME_VIEW_PLACEMENT_MAXIMIZED
                                                 : LEME_VIEW_PLACEMENT_NORMAL);
}
