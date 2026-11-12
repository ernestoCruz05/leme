#include "input/pointer_internal.h"
#include "input/input.h"
#include "input/internal.h"

#include "config/config.h"
#include "config/style.h"
#include "core/server.h"
#include "desktop/desktop.h"
#include "desktop/buttons.h"
#include "desktop/decoration.h"
#include "output/output.h"
#include "protocols/desktop.h"
#include "protocols/input.h"
#include "protocols/session.h"
#include "render/render.h"
#include "render/workspace_transition.h"
#include "shell/layer.h"
#include "shell/policy.h"
#include "shell/view.h"
#include "shell/container.h"
#include "workspace/tag.h"

#include <limits.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdint.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>

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

static enum leme_drop_mode
leme_input_pointer_drop_mode(const struct leme_view *target) {
  const struct leme_tag *tag = leme_ownership_tag(target);
  struct leme_tag_settings settings = {.drop_mode = LEME_DROP_MODE_SIMPLE};

  if (tag != NULL && target->server->config != NULL) {
    leme_config_tag_settings(target->server->config, tag->id, &settings);
  }
  return settings.drop_mode;
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
    const struct leme_layout_limits limits =
        grab->view == NULL
            ? (struct leme_layout_limits){0}
            : (struct leme_layout_limits){
                  .min_width =
                      leme_view_min_length(grab->view, LEME_SPLIT_HORIZONTAL),
                  .min_height =
                      leme_view_min_length(grab->view, LEME_SPLIT_VERTICAL),
                  .max_width =
                      leme_view_max_length(grab->view, LEME_SPLIT_HORIZONTAL),
                  .max_height =
                      leme_view_max_length(grab->view, LEME_SPLIT_VERTICAL),
              };
    box = leme_layout_resize_box(grab->saved_box, grab->edges, dx, dy, limits);
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
        !leme_layout_drop_at(leme_input_pointer_drop_mode(target),
                             leme_render_view_content_box(target, target->box),
                             grab->server->cursor->x, grab->server->cursor->y,
                             &drop)) {
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

static void leme_input_pointer_grab_release(struct leme_pointer_grab *grab) {
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
      (void)leme_container_move_to_output(moving_container, destination, true);
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

static uint32_t leme_input_pointer_grab_button(
    struct wlr_seat_pointer_grab *seat_grab,
    uint32_t time_msec, // NOLINT(bugprone-easily-swappable-parameters)
    uint32_t button, enum wl_pointer_button_state state) {
  struct leme_pointer_grab *grab = seat_grab->data;

  (void)time_msec;
  if (button == grab->button && state == WL_POINTER_BUTTON_STATE_RELEASED &&
      !leme_input_pointer_button_held(grab->server->seat, button)) {
    leme_input_pointer_grab_release(grab);
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

const struct wlr_pointer_grab_interface leme_pointer_grab_interface = {
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
                                               uint32_t button,
                                               bool requested) {
  if (container == NULL || !leme_container_action_allowed(container->focused)) {
    return false;
  }
  struct leme_server *server = container->tag->owner->server;
  struct leme_pointer_grab *grab = server->pointer_grab;
  const bool resize = edges != LEME_GRAB_EDGE_NONE;
  enum leme_grab_edge checked = LEME_GRAB_EDGE_NONE;
  if (grab == NULL || server->seat == NULL || server->cursor == NULL ||
      (resize &&
       (!leme_input_pointer_edges(leme_input_pointer_wlr_edges(edges),
                                  &checked) ||
        checked != edges ||
        (!requested && leme_render_container_resize_edges(
                           container, server->cursor->x, server->cursor->y) !=
                           leme_input_pointer_wlr_edges(edges)))) ||
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
                                     leme_view_min_length, &drag)) {
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
                                            button, false);
}

static bool
leme_input_pointer_grab_start_container_resize(struct leme_container *container,
                                               enum leme_grab_edge edges,
                                               uint32_t button) {
  return edges != LEME_GRAB_EDGE_NONE &&
         leme_input_pointer_container_start(container, edges, button, false);
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
                                       view, edges, leme_view_min_length,
                                       &grab->resize_drag)) {
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

bool leme_input_pointer_grab_start_surface(
    struct leme_view *view, struct wlr_surface *origin, bool resize,
    uint32_t serial, // NOLINT(bugprone-easily-swappable-parameters)
    uint32_t wlr_edges) {
  if (view == NULL || view->server == NULL || origin == NULL) {
    return false;
  }
  struct wlr_seat *seat = view->server->seat;
  const uint32_t button = seat->pointer_state.grab_button;
  if (seat->pointer_state.focused_surface != origin ||
      !leme_input_pointer_button_held(seat, button) ||
      !wlr_seat_validate_pointer_grab_serial(seat, origin, serial)) {
    return false;
  }
  return leme_input_pointer_grab_start(view, resize, button, wlr_edges, false,
                                       false);
}

bool leme_input_pointer_grab_start_container_surface(
    struct leme_container *container, struct wlr_surface *origin, bool resize,
    uint32_t serial, // NOLINT(bugprone-easily-swappable-parameters)
    uint32_t wlr_edges) {
  if (container == NULL || origin == NULL) {
    return false;
  }
  struct wlr_seat *seat = container->tag->owner->server->seat;
  const uint32_t button = seat->pointer_state.grab_button;
  enum leme_grab_edge edges = LEME_GRAB_EDGE_NONE;
  if (seat->pointer_state.focused_surface != origin ||
      !leme_input_pointer_button_held(seat, button) ||
      !wlr_seat_validate_pointer_grab_serial(seat, origin, serial) ||
      (resize && (!leme_input_pointer_edges(wlr_edges, &edges) ||
                  edges == LEME_GRAB_EDGE_NONE))) {
    return false;
  }
  return leme_input_pointer_container_start(container, edges, button, true);
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

bool leme_input_pointer_grab_abort(struct leme_server *server) {
  struct leme_pointer_grab *grab;

  if (!leme_input_pointer_grab_active(server)) {
    return false;
  }
  grab = server->pointer_grab;
  if (grab->mode == LEME_POINTER_GRAB_NONE ||
      grab->mode == LEME_POINTER_GRAB_SINK) {
    return false;
  }
  if ((grab->mode == LEME_POINTER_GRAB_FLOAT_MOVE ||
       grab->mode == LEME_POINTER_GRAB_FLOAT_RESIZE) &&
      grab->view != NULL) {
    (void)leme_view_apply_interactive_box(grab->view, grab->saved_box, false);
  } else if (grab->mode == LEME_POINTER_GRAB_TILED_RESIZE &&
             leme_layout_resize_drag_update(&grab->resize_drag, 0, 0)) {
    leme_view_arrange(server);
  }
  leme_input_pointer_grab_cancel(server);
  return true;
}

void leme_input_pointer_grab_cancel_tiled(struct leme_server *server) {
  if (leme_input_pointer_grab_active(server) &&
      leme_input_pointer_grab_mode_tiled(server->pointer_grab->mode)) {
    leme_input_pointer_grab_cancel(server);
  }
}

void leme_input_pointer_grab_cancel_tiled_on(struct leme_server *server,
                                             const struct leme_tags *tags,
                                             uint16_t tag_id) {
  const struct leme_tag *tag;

  if (!leme_input_pointer_grab_active(server) ||
      !leme_input_pointer_grab_mode_tiled(server->pointer_grab->mode) ||
      server->pointer_grab->view == NULL) {
    return;
  }
  tag = leme_ownership_tag(server->pointer_grab->view);
  if (tag == NULL || (tag->owner == tags && tag->id == tag_id)) {
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

static bool leme_input_pointer_resize_allowed(struct leme_server *server) {
  return server->seat != NULL && server->cursor != NULL &&
         leme_input_idle(server) &&
         !leme_input_protocols_shortcuts_inhibited(server) &&
         server->seat->pointer_state.button_count == 0 &&
         server->desk_owner.owner == NULL;
}

void leme_input_pointer_resize_hover(struct leme_server *server,
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

static bool leme_input_pointer_start_compositor_grab(struct leme_view *view,
                                                     bool resize,
                                                     uint32_t button) {
  uint32_t edges = WLR_EDGE_NONE;

  if (!resize && view->container != NULL) {
    return leme_input_pointer_start_detach(view, button);
  }
  if (resize) {
    const struct wlr_cursor *cursor = view->server->cursor;
    int surface_x = leme_input_pointer_round_delta(cursor->x - view->box.x);
    int surface_y = leme_input_pointer_round_delta(cursor->y - view->box.y);

    edges = leme_input_pointer_wlr_edges(
        leme_layout_resize_edges(view->box, surface_x, surface_y));
  }
  return leme_input_pointer_grab_start(view, resize, button, edges, true,
                                       false);
}

bool leme_input_pointer_gesture_grab_begin(struct leme_server *server,
                                           bool resize) {
  struct leme_render_hit hit = {0};

  if (!leme_render_at(server, server->cursor->x, server->cursor->y, &hit) ||
      hit.view == NULL) {
    return false;
  }
  return leme_input_pointer_start_compositor_grab(hit.view, resize, 0);
}

void leme_input_pointer_gesture_grab_motion(struct leme_server *server,
                                            uint32_t time_msec, double dx,
                                            double dy) {
  if (!leme_input_pointer_grab_active(server)) {
    return;
  }
  wlr_cursor_move(server->cursor, NULL, dx, dy);
  wlr_seat_pointer_notify_motion(server->seat, time_msec, server->cursor->x,
                                 server->cursor->y);
}

void leme_input_pointer_gesture_grab_end(struct leme_server *server,
                                         bool cancelled) {
  if (!leme_input_pointer_grab_active(server)) {
    return;
  }
  if (cancelled) {
    (void)leme_input_pointer_grab_abort(server);
    return;
  }
  leme_input_pointer_grab_release(server->pointer_grab);
}

bool leme_input_pointer_try_compositor_grab(
    struct leme_server *server, const struct leme_render_hit *hit,
    const struct wlr_pointer_button_event *event, bool inhibited) {
  uint32_t modifiers = leme_input_modifiers(server);

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
  return leme_input_pointer_start_compositor_grab(
      hit->view, event->button == BTN_RIGHT, event->button);
}

bool leme_input_pointer_try_edge_resize(
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
