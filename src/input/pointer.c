#include "input/input.h"
#include "input/internal.h"
#include "input/public.h"
#include "public/server.h"

#include "config/config.h"
#include "config/live.h"
#include "config/style.h"
#include "core/server.h"
#include "desktop/desktop.h"
#include "desktop/drag.h"
#include "desktop/strip.h"
#include "desktop/buttons.h"
#include "desktop/owner.h"
#include "desktop/decoration.h"
#include "output/output.h"
#include "protocols/data.h"
#include "protocols/desktop.h"
#include "protocols/input.h"
#include "protocols/session.h"
#include "render/animation.h"
#include "render/render.h"
#include "render/workspace_transition.h"
#include "shell/layer.h"
#include "shell/policy.h"
#include "shell/view.h"
#include "shell/container.h"
#include "workspace/tag.h"

#include <libinput.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/backend/libinput.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>

struct leme_pointer {
  struct leme_server *server;
  struct wlr_input_device *device;
  struct leme_public_id public_id;
  struct wl_listener destroy;
  struct wl_list link;
};

enum leme_pointer_grab_mode {
  LEME_POINTER_GRAB_NONE,
  LEME_POINTER_GRAB_FLOAT_MOVE,
  LEME_POINTER_GRAB_FLOAT_RESIZE,
  LEME_POINTER_GRAB_CONTAINER_MOVE,
  LEME_POINTER_GRAB_CONTAINER_RESIZE,
  LEME_POINTER_GRAB_CONTAINER_DETACH,
  LEME_POINTER_GRAB_CONTAINER_SPLIT,
  LEME_POINTER_GRAB_TILED_MOVE,
  LEME_POINTER_GRAB_TILED_RESIZE,
  LEME_POINTER_GRAB_SINK,
};

struct leme_pointer_grab {
  struct leme_server *server;
  struct wlr_seat_pointer_grab seat_grab;
  enum leme_pointer_grab_mode mode;
  struct leme_view *view;
  struct leme_public_id container;
  struct leme_public_id source_container;
  struct leme_public_id source_survivor;
  uint64_t source_generation;
  struct leme_box source_content;
  uint32_t button;
  double cursor_x;
  double cursor_y;
  struct leme_box saved_box;
  enum leme_grab_edge edges;
  enum leme_grab_edge hover_edges;
  struct leme_layout_detach *detach;
  struct leme_layout_resize_drag resize_drag;
  struct leme_view *drop_target;
  enum leme_direction drop_direction;
  enum leme_drop_mode drop_mode;
  bool preview_failed;
  struct leme_output *destination;
  bool drop_empty;
  bool restore_pending;
  bool titlebar_pending;
  bool released;
  bool snap_enabled;
  enum leme_view_placement snap_placement;
  struct leme_render_preview snap_preview;
  struct leme_container_drop container_drop;
};

static void leme_input_process_motion(struct leme_server *server,
                                      uint32_t time_msec,
                                      struct leme_render_hit *result,
                                      bool allow_view_focus);

bool leme_input_pointer_grab_moves_container(
    const struct leme_container *container) {
  if (container == NULL || container->tag == NULL ||
      container->tag->owner == NULL || container->tag->owner->server == NULL) {
    return false;
  }
  const struct leme_pointer_grab *grab =
      container->tag->owner->server->pointer_grab;
  return grab != NULL && grab->mode == LEME_POINTER_GRAB_CONTAINER_MOVE &&
         grab->container.serial == container->id.serial;
}

static uint32_t leme_input_pointer_wlr_edges(enum leme_grab_edge edges);

static bool leme_input_pointer_button_held(const struct wlr_seat *seat,
                                           uint32_t button) {
  size_t index;

  for (index = 0; index < seat->pointer_state.button_count; index++) {
    if (seat->pointer_state.buttons[index].button == button &&
        seat->pointer_state.buttons[index].n_pressed > 0) {
      return true;
    }
  }
  return false;
}

static size_t
leme_input_pointer_button_press_count(const struct wlr_seat *seat) {
  size_t count = 0;
  size_t index;

  for (index = 0; index < seat->pointer_state.button_count; index++) {
    count += seat->pointer_state.buttons[index].n_pressed;
  }
  return count;
}

static int leme_input_pointer_round_delta(double value) {
  if (value >= (double)INT_MAX) {
    return INT_MAX;
  }
  if (value <= (double)INT_MIN) {
    return INT_MIN;
  }
  return (int)llround(value);
}

static int leme_input_pointer_add_saturated(int value, int delta) {
  int64_t result = (int64_t)value + delta;

  if (result > INT_MAX) {
    return INT_MAX;
  }
  if (result < INT_MIN) {
    return INT_MIN;
  }
  return (int)result;
}

static bool leme_input_pointer_edges(uint32_t wlr_edges,
                                     enum leme_grab_edge *edges) {
  const uint32_t allowed =
      WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT;

  if (wlr_edges == WLR_EDGE_NONE || (wlr_edges & ~allowed) != 0 ||
      ((wlr_edges & WLR_EDGE_TOP) != 0 && (wlr_edges & WLR_EDGE_BOTTOM) != 0) ||
      ((wlr_edges & WLR_EDGE_LEFT) != 0 && (wlr_edges & WLR_EDGE_RIGHT) != 0)) {
    return false;
  }
  *edges = LEME_GRAB_EDGE_NONE;
  if ((wlr_edges & WLR_EDGE_TOP) != 0) {
    *edges |= LEME_GRAB_EDGE_TOP;
  }
  if ((wlr_edges & WLR_EDGE_BOTTOM) != 0) {
    *edges |= LEME_GRAB_EDGE_BOTTOM;
  }
  if ((wlr_edges & WLR_EDGE_LEFT) != 0) {
    *edges |= LEME_GRAB_EDGE_LEFT;
  }
  if ((wlr_edges & WLR_EDGE_RIGHT) != 0) {
    *edges |= LEME_GRAB_EDGE_RIGHT;
  }
  return true;
}

static const char *leme_input_pointer_grab_cursor(enum leme_grab_edge edges) {
  if (edges == LEME_GRAB_EDGE_NONE) {
    return "grabbing";
  }
  if ((edges & LEME_GRAB_EDGE_TOP) != 0) {
    if ((edges & LEME_GRAB_EDGE_LEFT) != 0) {
      return "nw-resize";
    }
    if ((edges & LEME_GRAB_EDGE_RIGHT) != 0) {
      return "ne-resize";
    }
    return "n-resize";
  }
  if ((edges & LEME_GRAB_EDGE_BOTTOM) != 0) {
    if ((edges & LEME_GRAB_EDGE_LEFT) != 0) {
      return "sw-resize";
    }
    if ((edges & LEME_GRAB_EDGE_RIGHT) != 0) {
      return "se-resize";
    }
    return "s-resize";
  }
  return (edges & LEME_GRAB_EDGE_LEFT) != 0 ? "w-resize" : "e-resize";
}

static bool
leme_input_pointer_grab_mode_tiled(enum leme_pointer_grab_mode mode) {
  return mode == LEME_POINTER_GRAB_TILED_MOVE ||
         mode == LEME_POINTER_GRAB_TILED_RESIZE;
}

static void
leme_input_pointer_grab_hide_preview(struct leme_pointer_grab *grab) {
  leme_render_preview_hide(grab->server, &grab->snap_preview);
  grab->container_drop = (struct leme_container_drop){0};
  grab->snap_placement = LEME_VIEW_PLACEMENT_NORMAL;
  if (grab->drop_target != NULL) {
    leme_render_view_hide_drop_preview(grab->drop_target);
    grab->drop_target = NULL;
  }
}

static void leme_input_pointer_container_defer(struct leme_pointer_grab *grab,
                                               bool deferred) {
  struct leme_container *container =
      leme_container_by_public_id(grab->server, grab->container);
  if (container != NULL) {
    for (size_t i = 0; i < container->count; i++) {
      struct leme_view *view = container->views[i];
      leme_view_set_configure_deferred(
          view, deferred && (grab->mode != LEME_POINTER_GRAB_CONTAINER_RESIZE ||
                             view->kind == LEME_VIEW_XWAYLAND));
    }
  }
}

static void leme_input_pointer_grab_cleanup(struct leme_pointer_grab *grab) {
  struct leme_server *server = grab->server;

  leme_input_pointer_grab_hide_preview(grab);
  if (grab->detach != NULL) {
    if (grab->view != NULL) {
      leme_view_restore_tiled_drag(grab->view, &grab->detach);
    } else {
      leme_layout_discard_detached(&grab->detach);
    }
  }
  struct leme_view *view = grab->view;

  grab->mode = LEME_POINTER_GRAB_NONE;
  struct leme_container *container =
      leme_container_by_public_id(server, grab->container);
  if (container != NULL) {
    leme_container_arrange(container, false);
  }
  leme_input_pointer_container_defer(grab, false);
  grab->container = (struct leme_public_id){0};
  grab->source_container = (struct leme_public_id){0};
  grab->source_survivor = (struct leme_public_id){0};
  grab->source_generation = 0;
  grab->source_content = (struct leme_box){0};
  leme_view_set_configure_deferred(grab->view, false);
  grab->view = NULL;
  leme_view_follow_committed_size(view);
  leme_desk_decoration_refresh(view);
  grab->button = 0;
  grab->edges = LEME_GRAB_EDGE_NONE;
  grab->resize_drag = (struct leme_layout_resize_drag){0};
  grab->drop_direction = LEME_DIRECTION_LEFT;
  grab->preview_failed = false;
  grab->destination = NULL;
  grab->drop_empty = false;
  grab->restore_pending = false;
  grab->titlebar_pending = false;
  grab->released = false;
  grab->snap_enabled = false;
  leme_desktop_cursor_restore(server);
  if (server->desktop != NULL && server->seat != NULL &&
      server->cursor != NULL && !leme_session_locked(server)) {
    wlr_seat_pointer_clear_focus(server->seat);
    leme_input_process_motion(server, 0, NULL, false);
  }
}

static void
leme_input_pointer_grab_enter(struct wlr_seat_pointer_grab *seat_grab,
                              struct wlr_surface *surface, double sx,
                              double sy) {
  (void)seat_grab;
  (void)surface;
  (void)sx;
  (void)sy;
}

static void
leme_input_pointer_grab_clear_focus(struct wlr_seat_pointer_grab *seat_grab) {
  (void)seat_grab;
}

static bool
leme_input_pointer_grab_cross_allowed(const struct leme_server *server) {
  return server->config == NULL ||
         server->config->output_policy.cross_output_drag;
}

static bool
leme_input_pointer_container_destination(const struct leme_output *output) {
  return leme_desk_output_active(output) &&
         leme_tag_fullscreen_view(
             output->tags.table[output->tags.focused_id]) == NULL;
}

static void
leme_input_pointer_grab_update_destination(struct leme_pointer_grab *grab) {
  struct leme_server *server = grab->server;
  struct leme_output *resolved;
  const struct leme_container *container =
      leme_container_by_public_id(server, grab->container);
  struct leme_output *source = container == NULL
                                   ? leme_view_output(grab->view)
                                   : container->tag->owner->output;
  if (!leme_input_pointer_grab_cross_allowed(server)) {
    grab->destination = source;
    return;
  }
  resolved = leme_output_at(server, server->cursor->x, server->cursor->y);
  if (resolved == NULL || !resolved->wlr_output->enabled) {
    return;
  }
  grab->destination =
      container != NULL && !leme_input_pointer_container_destination(resolved)
          ? source
          : resolved;
}

static bool leme_input_pointer_snap_neighbour(double x, double y, void *data) {
  return leme_output_at(data, x, y) != NULL;
}

static struct leme_view *
leme_input_pointer_container_target(const struct leme_pointer_grab *grab,
                                    const struct leme_render_hit *hit) {
  struct leme_view *view = hit->view != NULL        ? hit->view
                           : hit->container == NULL ? NULL
                                                    : hit->container->focused;
  if (view == NULL ||
      view->public_meta.id.serial == grab->source_survivor.serial ||
      (view->container != NULL &&
       view->container->id.serial == grab->source_container.serial)) {
    return NULL;
  }
  return view;
}

static void
leme_input_pointer_grab_container_drop(struct leme_pointer_grab *grab) {
  struct leme_server *server = grab->server;
  struct leme_render_hit hit = {0};
  leme_render_drop_at(
      server, grab->view,
      grab->snap_preview.rect == NULL ? NULL : &grab->snap_preview.rect->node,
      server->cursor->x, server->cursor->y, &hit);
  struct leme_container_drop drop = {0};
  struct leme_view *target = leme_input_pointer_container_target(grab, &hit);
  if (leme_view_output(target) != grab->destination ||
      !leme_container_prepare_drop(target, grab->view, server->cursor->x,
                                   server->cursor->y, &drop) ||
      !leme_render_preview_show(server, &grab->snap_preview, drop.preview,
                                server->cursor->x, server->cursor->y)) {
    leme_input_pointer_grab_hide_preview(grab);
    return;
  }
  grab->container_drop = drop;
  grab->snap_placement = LEME_VIEW_PLACEMENT_NORMAL;
}

static bool leme_input_pointer_group_held(const struct leme_server *server) {
  return server->config != NULL &&
         (leme_input_modifiers(server) &
          server->config->desktop_container_modifier) != 0;
}

static void leme_input_pointer_grab_snap(struct leme_pointer_grab *grab) {
  const struct leme_output *output = grab->destination;
  if (!grab->snap_enabled || output == NULL) {
    leme_input_pointer_grab_hide_preview(grab);
    return;
  }
  struct leme_server *server = grab->server;
  struct leme_container *container =
      leme_container_by_public_id(server, grab->container);
  if (container == NULL && leme_input_pointer_group_held(server)) {
    leme_input_pointer_grab_container_drop(grab);
    return;
  }
  const enum leme_view_placement placement = leme_placement_at(
      leme_output_full_box(output), server->cursor->x, server->cursor->y,
      leme_input_pointer_snap_neighbour, server);
  struct leme_box box = {0};
  if (container != NULL) {
    if (!leme_container_can_place(container, placement)) {
      leme_input_pointer_grab_hide_preview(grab);
      return;
    }
    box = leme_placement_box(leme_placement_area(container->tag, output),
                             placement);
  } else if (!leme_view_placement_box(grab->view, output, placement, &box)) {
    leme_input_pointer_grab_hide_preview(grab);
    return;
  }
  grab->container_drop = (struct leme_container_drop){0};
  if (!leme_render_preview_show(server, &grab->snap_preview, box,
                                server->cursor->x, server->cursor->y)) {
    leme_input_pointer_grab_hide_preview(grab);
    if (!grab->preview_failed) {
      wlr_log(WLR_ERROR, "%s", "leme: failed to render snap preview");
      grab->preview_failed = true;
    }
    return;
  }
  grab->snap_placement = placement;
}

static void leme_input_pointer_grab_restore(struct leme_pointer_grab *grab) {
  struct leme_view *view = grab->view;
  struct leme_server *server = grab->server;
  const int border = server->config == NULL ? 0 : server->config->border_width;
  const int height = leme_desk_style_titlebar_height(server->config);
  const int button = leme_desk_style_button_width(server->config);
  struct leme_container *container =
      leme_container_by_public_id(server, grab->container);
  struct leme_desk_decoration_boxes before = {0};
  const bool titlebar =
      (container != NULL ||
       leme_desk_decoration_height(view, grab->saved_box) > 0) &&
      leme_desk_decoration_geometry(grab->saved_box, border, height, button,
                                    &before) &&
      grab->cursor_y >= before.titlebar.y &&
      grab->cursor_y < (double)before.titlebar.y + before.titlebar.height;
  if (container != NULL) {
    leme_container_restore_placement(container);
  } else {
    leme_view_restore_placement(view);
  }
  const struct leme_box box = container != NULL ? container->box : view->box;
  struct leme_desk_decoration_boxes after = {0};
  const struct leme_box restored_anchor =
      titlebar &&
              leme_desk_decoration_geometry(box, border, height, button, &after)
          ? after.titlebar
          : box;
  const struct leme_placement_anchor anchor = {
      .original = titlebar ? before.titlebar : grab->saved_box,
      .restored = restored_anchor,
      .frame = box,
      .start_x = grab->cursor_x,
      .start_y = grab->cursor_y,
      .cursor_x = server->cursor->x,
      .cursor_y = server->cursor->y};
  grab->saved_box = leme_placement_anchored_box(&anchor);
  grab->cursor_x = server->cursor->x;
  grab->cursor_y = server->cursor->y;
  grab->restore_pending = false;
}

static bool leme_input_pointer_detach_motion(struct leme_pointer_grab *grab) {
  struct leme_server *server = grab->server;
  struct leme_view *view = grab->view;
  struct leme_container *container =
      leme_container_by_public_id(server, grab->source_container);
  if (container == NULL || view == NULL || view->container != container ||
      container->generation != grab->source_generation ||
      !leme_container_member_eligible(view)) {
    leme_input_pointer_grab_cancel(server);
    return false;
  }
  if (hypot(server->cursor->x - grab->cursor_x,
            server->cursor->y - grab->cursor_y) <= 4.0 ||
      leme_box_contains(container->box, server->cursor->x, server->cursor->y)) {
    return false;
  }
  if (container->count == 2) {
    grab->source_survivor = container->views[0] == view
                                ? container->views[1]->public_meta.id
                                : container->views[0]->public_meta.id;
  }
  const struct leme_public_id id = view->public_meta.id;
  grab->view = NULL;
  leme_container_remove_view(view);
  view = leme_view_by_public_id(server, id);
  if (view == NULL) {
    leme_input_pointer_grab_cancel(server);
    return false;
  }
  const struct leme_placement_anchor anchor = {
      .original = grab->source_content,
      .restored = leme_render_view_content_box(view, view->box),
      .frame = view->box,
      .start_x = grab->cursor_x,
      .start_y = grab->cursor_y,
      .cursor_x = server->cursor->x,
      .cursor_y = server->cursor->y};
  grab->view = view;
  grab->mode = LEME_POINTER_GRAB_FLOAT_MOVE;
  grab->saved_box = leme_placement_anchored_box(&anchor);
  grab->cursor_x = server->cursor->x;
  grab->cursor_y = server->cursor->y;
  grab->snap_enabled = true;
  leme_view_set_configure_deferred(view, true);
  leme_render_view_focus(view);
  return true;
}

static void leme_input_pointer_split_motion(struct leme_pointer_grab *grab) {
  struct leme_server *server = grab->server;
  struct leme_container *container =
      leme_container_by_public_id(server, grab->source_container);
  if (container == NULL || container->generation != grab->source_generation) {
    leme_input_pointer_grab_cancel(server);
    return;
  }
  const int dx =
      leme_input_pointer_round_delta(server->cursor->x - grab->cursor_x);
  const int dy =
      leme_input_pointer_round_delta(server->cursor->y - grab->cursor_y);
  if (leme_layout_resize_drag_update(&grab->resize_drag, dx, dy)) {
    leme_container_arrange(container, false);
  }
}

static void leme_input_pointer_grab_motion(
    struct wlr_seat_pointer_grab *seat_grab,
    uint32_t time_msec, // NOLINT(bugprone-easily-swappable-parameters)
    double sx, double sy) {
  struct leme_pointer_grab *grab = seat_grab->data;
  struct leme_box box;
  int dx;
  int dy;

  (void)time_msec;
  (void)sx;
  (void)sy;
  if (grab->mode == LEME_POINTER_GRAB_CONTAINER_DETACH &&
      !leme_input_pointer_detach_motion(grab)) {
    return;
  }
  if (grab->mode == LEME_POINTER_GRAB_CONTAINER_SPLIT) {
    leme_input_pointer_split_motion(grab);
    return;
  }
  struct leme_container *container =
      leme_container_by_public_id(grab->server, grab->container);
  if (grab->container.serial != 0 && container == NULL) {
    leme_input_pointer_grab_cancel(grab->server);
    return;
  }
  if ((grab->view == NULL && container == NULL) ||
      grab->mode == LEME_POINTER_GRAB_SINK ||
      grab->mode == LEME_POINTER_GRAB_NONE) {
    return;
  }
  if (grab->restore_pending || grab->titlebar_pending) {
    if (hypot(grab->server->cursor->x - grab->cursor_x,
              grab->server->cursor->y - grab->cursor_y) <= 4.0) {
      return;
    }
    leme_desk_buttons_cancel(grab->server);
    grab->titlebar_pending = false;
    if (grab->restore_pending) {
      leme_input_pointer_grab_restore(grab);
    }
  }
  dx = leme_input_pointer_round_delta(grab->server->cursor->x - grab->cursor_x);
  dy = leme_input_pointer_round_delta(grab->server->cursor->y - grab->cursor_y);
  if (grab->mode == LEME_POINTER_GRAB_TILED_RESIZE) {
    if (leme_layout_resize_drag_update(&grab->resize_drag, dx, dy)) {
      leme_view_arrange_instant(grab->server);
    }
    return;
  }
  if (grab->mode == LEME_POINTER_GRAB_FLOAT_MOVE ||
      grab->mode == LEME_POINTER_GRAB_TILED_MOVE ||
      grab->mode == LEME_POINTER_GRAB_CONTAINER_MOVE) {
    leme_input_pointer_grab_update_destination(grab);
    box = grab->saved_box;
    box.x = leme_input_pointer_add_saturated(box.x, dx);
    box.y = leme_input_pointer_add_saturated(box.y, dy);
    if (container != NULL &&
        !leme_input_pointer_grab_cross_allowed(grab->server) &&
        grab->destination == container->tag->owner->output) {
      box = leme_view_policy_clamp_box(
          box, leme_placement_area(container->tag, grab->destination));
    }
    if (grab->mode == LEME_POINTER_GRAB_FLOAT_MOVE &&
        !leme_input_pointer_grab_cross_allowed(grab->server) &&
        grab->destination != NULL) {
      box = leme_view_policy_clamp_box(
          box, leme_output_usable_box(grab->destination));
    }
  } else {
    box = leme_layout_resize_box(grab->saved_box, grab->edges, dx, dy);
  }
  if (container != NULL && grab->mode == LEME_POINTER_GRAB_CONTAINER_RESIZE &&
      leme_container_content_box(container, box).width <= 0) {
    return;
  }
  const bool applied =
      container != NULL
          ? leme_container_apply_interactive_box(container, box)
          : leme_view_apply_interactive_box(
                grab->view, box, grab->mode == LEME_POINTER_GRAB_FLOAT_RESIZE);
  if (!applied) {
    leme_input_pointer_grab_cancel(grab->server);
    return;
  }
  if (grab->mode == LEME_POINTER_GRAB_FLOAT_MOVE ||
      grab->mode == LEME_POINTER_GRAB_CONTAINER_MOVE) {
    leme_input_pointer_grab_snap(grab);
  }
  if (grab->mode == LEME_POINTER_GRAB_TILED_MOVE) {
    struct leme_tags *tags = leme_output_tags(grab->destination);
    struct leme_view *target = leme_tags_pointer_drop_target(
        tags, grab->view, grab->server->cursor->x, grab->server->cursor->y);
    struct leme_layout_drop drop;

    grab->drop_empty = target == NULL && tags != NULL &&
                       grab->destination != leme_view_output(grab->view);
    if (target != grab->drop_target) {
      leme_input_pointer_grab_hide_preview(grab);
    }
    if (target == NULL ||
        !leme_layout_drop_at(
            grab->drop_mode, leme_render_view_content_box(target, target->box),
            grab->server->cursor->x, grab->server->cursor->y, &drop)) {
      grab->drop_target = NULL;
      return;
    }
    grab->drop_target = target;
    grab->drop_direction = drop.direction;
    if (!leme_render_view_show_drop_preview(target, drop.box) &&
        !grab->preview_failed) {
      wlr_log(WLR_ERROR, "%s", "leme: failed to render tiled drop preview");
      grab->preview_failed = true;
    }
  }
}

static uint32_t leme_input_pointer_grab_button(
    struct wlr_seat_pointer_grab *seat_grab,
    uint32_t time_msec, // NOLINT(bugprone-easily-swappable-parameters)
    uint32_t button, enum wl_pointer_button_state state) {
  struct leme_pointer_grab *grab = seat_grab->data;

  (void)time_msec;
  if (button == grab->button && state == WL_POINTER_BUTTON_STATE_RELEASED &&
      !leme_input_pointer_button_held(grab->server->seat, button)) {
    grab->released = true;
    struct leme_server *server = grab->server;
    const struct leme_public_id id = grab->view == NULL
                                         ? (struct leme_public_id){0}
                                         : grab->view->public_meta.id;
    const enum leme_view_placement placement = grab->snap_placement;
    const struct leme_public_id container_id = grab->container;
    const struct leme_container_drop drop = grab->container_drop;
    struct leme_output *destination = grab->destination;
    struct leme_container *moving_container =
        leme_container_by_public_id(server, container_id);
    struct leme_output *source = moving_container == NULL
                                     ? leme_view_output(grab->view)
                                     : moving_container->tag->owner->output;
    bool crossing =
        destination != NULL && source != NULL && destination != source;

    if (grab->mode == LEME_POINTER_GRAB_TILED_MOVE && grab->view != NULL &&
        grab->detach != NULL) {
      struct leme_view *target = grab->drop_target;
      enum leme_direction direction = grab->drop_direction;

      leme_input_pointer_grab_hide_preview(grab);
      if (crossing && (target != NULL || grab->drop_empty)) {
        if (!leme_view_drop_to_output(grab->view, &grab->detach, destination,
                                      target, direction)) {
          leme_view_restore_tiled_drag(grab->view, &grab->detach);
        }
      } else if (target == NULL ||
                 !leme_view_commit_tiled_drag(grab->view, &grab->detach, target,
                                              direction)) {
        leme_view_restore_tiled_drag(grab->view, &grab->detach);
      }
    } else {
      leme_input_pointer_grab_hide_preview(grab);
      if (crossing && grab->mode == LEME_POINTER_GRAB_CONTAINER_MOVE &&
          moving_container != NULL) {
        (void)leme_container_move_to_output(moving_container, destination,
                                            true);
      } else if (grab->mode == LEME_POINTER_GRAB_CONTAINER_MOVE &&
                 moving_container != NULL && source != NULL) {
        (void)leme_container_apply_interactive_box(
            moving_container,
            leme_view_policy_clamp_box(
                moving_container->box,
                leme_placement_area(moving_container->tag, source)));
      } else if (crossing && grab->mode == LEME_POINTER_GRAB_FLOAT_MOVE &&
                 drop.target.serial == 0) {
        if (leme_view_is_shown_scratchpad(grab->view)) {
          (void)leme_scratchpad_move_shown(grab->view, destination);
        } else if (leme_view_is_sticky(grab->view)) {
          (void)leme_sticky_move_to_output(grab->view, destination, true);
        } else {
          (void)leme_view_drop_to_output(grab->view, NULL, destination, NULL,
                                         LEME_DIRECTION_LEFT);
        }
      }
    }
    wlr_seat_pointer_end_grab(server->seat);
    struct leme_container *container =
        leme_container_by_public_id(server, container_id);
    if (container != NULL && container->tag->owner->output == destination &&
        placement != LEME_VIEW_PLACEMENT_NORMAL) {
      (void)leme_container_set_placement(container, placement);
      leme_input_process_motion(server, 0, NULL, false);
    }
    struct leme_view *view = leme_view_by_public_id(server, id);
    if (view != NULL && placement != LEME_VIEW_PLACEMENT_NORMAL &&
        leme_view_output(view) == destination &&
        leme_view_set_placement(view, placement)) {
      leme_input_process_motion(server, 0, NULL, false);
    } else if (view != NULL && placement == LEME_VIEW_PLACEMENT_NORMAL &&
               drop.target.serial != 0) {
      if (leme_container_commit_drop(server, &drop)) {
        if (crossing) {
          leme_output_set_focused(server, destination, false);
          leme_view_focus(view);
        }
        leme_input_process_motion(server, 0, NULL, false);
      } else if (crossing && view->container == NULL &&
                 leme_view_output(view) != destination) {
        (void)leme_view_drop_to_output(view, NULL, destination, NULL,
                                       LEME_DIRECTION_LEFT);
        leme_input_process_motion(server, 0, NULL, false);
      }
    }
  }
  return 0;
}

static void leme_input_pointer_grab_axis(
    struct wlr_seat_pointer_grab *seat_grab,
    uint32_t time_msec, // NOLINT(bugprone-easily-swappable-parameters)
    enum wl_pointer_axis orientation, double value, int32_t value_discrete,
    enum wl_pointer_axis_source source,
    enum wl_pointer_axis_relative_direction relative_direction) {
  (void)seat_grab;
  (void)time_msec;
  (void)orientation;
  (void)value;
  (void)value_discrete;
  (void)source;
  (void)relative_direction;
}

static void
leme_input_pointer_grab_frame(struct wlr_seat_pointer_grab *seat_grab) {
  (void)seat_grab;
}

static void
leme_input_pointer_grab_cancelled(struct wlr_seat_pointer_grab *seat_grab) {
  struct leme_pointer_grab *grab = seat_grab->data;

  if (!grab->released) {
    leme_desk_buttons_cancel(grab->server);
  }
  leme_input_pointer_grab_cleanup(grab);
}

static const struct wlr_pointer_grab_interface leme_pointer_grab_interface = {
    .enter = leme_input_pointer_grab_enter,
    .clear_focus = leme_input_pointer_grab_clear_focus,
    .motion = leme_input_pointer_grab_motion,
    .button = leme_input_pointer_grab_button,
    .axis = leme_input_pointer_grab_axis,
    .frame = leme_input_pointer_grab_frame,
    .cancel = leme_input_pointer_grab_cancelled,
};

static bool leme_input_pointer_container_start(struct leme_container *container,
                                               enum leme_grab_edge edges,
                                               uint32_t button) {
  if (container == NULL || !leme_container_action_allowed(container->focused)) {
    return false;
  }
  struct leme_server *server = container->tag->owner->server;
  struct leme_pointer_grab *grab = server->pointer_grab;
  const bool resize = edges != LEME_GRAB_EDGE_NONE;
  enum leme_grab_edge checked = LEME_GRAB_EDGE_NONE;
  if (grab == NULL || server->seat == NULL || server->cursor == NULL ||
      (resize && (!leme_input_pointer_edges(leme_input_pointer_wlr_edges(edges),
                                            &checked) ||
                  checked != edges ||
                  leme_render_container_resize_edges(
                      container, server->cursor->x, server->cursor->y) !=
                      leme_input_pointer_wlr_edges(edges))) ||
      !leme_desktop_cursor_override(server,
                                    leme_input_pointer_grab_cursor(edges))) {
    return false;
  }
  if (resize && !leme_container_unplace_for_resize(container)) {
    leme_desktop_cursor_restore(server);
    return false;
  }
  leme_render_output_animations_finish(container->tag->owner->output);
  *grab = (struct leme_pointer_grab){
      .server = server,
      .container = container->id,
      .mode = resize ? LEME_POINTER_GRAB_CONTAINER_RESIZE
                     : LEME_POINTER_GRAB_CONTAINER_MOVE,
      .button = button,
      .cursor_x = server->cursor->x,
      .cursor_y = server->cursor->y,
      .saved_box = container->box,
      .edges = edges,
      .titlebar_pending = !resize,
      .restore_pending =
          container->placement.kind != LEME_VIEW_PLACEMENT_NORMAL,
      .snap_enabled = !resize,
      .destination = container->tag->owner->output,
      .seat_grab = {.interface = &leme_pointer_grab_interface, .data = grab}};
  leme_input_pointer_container_defer(grab, true);
  wlr_seat_pointer_start_grab(server->seat, &grab->seat_grab);
  leme_input_protocols_update_pointer_focus(server);
  return true;
}

static bool leme_input_pointer_start_split(struct leme_view *view,
                                           enum leme_grab_edge edges,
                                           uint32_t button) {
  if (!leme_container_action_allowed(view) ||
      !leme_layout_drag_supported(&view->container->layout)) {
    return false;
  }
  struct leme_server *server = view->server;
  struct leme_pointer_grab *grab = server->pointer_grab;
  struct leme_layout_resize_drag drag = {0};
  if (grab == NULL || server->seat == NULL || server->cursor == NULL ||
      !leme_layout_resize_drag_begin(view->container->layout.root, view, edges,
                                     &drag)) {
    return false;
  }
  if (drag.horizontal == NULL) {
    edges = (enum leme_grab_edge)(edges &
                                  (LEME_GRAB_EDGE_TOP | LEME_GRAB_EDGE_BOTTOM));
  }
  if (drag.vertical == NULL) {
    edges = (enum leme_grab_edge)(edges &
                                  (LEME_GRAB_EDGE_LEFT | LEME_GRAB_EDGE_RIGHT));
  }
  if (!leme_desktop_cursor_override(server,
                                    leme_input_pointer_grab_cursor(edges))) {
    return false;
  }
  leme_render_output_animations_finish(leme_view_output(view));
  *grab = (struct leme_pointer_grab){
      .server = server,
      .view = view,
      .mode = LEME_POINTER_GRAB_CONTAINER_SPLIT,
      .source_container = view->container->id,
      .source_generation = view->container->generation,
      .button = button,
      .cursor_x = server->cursor->x,
      .cursor_y = server->cursor->y,
      .saved_box = view->box,
      .edges = edges,
      .resize_drag = drag,
      .destination = leme_view_output(view),
      .seat_grab = {.interface = &leme_pointer_grab_interface, .data = grab}};
  wlr_seat_pointer_start_grab(server->seat, &grab->seat_grab);
  leme_input_protocols_update_pointer_focus(server);
  return true;
}

static bool leme_input_pointer_start_detach(struct leme_view *view,
                                            uint32_t button) {
  if (!leme_container_action_allowed(view)) {
    return false;
  }
  struct leme_server *server = view->server;
  struct leme_pointer_grab *grab = server->pointer_grab;
  if (grab == NULL || server->seat == NULL || server->cursor == NULL ||
      !leme_desktop_cursor_override(server, "grabbing")) {
    return false;
  }
  leme_render_output_animations_finish(leme_view_output(view));
  *grab = (struct leme_pointer_grab){
      .server = server,
      .view = view,
      .mode = LEME_POINTER_GRAB_CONTAINER_DETACH,
      .source_container = view->container->id,
      .source_generation = view->container->generation,
      .source_content = leme_render_view_content_box(view, view->box),
      .button = button,
      .cursor_x = server->cursor->x,
      .cursor_y = server->cursor->y,
      .saved_box = view->box,
      .destination = leme_view_output(view),
      .seat_grab = {.interface = &leme_pointer_grab_interface, .data = grab}};
  wlr_seat_pointer_start_grab(server->seat, &grab->seat_grab);
  leme_input_protocols_update_pointer_focus(server);
  return true;
}

bool leme_input_pointer_grab_start_container(struct leme_container *container,
                                             uint32_t button) {
  return leme_input_pointer_container_start(container, LEME_GRAB_EDGE_NONE,
                                            button);
}

bool leme_input_pointer_grab_start_container_resize(
    struct leme_container *container, enum leme_grab_edge edges,
    uint32_t button) {
  return edges != LEME_GRAB_EDGE_NONE &&
         leme_input_pointer_container_start(container, edges, button);
}

static bool leme_input_pointer_grab_start(
    struct leme_view *view, bool resize,
    uint32_t button, // NOLINT(bugprone-easily-swappable-parameters)
    uint32_t wlr_edges, bool compositor_owned, bool edge_resize) {
  struct leme_server *server;
  struct leme_pointer_grab *grab;
  enum leme_grab_edge edges = LEME_GRAB_EDGE_NONE;
  const char *cursor_name;
  bool tiled;

  if (view == NULL || view->server == NULL) {
    return false;
  }
  server = view->server;
  if (resize && !edge_resize && compositor_owned && view->container != NULL) {
    return leme_input_pointer_edges(wlr_edges, &edges) &&
           leme_input_pointer_start_split(view, edges, button);
  }
  grab = server->pointer_grab;
  tiled = leme_view_geometry_owner(view) == LEME_VIEW_GEOMETRY_TAG_LAYOUT;
  if (grab == NULL || server->seat == NULL || server->cursor == NULL ||
      wlr_seat_pointer_has_grab(server->seat) || leme_session_locked(server) ||
      server->seat->drag != NULL || !view->mapped || view->unmanaged ||
      view->detached || view->fullscreen ||
      leme_view_geometry_owner(view) == LEME_VIEW_GEOMETRY_CONTAINER ||
      (leme_view_is_placed(view) && resize && !edge_resize) ||
      (edge_resize &&
       (!resize || tiled || !compositor_owned || leme_view_is_maximized(view) ||
        leme_render_view_resize_edges(view, server->cursor->x,
                                      server->cursor->y) != wlr_edges)) ||
      (leme_view_is_scratchpad(view) && !leme_view_is_shown_scratchpad(view)) ||
      (tiled && leme_ownership_tag(view) == NULL) ||
      (!compositor_owned && tiled) ||
      (resize && !leme_input_pointer_edges(wlr_edges, &edges)) ||
      grab->detach != NULL) {
    return false;
  }
  if (tiled && resize) {
    if (!leme_layout_drag_supported(&leme_ownership_tag(view)->layout) ||
        !leme_layout_resize_drag_begin(leme_ownership_tag(view)->layout.root,
                                       view, edges, &grab->resize_drag)) {
      return false;
    }
    if (grab->resize_drag.horizontal == NULL) {
      edges = (enum leme_grab_edge)(
          edges & (LEME_GRAB_EDGE_TOP | LEME_GRAB_EDGE_BOTTOM));
    }
    if (grab->resize_drag.vertical == NULL) {
      edges = (enum leme_grab_edge)(
          edges & (LEME_GRAB_EDGE_LEFT | LEME_GRAB_EDGE_RIGHT));
    }
  }
  cursor_name =
      tiled && !resize ? "grabbing" : leme_input_pointer_grab_cursor(edges);
  if (!leme_desktop_cursor_override(server, cursor_name)) {
    grab->resize_drag = (struct leme_layout_resize_drag){0};
    return false;
  }
  grab->hover_edges = LEME_GRAB_EDGE_NONE;
  if ((tiled && !resize && !leme_view_begin_tiled_drag(view, &grab->detach)) ||
      (edge_resize && !leme_view_unplace_for_resize(view))) {
    leme_desktop_cursor_restore(server);
    return false;
  }
  if (tiled) {
    grab->mode =
        resize ? LEME_POINTER_GRAB_TILED_RESIZE : LEME_POINTER_GRAB_TILED_MOVE;
  } else {
    grab->mode =
        resize ? LEME_POINTER_GRAB_FLOAT_RESIZE : LEME_POINTER_GRAB_FLOAT_MOVE;
  }
  const struct leme_tag *tag = leme_ownership_tag(view);
  if (!resize && tag != NULL && tag->layout.kind == LEME_LAYOUT_DESKTOP) {
    leme_render_view_finish_animation(view);
  }
  grab->view = view;
  grab->button = button;
  grab->restore_pending = leme_view_is_placed(view);
  grab->titlebar_pending = false;
  grab->released = false;
  grab->snap_enabled = !resize && !tiled;
  grab->snap_placement = LEME_VIEW_PLACEMENT_NORMAL;
  grab->cursor_x = server->cursor->x;
  grab->cursor_y = server->cursor->y;
  grab->saved_box = view->box;
  grab->edges = edges;
  grab->drop_target = NULL;
  grab->drop_direction = LEME_DIRECTION_LEFT;
  grab->drop_mode = server->config == NULL ? LEME_DROP_MODE_SIMPLE
                                           : server->config->drop_mode;
  grab->preview_failed = false;
  grab->destination = leme_view_output(view);
  grab->drop_empty = false;
  grab->seat_grab.interface = &leme_pointer_grab_interface;
  grab->seat_grab.data = grab;
  if (view->kind == LEME_VIEW_XWAYLAND) {
    leme_view_set_configure_deferred(view, true);
  }
  wlr_seat_pointer_start_grab(server->seat, &grab->seat_grab);
  leme_input_protocols_update_pointer_focus(server);
  return true;
}

bool leme_input_pointer_grab_start_decoration(struct leme_view *view,
                                              uint32_t button) {
  if (!leme_desk_decoration_eligible(view) || leme_view_is_minimized(view) ||
      view->server->seat == NULL || !leme_input_idle(view->server) ||
      leme_input_protocols_shortcuts_inhibited(view->server)) {
    return false;
  }
  if (!leme_input_pointer_grab_start(view, false, button, WLR_EDGE_NONE, true,
                                     false)) {
    return false;
  }
  view->server->pointer_grab->titlebar_pending = true;
  return true;
}

bool leme_input_interacting(const struct leme_server *server) {
  return leme_input_pointer_grab_active(server) ||
         (server->seat != NULL && server->seat->drag != NULL);
}

bool leme_input_idle(const struct leme_server *server) {
  return !leme_session_locked(server) &&
         !leme_layer_keyboard_is_exclusive(server) &&
         !leme_input_protocols_pointer_locked(server) &&
         (server->seat == NULL || (server->seat->drag == NULL &&
                                   !wlr_seat_pointer_has_grab(server->seat) &&
                                   !wlr_seat_keyboard_has_grab(server->seat)));
}

uint32_t leme_input_modifiers(const struct leme_server *server) {
  struct wlr_keyboard *keyboard =
      server->seat == NULL ? NULL : wlr_seat_get_keyboard(server->seat);
  return keyboard == NULL ? 0
                          : wlr_keyboard_get_modifiers(keyboard) &
                                (WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL |
                                 WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO);
}

bool leme_input_pointer_grab_active(const struct leme_server *server) {
  return server != NULL && server->pointer_grab != NULL &&
         server->seat != NULL &&
         server->seat->pointer_state.grab == &server->pointer_grab->seat_grab;
}

void leme_input_pointer_grab_modifiers_changed(struct leme_server *server) {
  if (!leme_input_pointer_grab_active(server)) {
    return;
  }
  struct leme_pointer_grab *grab = server->pointer_grab;
  if (grab->mode != LEME_POINTER_GRAB_FLOAT_MOVE || grab->view == NULL ||
      grab->restore_pending || grab->titlebar_pending) {
    return;
  }
  leme_input_pointer_grab_snap(grab);
}

bool leme_input_pointer_grabbing(const struct leme_server *server,
                                 const struct leme_view *view) {
  return leme_input_pointer_grab_active(server) &&
         server->pointer_grab->view == view;
}

bool leme_input_pointer_grab_start_xdg(
    struct leme_view *view, bool resize,
    uint32_t serial, // NOLINT(bugprone-easily-swappable-parameters)
    uint32_t wlr_edges) {
  struct leme_server *server;
  uint32_t button;

  if (view == NULL || view->kind != LEME_VIEW_XDG) {
    return false;
  }
  server = view->server;
  button = server->seat->pointer_state.grab_button;
  if (leme_view_from_surface(
          server, server->seat->keyboard_state.focused_surface) != view ||
      leme_view_from_surface(
          server, server->seat->pointer_state.focused_surface) != view ||
      !leme_input_pointer_button_held(server->seat, button) ||
      !wlr_seat_validate_pointer_grab_serial(server->seat, NULL, serial)) {
    return false;
  }
  return leme_input_pointer_grab_start(view, resize, button, wlr_edges, false,
                                       false);
}

bool leme_input_pointer_grab_start_xwayland(struct leme_view *view, bool resize,
                                            uint32_t wlr_edges) {
  struct leme_server *server;
  uint32_t button;

  if (view == NULL || view->kind != LEME_VIEW_XWAYLAND) {
    return false;
  }
  server = view->server;
  if (server->focused_view != view ||
      leme_view_from_surface(
          server, server->seat->pointer_state.focused_surface) != view ||
      leme_input_pointer_button_press_count(server->seat) != 1) {
    return false;
  }
  button = server->seat->pointer_state.buttons[0].button;
  return leme_input_pointer_grab_start(view, resize, button, wlr_edges, false,
                                       false);
}

static void leme_input_pointer_grab_sink(struct leme_pointer_grab *grab) {
  struct leme_view *view = grab->view;

  leme_input_pointer_grab_hide_preview(grab);
  leme_desk_buttons_cancel(grab->server);
  grab->titlebar_pending = false;
  grab->restore_pending = false;
  grab->snap_enabled = false;
  grab->mode = LEME_POINTER_GRAB_SINK;
  leme_input_pointer_container_defer(grab, false);
  grab->container = (struct leme_public_id){0};
  leme_view_set_configure_deferred(grab->view, false);
  grab->view = NULL;
  leme_view_follow_committed_size(view);
  leme_desk_decoration_refresh(view);
  grab->resize_drag = (struct leme_layout_resize_drag){0};
  leme_desktop_cursor_restore(grab->server);
  if (!leme_input_pointer_button_held(grab->server->seat, grab->button)) {
    wlr_seat_pointer_end_grab(grab->server->seat);
  }
}

void leme_input_pointer_grab_cancel(struct leme_server *server) {
  struct leme_pointer_grab *grab;

  if (!leme_input_pointer_grab_active(server)) {
    return;
  }
  grab = server->pointer_grab;
  leme_input_pointer_grab_hide_preview(grab);
  if (grab->mode == LEME_POINTER_GRAB_TILED_MOVE && grab->view != NULL &&
      grab->detach != NULL) {
    leme_view_restore_tiled_drag(grab->view, &grab->detach);
  }
  leme_input_pointer_grab_sink(grab);
}

void leme_input_pointer_grab_cancel_tiled(struct leme_server *server) {
  if (leme_input_pointer_grab_active(server) &&
      leme_input_pointer_grab_mode_tiled(server->pointer_grab->mode)) {
    leme_input_pointer_grab_cancel(server);
  }
}

void leme_input_pointer_grab_cancel_view(struct leme_view *view) {
  struct leme_pointer_grab *grab;

  if (view == NULL || !leme_input_pointer_grab_active(view->server)) {
    return;
  }
  grab = view->server->pointer_grab;
  if (view->container != NULL &&
      (grab->container.serial == view->container->id.serial ||
       (grab->mode == LEME_POINTER_GRAB_CONTAINER_SPLIT &&
        grab->source_container.serial == view->container->id.serial))) {
    leme_input_pointer_grab_cancel(view->server);
    return;
  }
  if (grab->container_drop.target.serial == view->public_meta.id.serial) {
    leme_input_pointer_grab_hide_preview(grab);
  }
  if (grab->mode == LEME_POINTER_GRAB_TILED_MOVE && grab->view == view) {
    leme_input_pointer_grab_hide_preview(grab);
    leme_view_discard_tiled_drag(view, &grab->detach);
    leme_input_pointer_grab_sink(grab);
    return;
  }
  if (leme_input_pointer_grab_mode_tiled(grab->mode)) {
    if (grab->view == view ||
        (grab->view != NULL && leme_ownership_tag(view) != NULL &&
         leme_ownership_tag(view) == leme_ownership_tag(grab->view) &&
         leme_view_geometry_owner(view) != LEME_VIEW_GEOMETRY_FLOATING &&
         !view->unmanaged)) {
      leme_input_pointer_grab_cancel(view->server);
    }
    return;
  }
  if (grab->view == view) {
    leme_input_pointer_grab_cancel(view->server);
  }
}

void leme_input_pointer_grab_finish(struct leme_server *server) {
  if (leme_input_pointer_grab_active(server)) {
    wlr_seat_pointer_end_grab(server->seat);
  }
}

static struct leme_pointer_settings
leme_input_pointer_settings(const struct leme_config *config,
                            const char *device_name) {
  struct leme_pointer_settings settings = config->pointer_defaults;
  size_t index;

  for (index = 0; device_name != NULL && index < config->pointer_rule_count;
       index++) {
    const struct leme_pointer_rule *rule = &config->pointer_rules[index];
    const struct leme_pointer_settings *override = &rule->settings;

    if (strcmp(rule->name, device_name) != 0) {
      continue;
    }
    if ((override->fields & LEME_POINTER_PROFILE) != 0) {
      settings.profile = override->profile;
    }
    if ((override->fields & LEME_POINTER_SPEED) != 0) {
      settings.speed = override->speed;
    }
    if ((override->fields & LEME_POINTER_NATURAL_SCROLL) != 0) {
      settings.natural_scroll = override->natural_scroll;
    }
    if ((override->fields & LEME_POINTER_LEFT_HANDED) != 0) {
      settings.left_handed = override->left_handed;
    }
    if ((override->fields & LEME_POINTER_TAP) != 0) {
      settings.tap = override->tap;
    }
    break;
  }
  return settings;
}

static void leme_input_pointer_status(const struct wlr_input_device *device,
                                      const char *property,
                                      enum libinput_config_status status) {
  if (status != LIBINPUT_CONFIG_STATUS_SUCCESS) {
    wlr_log(WLR_ERROR, "leme: pointer %s failed to set %s: %s",
            device->name == NULL ? "(unnamed)" : device->name, property,
            libinput_config_status_to_str(status));
  }
}

static void leme_input_configure_pointer(struct leme_server *server,
                                         struct wlr_input_device *device,
                                         struct leme_public_id public_id,
                                         const struct leme_config *config) {
  struct leme_pointer_settings settings;
  struct libinput_device *libinput_device;
  enum libinput_config_accel_profile profile;
  uint32_t profiles;

  if (config == NULL || !wlr_input_device_is_libinput(device)) {
    return;
  }
  libinput_device = wlr_libinput_get_device_handle(device);
  if (libinput_device == NULL) {
    return;
  }
  settings = leme_input_pointer_settings(config, device->name);
  if (server != NULL && server->config_store != NULL) {
    size_t count = 0;
    const struct leme_scoped_override *ovs =
        leme_config_store_overrides(server->config_store, &count);
    for (size_t i = 0; i < count; i++) {
      if (ovs[i].has_target && ovs[i].target.kind == LEME_PUBLIC_INPUT &&
          ovs[i].target.id.serial == public_id.serial &&
          ovs[i].path_count == 1 && ovs[i].path != NULL &&
          ovs[i].path[0] != NULL) {
        if (strcmp(ovs[i].path[0], "accel_profile") == 0) {
          struct leme_public_text text = {0};
          if (leme_public_as_text(ovs[i].value, &text)) {
            if (text.length == 4 && memcmp(text.data, "flat", 4) == 0) {
              settings.profile = LEME_POINTER_ACCEL_FLAT;
            } else if (text.length == 8 &&
                       memcmp(text.data, "adaptive", 8) == 0) {
              settings.profile = LEME_POINTER_ACCEL_ADAPTIVE;
            }
          }
        } else if (strcmp(ovs[i].path[0], "accel_speed") == 0) {
          double sp = 0;
          if (leme_public_as_number(ovs[i].value, &sp)) {
            settings.speed = sp;
          }
        } else if (strcmp(ovs[i].path[0], "natural_scroll") == 0) {
          bool b = false;
          if (leme_public_as_bool(ovs[i].value, &b)) {
            settings.natural_scroll = b;
          }
        } else if (strcmp(ovs[i].path[0], "left_handed") == 0) {
          bool b = false;
          if (leme_public_as_bool(ovs[i].value, &b)) {
            settings.left_handed = b;
          }
        } else if (strcmp(ovs[i].path[0], "tap") == 0) {
          bool b = false;
          if (leme_public_as_bool(ovs[i].value, &b)) {
            settings.tap = b;
          }
        }
      }
    }
  }
  profile = settings.profile == LEME_POINTER_ACCEL_FLAT
                ? LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT
                : LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE;
  profiles = libinput_device_config_accel_get_profiles(libinput_device);
  if ((profiles & profile) != 0) {
    leme_input_pointer_status(
        device, "accel_profile",
        libinput_device_config_accel_set_profile(libinput_device, profile));
  }
  if (libinput_device_config_accel_is_available(libinput_device)) {
    leme_input_pointer_status(device, "accel_speed",
                              libinput_device_config_accel_set_speed(
                                  libinput_device, settings.speed));
  }
  if (libinput_device_config_scroll_has_natural_scroll(libinput_device)) {
    leme_input_pointer_status(
        device, "natural_scroll",
        libinput_device_config_scroll_set_natural_scroll_enabled(
            libinput_device, settings.natural_scroll));
  }
  if (libinput_device_config_left_handed_is_available(libinput_device)) {
    leme_input_pointer_status(device, "left_handed",
                              libinput_device_config_left_handed_set(
                                  libinput_device, settings.left_handed));
  }
  if (libinput_device_config_tap_get_finger_count(libinput_device) > 0) {
    leme_input_pointer_status(
        device, "tap",
        libinput_device_config_tap_set_enabled(
            libinput_device, settings.tap ? LIBINPUT_CONFIG_TAP_ENABLED
                                          : LIBINPUT_CONFIG_TAP_DISABLED));
  }
  wlr_log(WLR_INFO, "leme: configured pointer %s",
          device->name == NULL ? "(unnamed)" : device->name);
}

void leme_input_apply_pointer_config(struct leme_server *server,
                                     const struct leme_config *config) {
  struct leme_pointer *pointer;

  if (server == NULL || config == NULL || server->cursor == NULL) {
    return;
  }
  leme_input_workspace_gesture_cancel(server);
  wl_list_for_each(pointer, &server->pointers, link) {
    leme_input_configure_pointer(server, pointer->device, pointer->public_id,
                                 config);
  }
  leme_public_server_invalidate(server);
}

static void leme_input_handle_pointer_destroy(struct wl_listener *listener,
                                              void *data) {
  struct leme_pointer *pointer = wl_container_of(listener, pointer, destroy);
  struct leme_server *server = pointer->server;

  (void)data;
  if (server->gesture.pointer != NULL &&
      &server->gesture.pointer->base == pointer->device) {
    leme_input_workspace_gesture_cancel(server);
  }
  wlr_cursor_detach_input_device(server->cursor, pointer->device);
  wl_list_remove(&pointer->destroy.link);
  wl_list_remove(&pointer->link);
  if (server->config_store != NULL) {
    struct leme_control_target target = {
        .kind = LEME_PUBLIC_INPUT,
        .id = pointer->public_id,
    };
    leme_config_store_drop_target(server->config_store, &target);
  }
  free(pointer);
  leme_input_update_capabilities(server);
  leme_public_server_invalidate(server);
}

void leme_input_pointer_add(struct leme_server *server,
                            struct wlr_input_device *device) {
  struct leme_pointer *pointer = calloc(1, sizeof(*pointer));

  if (pointer == NULL) {
    return;
  }
  pointer->server = server;
  pointer->device = device;
  if (leme_public_model_available(server->public_model) &&
      leme_public_model_issue_id(server->public_model, &pointer->public_id) !=
          LEME_PUBLIC_OK) {
    leme_public_model_disable(server->public_model);
  }
  leme_input_configure_pointer(server, device, pointer->public_id,
                               server->config);
  pointer->destroy.notify = leme_input_handle_pointer_destroy;
  wl_signal_add(&device->events.destroy, &pointer->destroy);
  wl_list_insert(&server->pointers, &pointer->link);
  wlr_cursor_attach_input_device(server->cursor, device);
  leme_public_server_invalidate(server);
}

enum leme_public_status
leme_input_public_pointers(const struct leme_server *server,
                           leme_public_input_visitor visit, void *context) {
  if (server == NULL || visit == NULL || server->pointers.next == NULL)
    return LEME_PUBLIC_INVALID;
  if (leme_session_locked(server))
    return LEME_PUBLIC_LOCKED;
  if (!leme_public_model_available(server->public_model))
    return LEME_PUBLIC_UNAVAILABLE;
  const struct leme_pointer *pointer = NULL;
  wl_list_for_each(pointer, &server->pointers, link) {
    struct leme_public_input_info info = {
        .id = pointer->public_id,
        .name = pointer->device->name,
        .seat = server->seat == NULL ? NULL : server->seat->name,
        .libinput = wlr_input_device_is_libinput(pointer->device)};
    struct libinput_device *device =
        info.libinput ? wlr_libinput_get_device_handle(pointer->device) : NULL;
    if (device != NULL) {
      info.has_ids = true;
      info.vendor = libinput_device_get_id_vendor(device);
      info.product = libinput_device_get_id_product(device);
      const uint32_t profiles =
          libinput_device_config_accel_get_profiles(device);
      info.adaptive = (profiles & LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE) != 0;
      info.flat = (profiles & LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT) != 0;
      const enum libinput_config_accel_profile profile =
          libinput_device_config_accel_get_profile(device);
      if (profile == LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE)
        info.accel_profile = "adaptive";
      if (profile == LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT)
        info.accel_profile = "flat";
      info.has_accel = libinput_device_config_accel_is_available(device) != 0;
      if (info.has_accel)
        info.accel_speed = libinput_device_config_accel_get_speed(device);
      info.has_natural_scroll =
          libinput_device_config_scroll_has_natural_scroll(device) != 0;
      if (info.has_natural_scroll)
        info.natural_scroll =
            libinput_device_config_scroll_get_natural_scroll_enabled(device) !=
            0;
      info.has_left_handed =
          libinput_device_config_left_handed_is_available(device) != 0;
      if (info.has_left_handed)
        info.left_handed = libinput_device_config_left_handed_get(device) != 0;
      info.has_tap = libinput_device_config_tap_get_finger_count(device) > 0;
      if (info.has_tap)
        info.tap = libinput_device_config_tap_get_enabled(device) ==
                   LIBINPUT_CONFIG_TAP_ENABLED;
    }
    const enum leme_public_status status = visit(context, &info);
    if (status != LEME_PUBLIC_OK)
      return status;
  }
  return LEME_PUBLIC_OK;
}

static void leme_input_follow_pointer_output(struct leme_server *server) {
  struct leme_output *output =
      leme_output_at(server, server->cursor->x, server->cursor->y);

  if (output == NULL || output == leme_output_focused(server)) {
    return;
  }
  leme_output_set_focused(server, output, false);
}

static enum leme_layout_kind
leme_input_pointer_view_layout(const struct leme_server *server,
                               const struct leme_view *view) {
  const struct leme_tag *tag = leme_ownership_tag(view);

  return tag != NULL ? tag->layout.kind
                     : leme_tags_layout_kind(leme_focused_tags(server));
}

static struct wlr_seat_client *
leme_input_surface_client(struct leme_server *server,
                          struct wlr_surface *surface) {
  return surface == NULL
             ? NULL
             : wlr_seat_client_for_wl_client(
                   server->seat, wl_resource_get_client(surface->resource));
}

static bool leme_input_pointer_resize_allowed(struct leme_server *server) {
  return server->seat != NULL && server->cursor != NULL &&
         leme_input_idle(server) &&
         !leme_input_protocols_shortcuts_inhibited(server) &&
         server->seat->pointer_state.button_count == 0 &&
         server->desk_owner.owner == NULL;
}

static void leme_input_pointer_resize_hover(struct leme_server *server,
                                            const struct leme_render_hit *hit) {
  struct leme_pointer_grab *grab = server->pointer_grab;
  if (grab == NULL || leme_input_pointer_grab_active(server)) {
    return;
  }
  enum leme_grab_edge edges = LEME_GRAB_EDGE_NONE;
  if ((hit->view != NULL || hit->container != NULL) &&
      hit->resize_edges != WLR_EDGE_NONE &&
      leme_input_pointer_resize_allowed(server)) {
    (void)leme_input_pointer_edges(hit->resize_edges, &edges);
  }
  if (edges == grab->hover_edges) {
    return;
  }
  leme_desktop_cursor_default(server);
  grab->hover_edges = LEME_GRAB_EDGE_NONE;
  if (edges != LEME_GRAB_EDGE_NONE &&
      leme_desktop_cursor_override(server,
                                   leme_input_pointer_grab_cursor(edges))) {
    grab->hover_edges = edges;
  }
}

static void leme_input_process_motion(struct leme_server *server,
                                      uint32_t time_msec,
                                      struct leme_render_hit *result,
                                      bool allow_view_focus) {
  struct leme_render_hit local;
  struct leme_render_hit *hit = result == NULL ? &local : result;

  if (leme_desk_owner_handle_motion(server)) {
    *hit = (struct leme_render_hit){0};
    leme_input_pointer_resize_hover(server, hit);
    return;
  }
  leme_render_at(server, server->cursor->x, server->cursor->y, hit);
  if (!leme_session_surface_allowed(server, hit->surface)) {
    *hit = (struct leme_render_hit){0};
  }
  if (allow_view_focus && !leme_session_locked(server)) {
    leme_input_follow_pointer_output(server);
  }
  struct leme_view *focus = hit->view != NULL        ? hit->view
                            : hit->container == NULL ? NULL
                                                     : hit->container->focused;
  const enum leme_layout_kind layout =
      leme_input_pointer_view_layout(server, focus);
  if (allow_view_focus && !leme_session_locked(server) &&
      leme_view_policy_hover_focus(focus) && layout != LEME_LAYOUT_ACCORDION) {
    leme_view_focus_ex(focus, layout != LEME_LAYOUT_DESKTOP);
  }
  leme_desk_buttons_handle_motion(server, hit);
  if (hit->surface != server->seat->pointer_state.focused_surface &&
      server->seat->drag == NULL && !leme_input_pointer_grab_active(server) &&
      !leme_desktop_cursor_owned_by(
          server, leme_input_surface_client(server, hit->surface))) {
    leme_desktop_cursor_default(server);
    if (server->pointer_grab != NULL) {
      server->pointer_grab->hover_edges = LEME_GRAB_EDGE_NONE;
    }
  }
  leme_input_pointer_resize_hover(server, hit);
  if (hit->surface == NULL) {
    wlr_seat_pointer_notify_clear_focus(server->seat);
    leme_input_protocols_update_pointer_focus(server);
    return;
  }
  wlr_seat_pointer_notify_enter(server->seat, hit->surface, hit->surface_x,
                                hit->surface_y);
  wlr_seat_pointer_notify_motion(server->seat, time_msec, hit->surface_x,
                                 hit->surface_y);
  leme_input_protocols_update_pointer_focus(server);
}

void leme_input_refresh_pointer_focus(struct leme_server *server,
                                      uint32_t time_msec) {
  wlr_seat_pointer_notify_clear_focus(server->seat);
  leme_input_process_motion(server, time_msec, NULL, true);
}

static void leme_input_handle_motion(struct wl_listener *listener, void *data) {
  struct leme_server *server = wl_container_of(listener, server, cursor_motion);
  struct wlr_pointer_motion_event *event = data;

  double dx = event->delta_x;
  double dy = event->delta_y;

  leme_session_notify_activity(server);
  if (leme_input_pointer_grab_active(server)) {
    wlr_cursor_move(server->cursor, &event->pointer->base, dx, dy);
    wlr_seat_pointer_notify_motion(server->seat, event->time_msec,
                                   server->cursor->x, server->cursor->y);
    return;
  }
  leme_input_protocols_send_relative(server, event->time_msec, dx, dy,
                                     event->unaccel_dx, event->unaccel_dy);
  if (leme_input_protocols_constrain_motion(server, &dx, &dy)) {
    return;
  }
  wlr_cursor_move(server->cursor, &event->pointer->base, dx, dy);
  leme_data_update_drag_icon(server);
  leme_desk_drag_motion(server);
  leme_input_process_motion(server, event->time_msec, NULL, true);
}

static void leme_input_handle_motion_absolute(struct wl_listener *listener,
                                              void *data) {
  struct leme_server *server =
      wl_container_of(listener, server, cursor_motion_absolute);
  struct wlr_pointer_motion_absolute_event *event = data;
  double layout_x;
  double layout_y;
  double dx;
  double dy;

  leme_session_notify_activity(server);
  wlr_cursor_absolute_to_layout_coords(server->cursor, &event->pointer->base,
                                       event->x, event->y, &layout_x,
                                       &layout_y);
  dx = layout_x - server->cursor->x;
  dy = layout_y - server->cursor->y;
  if (leme_input_pointer_grab_active(server)) {
    wlr_cursor_move(server->cursor, &event->pointer->base, dx, dy);
    wlr_seat_pointer_notify_motion(server->seat, event->time_msec,
                                   server->cursor->x, server->cursor->y);
    return;
  }
  if (leme_input_protocols_constrain_motion(server, &dx, &dy)) {
    return;
  }
  wlr_cursor_move(server->cursor, &event->pointer->base, dx, dy);
  leme_data_update_drag_icon(server);
  leme_desk_drag_motion(server);
  leme_input_process_motion(server, event->time_msec, NULL, true);
}

static uint32_t leme_input_pointer_wlr_edges(enum leme_grab_edge edges) {
  uint32_t result = WLR_EDGE_NONE;

  if ((edges & LEME_GRAB_EDGE_TOP) != 0) {
    result |= WLR_EDGE_TOP;
  }
  if ((edges & LEME_GRAB_EDGE_BOTTOM) != 0) {
    result |= WLR_EDGE_BOTTOM;
  }
  if ((edges & LEME_GRAB_EDGE_LEFT) != 0) {
    result |= WLR_EDGE_LEFT;
  }
  if ((edges & LEME_GRAB_EDGE_RIGHT) != 0) {
    result |= WLR_EDGE_RIGHT;
  }
  return result;
}

static bool leme_input_pointer_try_compositor_grab(
    struct leme_server *server, const struct leme_render_hit *hit,
    const struct wlr_pointer_button_event *event, bool inhibited) {
  uint32_t modifiers = leme_input_modifiers(server);
  uint32_t edges = WLR_EDGE_NONE;

  if (event->state != WL_POINTER_BUTTON_STATE_PRESSED || modifiers == 0 ||
      inhibited || leme_session_locked(server) || server->seat->drag != NULL ||
      hit->view == NULL ||
      (event->button != BTN_LEFT && event->button != BTN_RIGHT)) {
    return false;
  }
  const struct leme_tag *tag = leme_ownership_tag(hit->view);
  const uint32_t group =
      server->config == NULL ? 0 : server->config->desktop_container_modifier;
  if (modifiers != WLR_MODIFIER_LOGO &&
      (event->button != BTN_LEFT || tag == NULL ||
       tag->layout.kind != LEME_LAYOUT_DESKTOP ||
       modifiers != (WLR_MODIFIER_LOGO | group))) {
    return false;
  }
  if (event->button == BTN_LEFT && hit->view->container != NULL) {
    return leme_input_pointer_start_detach(hit->view, event->button);
  }
  if (event->button == BTN_RIGHT) {
    int surface_x =
        leme_input_pointer_round_delta(server->cursor->x - hit->view->box.x);
    int surface_y =
        leme_input_pointer_round_delta(server->cursor->y - hit->view->box.y);

    edges = leme_input_pointer_wlr_edges(
        leme_layout_resize_edges(hit->view->box, surface_x, surface_y));
  }
  return leme_input_pointer_grab_start(hit->view, event->button == BTN_RIGHT,
                                       event->button, edges, true, false);
}

static bool leme_input_pointer_try_edge_resize(
    struct leme_server *server, const struct leme_render_hit *hit,
    const struct wlr_pointer_button_event *event) {
  if (event->state != WL_POINTER_BUTTON_STATE_PRESSED ||
      event->button != BTN_LEFT ||
      (hit->view == NULL && hit->container == NULL) ||
      hit->resize_edges == WLR_EDGE_NONE ||
      !leme_input_pointer_resize_allowed(server) ||
      leme_input_modifiers(server) != 0) {
    return false;
  }
  if (hit->container != NULL) {
    enum leme_grab_edge edges = LEME_GRAB_EDGE_NONE;
    return leme_input_pointer_edges(hit->resize_edges, &edges) &&
           leme_input_pointer_grab_start_container_resize(hit->container, edges,
                                                          event->button);
  }
  return leme_input_pointer_grab_start(hit->view, true, event->button,
                                       hit->resize_edges, true, true);
}

static void leme_input_handle_button(struct wl_listener *listener, void *data) {
  struct leme_server *server = wl_container_of(listener, server, cursor_button);
  struct wlr_pointer_button_event *event = data;
  struct leme_render_hit hit;
  bool inhibited;

  leme_session_notify_activity(server);
  if (leme_desk_owner_handle_button(server, event)) {
    return;
  }
  if (leme_input_pointer_grab_active(server)) {
    wlr_seat_pointer_notify_button(server->seat, event->time_msec,
                                   event->button, event->state);
    return;
  }
  inhibited = leme_input_protocols_shortcuts_inhibited(server);
  leme_input_process_motion(server, event->time_msec, &hit, true);
  if (event->state == WL_POINTER_BUTTON_STATE_PRESSED &&
      !leme_session_locked(server) && hit.container != NULL) {
    leme_view_focus(hit.container->focused);
  }
  if (event->state == WL_POINTER_BUTTON_STATE_PRESSED &&
      !leme_session_locked(server) && leme_view_policy_hover_focus(hit.view) &&
      leme_input_pointer_view_layout(server, hit.view) == LEME_LAYOUT_DESKTOP) {
    leme_view_focus(hit.view);
  }
  if (leme_input_pointer_try_compositor_grab(server, &hit, event, inhibited) ||
      leme_input_pointer_try_edge_resize(server, &hit, event)) {
    wlr_seat_pointer_notify_button(server->seat, event->time_msec,
                                   event->button, event->state);
    return;
  }
  if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
    if (leme_desk_buttons_handle_button(server, &hit, event) ||
        leme_desk_handle_button(server, &hit, event) ||
        leme_desk_strip_handle_button(server, &hit, event)) {
      return;
    }
    leme_layer_focus_on_button(hit.layer);
  }
  wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button,
                                 event->state);
}

static void leme_input_handle_axis(struct wl_listener *listener, void *data) {
  struct leme_server *server = wl_container_of(listener, server, cursor_axis);
  struct wlr_pointer_axis_event *event = data;

  leme_session_notify_activity(server);
  if (leme_desk_owner_handle_axis(server, event)) {
    return;
  }
  wlr_seat_pointer_notify_axis(
      server->seat, event->time_msec, event->orientation, event->delta,
      event->delta_discrete, event->source, event->relative_direction);
}

static void leme_input_handle_frame(struct wl_listener *listener, void *data) {
  struct leme_server *server = wl_container_of(listener, server, cursor_frame);

  (void)data;
  wlr_seat_pointer_notify_frame(server->seat);
}

static void leme_input_handle_set_cursor(struct wl_listener *listener,
                                         void *data) {
  struct leme_server *server =
      wl_container_of(listener, server, request_set_cursor);
  struct wlr_seat_pointer_request_set_cursor_event *event = data;

  if (!leme_input_pointer_grab_active(server) &&
      !leme_input_protocols_pointer_locked(server) &&
      leme_session_surface_allowed(
          server, server->seat->pointer_state.focused_surface) &&
      server->seat->pointer_state.focused_client == event->seat_client) {
    leme_desktop_cursor_surface_set(server, event->seat_client);
    wlr_cursor_set_surface(server->cursor, event->surface, event->hotspot_x,
                           event->hotspot_y);
  }
}

static bool leme_input_gesture_natural_scroll(struct leme_server *server,
                                              struct wlr_input_device *device) {
  if (device != NULL && wlr_input_device_is_libinput(device)) {
    struct libinput_device *libinput_device =
        wlr_libinput_get_device_handle(device);

    if (libinput_device != NULL &&
        libinput_device_config_scroll_has_natural_scroll(libinput_device)) {
      return libinput_device_config_scroll_get_natural_scroll_enabled(
                 libinput_device) != 0;
    }
  }
  return leme_input_pointer_settings(server->config,
                                     device != NULL ? device->name : NULL)
      .natural_scroll;
}

void leme_input_workspace_gesture_reset(struct leme_server *server) {
  if (server == NULL) {
    return;
  }
  if (!leme_gesture_accel_restore(&server->gesture.accel)) {
    wlr_log(WLR_ERROR, "%s",
            "leme: failed to restore gesture pointer acceleration");
  }
  server->gesture = (struct leme_workspace_gesture_state){0};
}

void leme_input_workspace_gesture_cancel(struct leme_server *server) {
  struct leme_output *output;
  bool was_engaged;

  if (server == NULL) {
    return;
  }
  const bool was_active = server->gesture.active;
  output = server->gesture.output;
  was_engaged = server->gesture.engaged;

  leme_input_workspace_gesture_reset(server);

  if (output != NULL && was_engaged && output->workspace_transition != NULL) {
    leme_render_workspace_transition_finish(output);
  }
  if (was_active && server->started && server->scene != NULL) {
    leme_view_refresh_tag_focus(server);
  }
}

static void leme_input_handle_swipe_begin(struct wl_listener *listener,
                                          void *data) {
  struct leme_server *server =
      wl_container_of(listener, server, cursor_swipe_begin);
  struct wlr_pointer_swipe_begin_event *event = data;
  struct leme_output *output;
  struct wlr_input_device *device;
  struct libinput_device *libinput_device = NULL;
  bool natural_scroll;
  uint16_t focused_id;
  double initial_pos;
  uint16_t ring[LEME_TAGS_RING_MAX];
  size_t ring_count = 0;
  size_t center_index;
  bool is_takeover;

  if (server == NULL || server->config == NULL || event == NULL) {
    return;
  }
  if (server->config->gestures.workspace_switch.fingers == 0 ||
      event->fingers != server->config->gestures.workspace_switch.fingers) {
    return;
  }

  const enum leme_workspace_gesture_mode mode =
      server->config->gestures.workspace_switch.mode;
  leme_input_workspace_gesture_cancel(server);

  if (leme_input_pointer_grab_active(server) || leme_session_locked(server)) {
    return;
  }
  output = leme_output_focused(server);
  if (output == NULL || leme_output_tags(output) == NULL ||
      output->wlr_output == NULL || !output->wlr_output->enabled) {
    return;
  }

  device = event->pointer != NULL ? &event->pointer->base : NULL;
  natural_scroll = leme_input_gesture_natural_scroll(server, device);
  focused_id = leme_output_tags(output)->focused_id;
  initial_pos = leme_tags_position(leme_output_tags(output));

  is_takeover = (output->workspace_transition != NULL &&
                 leme_render_workspace_transition_active(output));

  if (is_takeover && !leme_render_workspace_transition_presented_position(
                         output, &initial_pos)) {
    leme_render_workspace_transition_finish(output);
    is_takeover = false;
    initial_pos = leme_tags_position(leme_output_tags(output));
  }
  if (is_takeover) {
    size_t tr_count = 0;
    const uint16_t *tr_ring =
        leme_render_workspace_transition_ring(output, &tr_count);
    if (tr_ring != NULL && tr_count >= 2 && tr_count <= LEME_TAGS_RING_MAX) {
      memcpy(ring, tr_ring, tr_count * sizeof(uint16_t));
      ring_count = tr_count;
    }
  }
  if (ring_count == 0) {
    ring_count =
        leme_tags_ring(leme_output_tags(output), LEME_TAG_CHANGE_FORWARD, ring,
                       LEME_ARRAY_LENGTH(ring));
  }
  if (ring_count < 2 || ring_count > LEME_TAGS_RING_MAX) {
    return;
  }

  center_index = ring_count;
  for (size_t i = 0; i < ring_count; i++) {
    if (ring[i] == focused_id) {
      center_index = i;
      break;
    }
  }
  if (center_index >= ring_count) {
    return;
  }

  const double count = (double)ring_count;
  const double index = (double)center_index;
  double center_pos = index + count * round((initial_pos - index) / count);

  if (is_takeover && ((mode == LEME_WORKSPACE_GESTURE_SINGLE &&
                       fabs(initial_pos - center_pos) > 1.0) ||
                      !leme_render_workspace_transition_set_gesture_range(
                          output, mode == LEME_WORKSPACE_GESTURE_SINGLE,
                          center_pos - 1.0, center_pos + 1.0))) {
    leme_render_workspace_transition_finish(output);
    is_takeover = false;
    focused_id = leme_output_tags(output)->focused_id;
    initial_pos = leme_tags_position(leme_output_tags(output));
    ring_count =
        leme_tags_ring(leme_output_tags(output), LEME_TAG_CHANGE_FORWARD, ring,
                       LEME_ARRAY_LENGTH(ring));
    if (ring_count < 2 || ring_count > LEME_TAGS_RING_MAX) {
      return;
    }
    center_index = ring_count;
    for (size_t i = 0; i < ring_count; i++) {
      if (ring[i] == focused_id) {
        center_index = i;
        break;
      }
    }
    if (center_index >= ring_count) {
      return;
    }
    center_pos =
        (double)center_index +
        (double)ring_count *
            round((initial_pos - (double)center_index) / (double)ring_count);
  }

  leme_session_notify_activity(server);

  server->gesture.mode = mode;
  server->gesture.active = true;
  server->gesture.engaged = is_takeover;
  server->gesture.pointer = event->pointer;
  server->gesture.output = output;
  server->gesture.natural_scroll = natural_scroll;
  server->gesture.initial_position = initial_pos;
  server->gesture.center_position = center_pos;
  server->gesture.initial_tag_id = focused_id;
  server->gesture.displacement = 0.0;
  server->gesture.dx_accum = 0.0;
  server->gesture.dy_accum = 0.0;
  memcpy(server->gesture.ring, ring, ring_count * sizeof(uint16_t));
  server->gesture.ring_count = ring_count;

  leme_swipe_tracker_init(
      &server->gesture.tracker,
      server->config->gestures.workspace_switch.velocity_window_ms,
      server->config->gestures.workspace_switch.deceleration);

  if (device != NULL && wlr_input_device_is_libinput(device)) {
    libinput_device = wlr_libinput_get_device_handle(device);
  }
  if (libinput_device != NULL) {
    (void)leme_gesture_accel_begin(&server->gesture.accel, libinput_device);
  }

  if (is_takeover) {
    leme_animation_manager_cancel_data(&server->animations,
                                       output->workspace_transition);
    leme_render_workspace_transition_begin_gesture(
        output->workspace_transition);
    /* Bounds were adopted before acquiring input/animation ownership. */
  }
}

static void leme_input_handle_swipe_update(struct wl_listener *listener,
                                           void *data) {
  struct leme_server *server =
      wl_container_of(listener, server, cursor_swipe_update);
  struct wlr_pointer_swipe_update_event *event = data;
  struct leme_output *output;
  const struct leme_workspace_switch_gesture_settings *settings;
  double effective_dx;

  if (server == NULL || !server->gesture.active || event == NULL) {
    return;
  }
  if (server->gesture.pointer != NULL && event->pointer != NULL &&
      event->pointer != server->gesture.pointer) {
    return;
  }
  if (!isfinite(event->dx) || !isfinite(event->dy)) {
    leme_input_workspace_gesture_cancel(server);
    return;
  }

  leme_session_notify_activity(server);
  output = server->gesture.output;
  if (output == NULL || output != leme_output_focused(server) ||
      leme_output_tags(output) == NULL || leme_session_locked(server)) {
    leme_input_workspace_gesture_cancel(server);
    return;
  }

  settings = &server->config->gestures.workspace_switch;
  if (settings->distance <= 0.0 || !isfinite(settings->distance)) {
    leme_input_workspace_gesture_cancel(server);
    return;
  }

  effective_dx = server->gesture.natural_scroll ? -event->dx : event->dx;

  if (!server->gesture.engaged) {
    server->gesture.dx_accum += effective_dx;
    server->gesture.dy_accum += event->dy;

    if (!isfinite(server->gesture.dx_accum) ||
        !isfinite(server->gesture.dy_accum)) {
      leme_input_workspace_gesture_cancel(server);
      return;
    }

    const double dist =
        hypot(server->gesture.dx_accum, server->gesture.dy_accum);
    if (dist < 16.0) {
      return;
    }

    if (fabs(server->gesture.dy_accum) >= fabs(server->gesture.dx_accum)) {
      leme_input_workspace_gesture_reset(server);
      return;
    }

    leme_swipe_tracker_reset(&server->gesture.tracker);
    server->gesture.displacement = 0.0;

    const size_t adjacent_index = (size_t)leme_tags_ring_wrap(
        server->gesture.center_position + 1.0, server->gesture.ring_count);
    const uint16_t adj = server->gesture.ring[adjacent_index];
    struct leme_workspace_transition *transition =
        leme_render_workspace_transition_prepare_gesture(
            output, server->gesture.initial_tag_id, adj,
            LEME_TAG_CHANGE_FORWARD);

    if (transition != NULL) {
      leme_render_workspace_transition_begin_gesture(transition);

      size_t count = 0;
      const uint16_t *ring =
          leme_render_workspace_transition_ring(output, &count);
      bool ring_matches = (ring != NULL && count == server->gesture.ring_count);
      if (ring_matches) {
        for (size_t i = 0; i < count; i++) {
          if (ring[i] != server->gesture.ring[i]) {
            ring_matches = false;
            break;
          }
        }
      }
      if (!ring_matches) {
        leme_render_workspace_transition_finish(output);
        leme_input_workspace_gesture_cancel(server);
        return;
      }

      if (!leme_render_workspace_transition_set_gesture_range(
              output, server->gesture.mode == LEME_WORKSPACE_GESTURE_SINGLE,
              server->gesture.center_position - 1.0,
              server->gesture.center_position + 1.0)) {
        leme_render_workspace_transition_finish(output);
        leme_input_workspace_gesture_cancel(server);
        return;
      }
    }

    server->gesture.engaged = true;
  }

  const double delta = effective_dx / settings->distance;
  const double next_displacement = server->gesture.displacement + delta;
  const double raw = server->gesture.initial_position + next_displacement;

  if (!isfinite(delta) || !isfinite(next_displacement) || !isfinite(raw) ||
      (server->gesture.mode != LEME_WORKSPACE_GESTURE_SINGLE &&
       fabs(raw) > 0x1p52 - 2.0)) {
    leme_input_workspace_gesture_cancel(server);
    return;
  }

  server->gesture.displacement = next_displacement;
  leme_swipe_tracker_push(&server->gesture.tracker, delta, event->time_msec);

  const double visual =
      server->gesture.mode == LEME_WORKSPACE_GESTURE_SINGLE
          ? leme_swipe_position(raw, server->gesture.center_position)
          : raw;
  if (output->workspace_transition != NULL) {
    leme_render_workspace_transition_set_position(output, visual);
  }
}

static void leme_input_handle_swipe_end(struct wl_listener *listener,
                                        void *data) {
  struct leme_server *server =
      wl_container_of(listener, server, cursor_swipe_end);
  struct wlr_pointer_swipe_end_event *event = data;
  struct leme_output *output;
  const struct leme_workspace_switch_gesture_settings *settings;
  uint16_t target_id;
  size_t target_idx;
  enum leme_tag_change_direction change_dir;

  if (server == NULL || !server->gesture.active || event == NULL) {
    return;
  }
  if (server->gesture.pointer != NULL && event->pointer != NULL &&
      event->pointer != server->gesture.pointer) {
    return;
  }
  leme_session_notify_activity(server);
  output = server->gesture.output;
  if (output == NULL || output != leme_output_focused(server) ||
      leme_output_tags(output) == NULL || leme_session_locked(server)) {
    leme_input_workspace_gesture_cancel(server);
    return;
  }

  if (!server->gesture.engaged) {
    leme_input_workspace_gesture_cancel(server);
    return;
  }

  settings = &server->config->gestures.workspace_switch;
  leme_swipe_tracker_push(&server->gesture.tracker, 0.0, event->time_msec);

  const double raw =
      server->gesture.initial_position + server->gesture.displacement;
  const bool single = server->gesture.mode == LEME_WORKSPACE_GESTURE_SINGLE;
  double visual =
      single ? leme_swipe_position(raw, server->gesture.center_position) : raw;
  if (output->workspace_transition != NULL &&
      !leme_render_workspace_transition_presented_position(output, &visual)) {
    leme_input_workspace_gesture_cancel(server);
    return;
  }
  const double projection = leme_swipe_tracker_projected_position(
      &server->gesture.tracker, raw, event->time_msec);
  const double velocity =
      event->cancelled ? 0.0
                       : leme_swipe_tracker_velocity(&server->gesture.tracker,
                                                     event->time_msec) *
                             (single ? leme_swipe_position_derivative(
                                           raw, server->gesture.center_position)
                                     : 1.0);
  const bool forward = projection >= server->gesture.initial_position;
  bool bounded = single;
  double minimum = server->gesture.center_position - 1.0;
  double maximum = server->gesture.center_position + 1.0;
  double target = NAN;
  if (event->cancelled) {
    target = server->gesture.center_position;
    if (!single && server->gesture.ring_count >= 2) {
      const double count = (double)server->gesture.ring_count;
      target += count * round((visual - target) / count);
    }
  } else {
    switch (server->gesture.mode) {
    case LEME_WORKSPACE_GESTURE_SINGLE:
      target = leme_swipe_target(projection, server->gesture.center_position,
                                 settings->threshold);
      break;
    case LEME_WORKSPACE_GESTURE_SCRUB:
      bounded = true;
      minimum = floor(visual);
      maximum = ceil(visual);
      target =
          leme_swipe_continuous_target(fmax(minimum, fmin(projection, maximum)),
                                       settings->threshold, forward);
      break;
    case LEME_WORKSPACE_GESTURE_FREE:
      target = leme_swipe_continuous_target(projection, settings->threshold,
                                            forward);
      break;
    }
  }

  if (!isfinite(visual) || !isfinite(projection) || !isfinite(target) ||
      !isfinite(velocity)) {
    leme_input_workspace_gesture_cancel(server);
    return;
  }

  if (server->gesture.ring_count < 2) {
    leme_input_workspace_gesture_cancel(server);
    return;
  }

  target_idx = (size_t)leme_tags_ring_wrap(target, server->gesture.ring_count);
  if (target_idx >= server->gesture.ring_count) {
    leme_input_workspace_gesture_cancel(server);
    return;
  }
  target_id = server->gesture.ring[target_idx];

  if (output->workspace_transition != NULL &&
      !leme_render_workspace_transition_set_gesture_range(output, bounded,
                                                          minimum, maximum)) {
    leme_input_workspace_gesture_cancel(server);
    return;
  }

  if (target > server->gesture.center_position) {
    change_dir = LEME_TAG_CHANGE_FORWARD;
  } else if (target < server->gesture.center_position) {
    change_dir = LEME_TAG_CHANGE_BACKWARD;
  } else {
    change_dir = (visual >= server->gesture.center_position)
                     ? LEME_TAG_CHANGE_BACKWARD
                     : LEME_TAG_CHANGE_FORWARD;
  }

  (void)leme_tags_focus_id_direction(leme_output_tags(output), target_id,
                                     change_dir);

  struct leme_workspace_transition *transition = output->workspace_transition;
  leme_input_workspace_gesture_reset(server);
  leme_view_refresh_tag_focus(server);

  if (transition != NULL) {
    leme_render_workspace_transition_settle(output, visual, target, velocity);
  }
}

void leme_input_pointer_events_init(struct leme_server *server) {
  struct leme_pointer_grab *grab = calloc(1, sizeof(*grab));

  if (grab == NULL) {
    wlr_log(WLR_ERROR, "%s", "leme: failed to allocate pointer grab");
  } else {
    grab->server = server;
    grab->seat_grab.interface = &leme_pointer_grab_interface;
    grab->seat_grab.data = grab;
    server->pointer_grab = grab;
  }
  server->cursor_motion.notify = leme_input_handle_motion;
  wl_signal_add(&server->cursor->events.motion, &server->cursor_motion);
  server->cursor_motion_absolute.notify = leme_input_handle_motion_absolute;
  wl_signal_add(&server->cursor->events.motion_absolute,
                &server->cursor_motion_absolute);
  server->cursor_button.notify = leme_input_handle_button;
  wl_signal_add(&server->cursor->events.button, &server->cursor_button);
  server->cursor_axis.notify = leme_input_handle_axis;
  wl_signal_add(&server->cursor->events.axis, &server->cursor_axis);
  server->cursor_frame.notify = leme_input_handle_frame;
  wl_signal_add(&server->cursor->events.frame, &server->cursor_frame);
  server->cursor_swipe_begin.notify = leme_input_handle_swipe_begin;
  wl_signal_add(&server->cursor->events.swipe_begin,
                &server->cursor_swipe_begin);
  server->cursor_swipe_update.notify = leme_input_handle_swipe_update;
  wl_signal_add(&server->cursor->events.swipe_update,
                &server->cursor_swipe_update);
  server->cursor_swipe_end.notify = leme_input_handle_swipe_end;
  wl_signal_add(&server->cursor->events.swipe_end, &server->cursor_swipe_end);
  server->request_set_cursor.notify = leme_input_handle_set_cursor;
  wl_signal_add(&server->seat->events.request_set_cursor,
                &server->request_set_cursor);
}

void leme_input_pointers_finish(struct leme_server *server) {
  leme_public_server_invalidate(server);
  struct leme_pointer *pointer;
  struct leme_pointer *temporary;

  leme_input_workspace_gesture_cancel(server);
  leme_input_pointer_grab_finish(server);
  free(server->pointer_grab);
  server->pointer_grab = NULL;
  wl_list_for_each_safe(pointer, temporary, &server->pointers, link) {
    wlr_cursor_detach_input_device(server->cursor, pointer->device);
    wl_list_remove(&pointer->destroy.link);
    wl_list_remove(&pointer->link);
    free(pointer);
  }
  wl_list_remove(&server->cursor_motion.link);
  wl_list_remove(&server->cursor_motion_absolute.link);
  wl_list_remove(&server->cursor_button.link);
  wl_list_remove(&server->cursor_axis.link);
  wl_list_remove(&server->cursor_frame.link);
  wl_list_remove(&server->cursor_swipe_begin.link);
  wl_list_remove(&server->cursor_swipe_update.link);
  wl_list_remove(&server->cursor_swipe_end.link);
  wl_list_remove(&server->request_set_cursor.link);
}

bool leme_input_pointer_resolve_id(const struct leme_server *server,
                                   struct leme_public_id id,
                                   struct leme_input_target_info *info) {
  if (server == NULL || info == NULL || server->pointers.next == NULL) {
    return false;
  }
  const struct leme_pointer *pointer = NULL;
  wl_list_for_each(pointer, &server->pointers, link) {
    if (pointer->public_id.serial == id.serial) {
      info->exists = true;
      info->is_keyboard = false;
      info->is_libinput = wlr_input_device_is_libinput(pointer->device);
      if (info->is_libinput) {
        struct libinput_device *dev =
            wlr_libinput_get_device_handle(pointer->device);
        if (dev != NULL) {
          const uint32_t profiles =
              libinput_device_config_accel_get_profiles(dev);
          info->adaptive =
              (profiles & LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE) != 0;
          info->flat = (profiles & LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT) != 0;
          info->current_accel_profile =
              libinput_device_config_accel_get_profile(dev);
          info->has_accel = libinput_device_config_accel_is_available(dev) != 0;
          if (info->has_accel) {
            info->current_accel_speed =
                libinput_device_config_accel_get_speed(dev);
          }
          info->has_natural_scroll =
              libinput_device_config_scroll_has_natural_scroll(dev) != 0;
          if (info->has_natural_scroll) {
            info->current_natural_scroll =
                libinput_device_config_scroll_get_natural_scroll_enabled(dev) !=
                0;
          }
          info->has_left_handed =
              libinput_device_config_left_handed_is_available(dev) != 0;
          if (info->has_left_handed) {
            info->current_left_handed =
                libinput_device_config_left_handed_get(dev) != 0;
          }
          info->has_tap = libinput_device_config_tap_get_finger_count(dev) > 0;
          if (info->has_tap) {
            info->current_tap = libinput_device_config_tap_get_enabled(dev) ==
                                LIBINPUT_CONFIG_TAP_ENABLED;
          }
        }
      }
      return true;
    }
  }
  return false;
}

enum libinput_config_status
leme_input_apply_pointer_setting(struct leme_server *server,
                                 struct leme_public_id id,
                                 const struct leme_input_setting_val *val) {
  if (server == NULL || val == NULL || server->pointers.next == NULL) {
    return LIBINPUT_CONFIG_STATUS_UNSUPPORTED;
  }
  struct leme_pointer *pointer = NULL;
  wl_list_for_each(pointer, &server->pointers, link) {
    if (pointer->public_id.serial == id.serial) {
      if (!wlr_input_device_is_libinput(pointer->device)) {
        return LIBINPUT_CONFIG_STATUS_UNSUPPORTED;
      }
      struct libinput_device *dev =
          wlr_libinput_get_device_handle(pointer->device);
      if (dev == NULL) {
        return LIBINPUT_CONFIG_STATUS_UNSUPPORTED;
      }
      switch (val->key) {
      case LEME_INPUT_SETTING_ACCEL_PROFILE:
        return libinput_device_config_accel_set_profile(dev, val->profile);
      case LEME_INPUT_SETTING_ACCEL_SPEED:
        return libinput_device_config_accel_set_speed(dev, val->speed);
      case LEME_INPUT_SETTING_NATURAL_SCROLL:
        return libinput_device_config_scroll_set_natural_scroll_enabled(
            dev, val->boolean ? 1 : 0);
      case LEME_INPUT_SETTING_LEFT_HANDED:
        return libinput_device_config_left_handed_set(dev,
                                                      val->boolean ? 1 : 0);
      case LEME_INPUT_SETTING_TAP:
        return libinput_device_config_tap_set_enabled(
            dev, val->boolean ? LIBINPUT_CONFIG_TAP_ENABLED
                              : LIBINPUT_CONFIG_TAP_DISABLED);
      default:
        return LIBINPUT_CONFIG_STATUS_UNSUPPORTED;
      }
    }
  }
  return LIBINPUT_CONFIG_STATUS_UNSUPPORTED;
}

bool leme_input_resolve_target(const struct leme_server *server,
                               struct leme_public_id id,
                               struct leme_input_target_info *info) {
  if (server == NULL || info == NULL) {
    return false;
  }
  if (leme_input_pointer_resolve_id(server, id, info)) {
    return true;
  }
  if (leme_input_keyboard_resolve_id(server, id, info)) {
    return true;
  }
  return false;
}
