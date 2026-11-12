#include "shell/container.h"

#include "config/style.h"
#include "core/server.h"
#include "desktop/buttons.h"
#include "desktop/decoration.h"
#include "desktop/strip.h"
#include "input/input.h"
#include "output/output.h"
#include "protocols/publication.h"
#include "protocols/input.h"
#include "protocols/session.h"
#include "shell/layer.h"
#include "shell/policy.h"
#include "shell/ownership_internal.h"
#include "protocols/capture.h"
#include "protocols/decoration.h"
#include "public/server.h"
#include "render/render.h"
#include "render/workspace_transition.h"
#include "shell/container-policy.h"
#include "shell/view.h"
#include "workspace/tag.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_xdg_dialog_v1.h>
#include <wlr/xwayland/xwayland.h>

static struct leme_server *
leme_container_server(const struct leme_container *container) {
  return container->tag->owner->server;
}

struct leme_container *leme_container_for_view(const struct leme_view *view) {
  if (view == NULL || view->server == NULL ||
      view->server->views.next == NULL) {
    return NULL;
  }
  const struct leme_tag *tag = leme_ownership_tag(view);
  size_t limit = (size_t)wl_list_length(&view->server->views);
  while (view != NULL && leme_ownership_tag(view) == tag && limit > 0) {
    if (view->container != NULL) {
      return view->container;
    }
    view = leme_view_parent(view);
    limit--;
  }
  return NULL;
}

void leme_container_focus_view(struct leme_view *view) {
  struct leme_container *container = leme_container_for_view(view);
  if (container == NULL) {
    return;
  }
  while (view != NULL && view->container != container) {
    view = leme_view_parent(view);
  }
  if (view != NULL) {
    container->focused = view;
    leme_render_container_set_box(container, container->box);
  }
}

static int leme_container_border(const struct leme_container *container) {
  const struct leme_server *server = leme_container_server(container);
  return server->config == NULL ? 0 : server->config->border_width;
}

static struct leme_gaps
leme_container_shell_frame(const struct leme_container *container) {
  const int border =
      leme_decoration_protocol_container_replaces_border(container)
          ? 0
          : leme_container_border(container);
  const struct leme_gaps insets =
      leme_decoration_protocol_container_insets(container);
  return (struct leme_gaps){
      .top = insets.top + border,
      .right = insets.right + border,
      .bottom = insets.bottom + border,
      .left = insets.left + border,
  };
}

static struct leme_box leme_container_resize_box(struct leme_box box,
                                                 struct leme_gaps by,
                                                 int64_t sign) {
  const int64_t x = (int64_t)box.x + sign * by.left;
  const int64_t y = (int64_t)box.y + sign * by.top;
  const int64_t width =
      (int64_t)box.width - sign * ((int64_t)by.left + by.right);
  const int64_t height =
      (int64_t)box.height - sign * ((int64_t)by.top + by.bottom);
  if (x < INT_MIN || y < INT_MIN || x > INT_MAX || y > INT_MAX || width <= 0 ||
      width > INT_MAX || height <= 0 || height > INT_MAX ||
      x + width > INT_MAX || y + height > INT_MAX) {
    return (struct leme_box){0};
  }
  return (struct leme_box){(int)x, (int)y, (int)width, (int)height};
}

struct leme_box
leme_container_content_box(const struct leme_container *container,
                           struct leme_box frame) {
  struct leme_desk_decoration_boxes boxes = {0};
  if (container == NULL) {
    return (struct leme_box){0};
  }
  if (leme_decoration_protocol_container_decorated(container)) {
    return leme_container_resize_box(frame,
                                     leme_container_shell_frame(container), 1);
  }
  const struct leme_config *config = leme_container_server(container)->config;
  if (!leme_desk_decoration_geometry(frame, leme_container_border(container),
                                     leme_desk_style_titlebar_height(config),
                                     leme_desk_style_button_width(config),
                                     &boxes)) {
    return (struct leme_box){0};
  }
  return boxes.content;
}

static struct leme_box
leme_container_frame_box(const struct leme_container *container,
                         struct leme_box content) {
  if (leme_decoration_protocol_container_decorated(container)) {
    return leme_container_resize_box(content,
                                     leme_container_shell_frame(container), -1);
  }
  const int border = leme_container_border(container);
  const int titlebar =
      leme_desk_style_titlebar_height(leme_container_server(container)->config);
  const int64_t x = (int64_t)content.x - border;
  const int64_t y = (int64_t)content.y - border - titlebar;
  const int64_t width = (int64_t)content.width + 2 * (int64_t)border;
  const int64_t height =
      (int64_t)content.height + 2 * (int64_t)border + titlebar;
  if (x < INT_MIN || y < INT_MIN || width <= 0 || width > INT_MAX ||
      height <= 0 || height > INT_MAX || x + width > INT_MAX ||
      y + height > INT_MAX) {
    return (struct leme_box){0};
  }
  return (struct leme_box){(int)x, (int)y, (int)width, (int)height};
}

static struct leme_box
leme_container_normal_box(const struct leme_container *container) {
  if (container == NULL) {
    return (struct leme_box){0};
  }
  return container->placement.restore_valid
             ? leme_container_frame_box(container,
                                        container->placement.restore_box)
             : container->box;
}

static bool
leme_container_member_protocol_eligible(const struct leme_view *view) {
  if (view->kind == LEME_VIEW_XDG && view->xdg_toplevel != NULL) {
    if (view->xdg_toplevel->parent != NULL ||
        (view->xdg_toplevel->base != NULL &&
         wlr_xdg_dialog_v1_try_from_wlr_xdg_toplevel(view->xdg_toplevel) !=
             NULL)) {
      return false;
    }
  }
  if (view->kind == LEME_VIEW_XWAYLAND && view->xwayland_surface != NULL) {
    struct wlr_xwayland_surface *surface = view->xwayland_surface;
    if (surface->override_redirect || surface->parent != NULL ||
        surface->modal ||
        (surface->xwm != NULL &&
         wlr_xwayland_surface_has_window_type(
             surface, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DIALOG))) {
      return false;
    }
  }
  return true;
}

bool leme_container_member_eligible(const struct leme_view *view) {
  const struct leme_tag *tag = leme_ownership_tag(view);
  if (view == NULL || view->server == NULL || !view->mapped ||
      view->unmanaged || view->detached || view->fullscreen ||
      leme_view_is_minimized(view) || tag == NULL || tag->owner == NULL ||
      tag->owner->output == NULL || tag->owner->output->wlr_output == NULL ||
      !tag->owner->output->wlr_output->enabled ||
      tag->layout.kind != LEME_LAYOUT_DESKTOP ||
      tag->owner->focused_is_candidate || tag->owner->focused_id != tag->id ||
      !leme_container_member_protocol_eligible(view)) {
    return false;
  }
  return leme_tag_fullscreen_view(tag) == NULL;
}

static bool leme_container_input_allowed(const struct leme_view *view) {
  return leme_container_member_eligible(view) &&
         leme_input_idle(view->server) &&
         !leme_input_protocols_shortcuts_inhibited(view->server);
}

bool leme_container_action_allowed(const struct leme_view *view) {
  return leme_container_input_allowed(view) && view->container != NULL;
}

bool leme_container_detach_current(struct leme_view *view) {
  if (!leme_container_action_allowed(view)) {
    return false;
  }
  leme_container_remove_view(view);
  return leme_view_apply_interactive_box(view, view->box, true);
}

void leme_container_reconcile_view(struct leme_view *view) {
  if (view != NULL && view->container != NULL &&
      !leme_container_member_protocol_eligible(view)) {
    leme_container_remove_view(view);
  }
}

static void leme_container_invalidate(struct leme_container *container) {
  leme_public_server_invalidate(leme_container_server(container));
  leme_publication_invalidate(leme_container_server(container));
}

static bool
leme_container_geometry_available(const struct leme_container *container) {
  return container != NULL && container->count >= 2 && !container->minimized &&
         leme_container_member_eligible(container->focused);
}

bool leme_container_can_place(const struct leme_container *container,
                              enum leme_view_placement kind) {
  if (!leme_container_geometry_available(container)) {
    return false;
  }
  const struct leme_box box = leme_placement_box(
      leme_placement_area(container->tag, container->tag->owner->output), kind);
  return leme_container_content_box(container, box).width > 0;
}

static void leme_container_geometry_changed(struct leme_container *container,
                                            bool animate) {
  container->generation++;
  leme_container_arrange(container, animate);
  for (size_t i = 0; i < container->count; i++) {
    leme_view_sync_wm_state(container->views[i]);
  }
  leme_container_invalidate(container);
  leme_capture_sync(leme_container_server(container));
}

bool leme_container_set_layout(struct leme_container *container,
                               enum leme_layout_kind kind) {
  if (container == NULL || !leme_container_action_allowed(container->focused) ||
      kind < LEME_LAYOUT_DWINDLE || kind > LEME_LAYOUT_MASTER_STACK) {
    return false;
  }
  if (kind == container->layout.kind) {
    return true;
  }
  struct leme_layout prepared = {0};
  if (!leme_layout_prepare(&container->layout, kind, container->views,
                           container->count, &prepared)) {
    return false;
  }
  leme_render_output_animations_finish(container->tag->owner->output);
  leme_layout_finish(&container->layout);
  container->layout = prepared;
  leme_container_geometry_changed(container, true);
  leme_render_container_restack(container, false);
  return true;
}

static size_t leme_container_index(const struct leme_container *container,
                                   const struct leme_view *view) {
  size_t index = 0;
  while (index < container->count && container->views[index] != view) {
    index++;
  }
  return index;
}

bool leme_container_swap_direction(struct leme_view *view,
                                   enum leme_direction direction) {
  if (!leme_container_action_allowed(view) || direction < LEME_DIRECTION_LEFT ||
      direction > LEME_DIRECTION_DOWN) {
    return false;
  }
  struct leme_container *container = view->container;
  if (container->count > SIZE_MAX / sizeof(struct leme_layout_candidate)) {
    return false;
  }
  struct leme_layout_candidate *candidates =
      calloc(container->count, sizeof(*candidates));
  if (candidates == NULL) {
    return false;
  }
  for (size_t i = 0; i < container->count; i++) {
    candidates[i] = (struct leme_layout_candidate){
        .view = container->views[i], .box = container->views[i]->box};
  }
  struct leme_view *neighbour = leme_layout_directional_candidate(
      candidates, container->count, view, direction);
  free(candidates);
  if (neighbour == NULL) {
    return true;
  }
  const size_t first = leme_container_index(container, view);
  const size_t second = leme_container_index(container, neighbour);
  if (first == container->count || second == container->count ||
      (container->layout.kind == LEME_LAYOUT_DWINDLE &&
       !leme_layout_swap_views(container->layout.root, view, neighbour))) {
    return false;
  }
  container->views[first] = neighbour;
  container->views[second] = view;
  leme_container_geometry_changed(container, true);
  leme_render_container_restack(container, false);
  return true;
}

static enum leme_direction leme_container_opposite(enum leme_direction side) {
  switch (side) {
  case LEME_DIRECTION_LEFT:
    return LEME_DIRECTION_RIGHT;
  case LEME_DIRECTION_RIGHT:
    return LEME_DIRECTION_LEFT;
  case LEME_DIRECTION_UP:
    return LEME_DIRECTION_DOWN;
  case LEME_DIRECTION_DOWN:
  default:
    return LEME_DIRECTION_UP;
  }
}

static bool leme_container_create(struct leme_view *target,
                                  struct leme_view *added,
                                  enum leme_direction direction,
                                  struct leme_container **out);

static bool leme_container_insert(struct leme_container *container,
                                  struct leme_view *target,
                                  struct leme_view *added,
                                  enum leme_direction direction);

bool leme_container_join(struct leme_view *view,
                         enum leme_direction direction) {
  if (direction < LEME_DIRECTION_LEFT || direction > LEME_DIRECTION_DOWN ||
      !leme_container_input_allowed(view) || view->container != NULL) {
    return false;
  }
  const struct leme_tag *tag = leme_ownership_tag(view);
  size_t count = 0;
  const struct leme_view *other = NULL;
  wl_list_for_each(other, &tag->views, tag_link) { count++; }
  if (count > SIZE_MAX / sizeof(struct leme_layout_candidate)) {
    return false;
  }
  struct leme_layout_candidate *candidates = calloc(count, sizeof(*candidates));
  if (candidates == NULL) {
    return false;
  }
  size_t used = 0;
  struct leme_view *candidate = NULL;
  wl_list_for_each(candidate, &tag->views, tag_link) {
    if (used < count &&
        (candidate == view || leme_container_member_eligible(candidate))) {
      candidates[used++] = (struct leme_layout_candidate){
          .view = candidate, .box = candidate->box};
    }
  }
  struct leme_view *target =
      leme_layout_directional_candidate(candidates, used, view, direction);
  free(candidates);
  if (target == NULL) {
    return false;
  }
  const enum leme_direction side = leme_container_opposite(direction);
  if (target->container != NULL) {
    return leme_container_insert(target->container, target, view, side);
  }
  struct leme_container *container = NULL;
  return leme_container_create(target, view, side, &container);
}

void leme_container_restore_placement(struct leme_container *container) {
  if (container == NULL ||
      container->placement.kind == LEME_VIEW_PLACEMENT_NORMAL) {
    return;
  }
  const struct leme_box normal = leme_view_policy_clamp_box(
      leme_container_normal_box(container),
      leme_placement_area(container->tag, container->tag->owner->output));
  if (leme_container_content_box(container, normal).width <= 0) {
    return;
  }
  leme_placement_forget(&container->placement);
  container->box = normal;
  leme_container_geometry_changed(
      container,
      !leme_input_pointer_grab_active(leme_container_server(container)));
}

bool leme_container_set_placement(struct leme_container *container,
                                  enum leme_view_placement kind) {
  if (container == NULL || !leme_container_action_allowed(container->focused) ||
      kind < LEME_VIEW_PLACEMENT_NORMAL ||
      kind > LEME_VIEW_PLACEMENT_BOTTOM_RIGHT ||
      (kind != LEME_VIEW_PLACEMENT_NORMAL &&
       !leme_container_can_place(container, kind))) {
    return false;
  }
  if (kind == container->placement.kind) {
    return true;
  }
  if (kind == LEME_VIEW_PLACEMENT_NORMAL) {
    leme_container_restore_placement(container);
    return container->placement.kind == kind;
  }
  if (!leme_placement_set(
          &container->placement, kind,
          leme_container_content_box(container, container->box))) {
    return false;
  }
  leme_container_geometry_changed(container, true);
  return true;
}

bool leme_container_unplace_for_resize(struct leme_container *container) {
  if (!leme_container_geometry_available(container) ||
      container->placement.kind == LEME_VIEW_PLACEMENT_MAXIMIZED) {
    return false;
  }
  leme_placement_forget(&container->placement);
  leme_container_geometry_changed(container, false);
  return true;
}

bool leme_container_apply_interactive_box(struct leme_container *container,
                                          struct leme_box box) {
  if (!leme_container_geometry_available(container) ||
      container->placement.kind != LEME_VIEW_PLACEMENT_NORMAL ||
      leme_container_content_box(container, box).width <= 0) {
    return false;
  }
  container->box = box;
  leme_container_geometry_changed(container, false);
  return true;
}

static void leme_container_destroy(struct leme_container *container) {
  leme_decoration_protocol_container_destroy(container);
  leme_desk_buttons_invalidate_container(container);
  leme_container_invalidate(container);
  wl_list_remove(&container->link);
  leme_render_container_destroy(container);
  leme_layout_finish(&container->layout);
  free(container->views);
  free(container);
}

static void leme_container_bind(struct leme_container *container,
                                struct leme_view *view) {
  leme_input_pointer_grab_cancel_view(view);
  leme_desk_buttons_invalidate_view(view);
  leme_render_view_finish_animation(view);
  leme_view_remember_desktop_box(view, view->box);
  const struct leme_box content = leme_render_view_content_box(view, view->box);
  leme_placement_forget(&view->placement);
  view->container = container;
  (void)leme_desk_decoration_sync(view);
  view->box = leme_render_view_frame_box(view, content);
  leme_render_view_set_box(view, view->box);
  leme_view_update_tiled(view);
  leme_view_sync_wm_state(view);
  leme_render_view_update_layer(view);
}

static void leme_container_unbind(struct leme_view *view) {
  const struct leme_box content = leme_render_view_content_box_for_state(
      view, view->fullscreen ? view->saved_box : view->box, false);
  leme_input_pointer_grab_cancel_view(view);
  leme_desk_buttons_invalidate_view(view);
  leme_render_view_finish_move(view);
  view->container = NULL;
  leme_placement_forget(&view->placement);
  if (!view->fullscreen) {
    view->box = (struct leme_box){0};
  }
  (void)leme_desk_decoration_sync(view);
  const struct leme_box frame =
      leme_render_view_frame_box_for_state(view, content, false);
  if (view->fullscreen) {
    view->saved_box = frame;
  } else {
    view->box = frame;
    leme_render_view_set_box(view, frame);
  }
  leme_view_update_tiled(view);
  leme_view_sync_wm_state(view);
}

static void leme_container_apply(struct leme_view *view, struct leme_box box,
                                 void *data) {
  const bool *animate = data;
  leme_view_apply_layout_box(view, box, *animate);
}

void leme_container_arrange(struct leme_container *container, bool animate) {
  if (container == NULL || container->count < 2) {
    return;
  }
  const struct leme_output *output = container->tag->owner->output;
  if (output != NULL) {
    const struct leme_box translated = leme_placement_translate_box(
        container->box, container->output_box, output->full_box);
    if (translated.width > 0) {
      container->box = translated;
    }
    if (container->placement.restore_valid) {
      const struct leme_box restored =
          leme_placement_translate_box(container->placement.restore_box,
                                       container->output_box, output->full_box);
      if (restored.width > 0) {
        container->placement.restore_box = restored;
      }
    }
    container->output_box = output->full_box;
  }
  if (container->placement.kind != LEME_VIEW_PLACEMENT_NORMAL &&
      output != NULL) {
    const struct leme_box area = leme_placement_area(container->tag, output);
    const struct leme_box box =
        leme_placement_box(area, container->placement.kind);
    if (leme_container_content_box(container, box).width > 0) {
      container->box = box;
    }
  }
  if (animate) {
    leme_render_container_move(container, container->render_box,
                               container->box);
    animate = container->animation_gate != NULL;
  }
  if (!animate) {
    leme_render_container_finish_move(container);
    for (size_t i = 0; i < container->count; i++) {
      leme_render_view_finish_move(container->views[i]);
    }
    leme_render_container_set_box(container, container->box);
  }
  container->arranged_content =
      leme_container_content_box(container, container->box);
  leme_layout_arrange_subject(&container->layout, container->views,
                              container->count, container->arranged_content,
                              LEME_CONTAINER_GAP, leme_container_apply,
                              &animate);
}

void leme_container_reconcile_geometry(struct leme_server *server) {
  if (server == NULL || server->outputs.next == NULL) {
    return;
  }
  struct leme_output *output = NULL;
  wl_list_for_each(output, &server->outputs, link) {
    if (output->tags.table == NULL) {
      continue;
    }
    for (uint16_t id = 1; id <= output->tags.max_tags; id++) {
      struct leme_tag *tag = output->tags.table[id];
      if (tag == NULL || tag->containers.next == NULL) {
        continue;
      }
      struct leme_container *container = NULL;
      wl_list_for_each(container, &tag->containers, link) {
        const struct leme_box old = container->output_box;
        const struct leme_box full = output->full_box;
        const struct leme_box placed = leme_placement_box(
            leme_placement_area(tag, output), container->placement.kind);
        const struct leme_box content =
            leme_container_content_box(container, container->box);
        const struct leme_box arranged = container->arranged_content;
        if (!leme_box_equal(old, full) || !leme_box_equal(content, arranged) ||
            (container->placement.kind != LEME_VIEW_PLACEMENT_NORMAL &&
             !leme_box_equal(placed, container->box))) {
          leme_container_arrange(container, false);
        }
      }
    }
  }
}

struct leme_container_transfer_item {
  struct leme_view *view;
  struct leme_box box;
  struct leme_box saved_box;
  struct leme_placement_state placement;
};

struct leme_container_transfer {
  struct leme_tag *source;
  struct leme_tag *target;
  struct leme_container_transfer_item *items;
  size_t count;
  struct leme_box area;
};

static bool
leme_container_transfer_contains(const struct leme_container *container,
                                 const struct leme_view *root,
                                 const struct leme_view *view, size_t limit) {
  if (container != NULL) {
    return leme_container_for_view(view) == container;
  }
  while (view != NULL && limit > 0) {
    if (view == root) {
      return true;
    }
    view = leme_view_parent(view);
    limit--;
  }
  return false;
}

static struct leme_box
leme_container_transfer_area(const struct leme_container *container,
                             const struct leme_view *root,
                             struct leme_tag *target) {
  struct leme_server *server = target->owner->server;
  const struct leme_output *output = target->owner->output;
  if (server->config == NULL || !server->config->desktop_minimized_strip ||
      output->tags.focused_id != target->id) {
    return output->usable_box;
  }
  const struct leme_tag *source =
      container == NULL ? leme_ownership_tag(root) : container->tag;
  size_t units = leme_minimize_unit_count(target);
  if (container != NULL && container->minimized) {
    if (units == SIZE_MAX) {
      return (struct leme_box){0};
    }
    units++;
  }
  const size_t limit = (size_t)wl_list_length(&server->views);
  const struct leme_view *view = NULL;
  wl_list_for_each(view, &source->views, tag_link) {
    if (leme_container_transfer_contains(container, root, view, limit) &&
        leme_view_minimized_root(view) == view) {
      if (units == SIZE_MAX) {
        return (struct leme_box){0};
      }
      units++;
    }
  }
  return leme_desk_strip_area_for_count(output->usable_box, units);
}

static bool
leme_container_transfer_prepare(const struct leme_container *container,
                                const struct leme_view *root,
                                struct leme_tag *target, bool translate,
                                struct leme_container_transfer *out) {
  const struct leme_tag *source =
      container != NULL ? container->tag : leme_ownership_tag(root);
  if (source == NULL || target == NULL || source->owner == NULL ||
      target->owner == NULL || source->owner->output == NULL ||
      target->owner->output == NULL || source == target ||
      source->layout.kind != LEME_LAYOUT_DESKTOP ||
      target->layout.kind != LEME_LAYOUT_DESKTOP) {
    return false;
  }
  struct leme_server *server = source->owner->server;
  const size_t limit = (size_t)wl_list_length(&server->views);
  struct leme_view *view = NULL;
  size_t count = 0;
  wl_list_for_each(view, &source->views, tag_link) {
    if (leme_container_transfer_contains(container, root, view, limit)) {
      count++;
    }
  }
  if (count == 0 || count > SIZE_MAX / sizeof(*out->items)) {
    return false;
  }
  struct leme_container_transfer_item *items = calloc(count, sizeof(*items));
  if (items == NULL) {
    return false;
  }
  const struct leme_box from = source->owner->output->full_box;
  const struct leme_box to = target->owner->output->full_box;
  const struct leme_box area =
      leme_container_transfer_area(container, root, target);
  if (area.width <= 0 || area.height <= 0) {
    free(items);
    return false;
  }
  size_t index = 0;
  wl_list_for_each(view, &source->views, tag_link) {
    if (!leme_container_transfer_contains(container, root, view, limit)) {
      continue;
    }
    if (!view->mapped || view->unmanaged || view->detached || index >= count) {
      free(items);
      return false;
    }
    struct leme_container_transfer_item *item = &items[index++];
    *item = (struct leme_container_transfer_item){.view = view,
                                                  .box = view->box,
                                                  .saved_box = view->saved_box,
                                                  .placement = view->placement};
    if (translate) {
      item->box = leme_placement_translate_box(item->box, from, to);
      if (item->box.width <= 0) {
        free(items);
        return false;
      }
      if (view->fullscreen) {
        item->saved_box =
            leme_placement_translate_box(item->saved_box, from, to);
        if (item->saved_box.width <= 0) {
          free(items);
          return false;
        }
      }
    }
    if (item->placement.restore_valid) {
      item->placement.restore_box =
          leme_placement_translate_box(item->placement.restore_box, from, to);
      if (item->placement.restore_box.width <= 0) {
        free(items);
        return false;
      }
    }
    if (view->container == NULL) {
      item->box = leme_view_policy_clamp_box(item->box, area);
    }
  }
  if (index != count) {
    free(items);
    return false;
  }
  *out = (struct leme_container_transfer){
      .source = container == NULL ? leme_ownership_tag(root) : container->tag,
      .target = target,
      .items = items,
      .count = count,
      .area = area};
  return true;
}

static void
leme_container_transfer_commit(struct leme_container_transfer *transfer,
                               bool preserve_home) {
  struct leme_tag *source = transfer->source;
  struct leme_tag *target = transfer->target;
  for (size_t i = 0; i < transfer->count; i++) {
    const struct leme_container_transfer_item *item = &transfer->items[i];
    struct leme_view *view = item->view;
    leme_layout_remove_view(&source->layout, view);
    if (source->focused_view == view) {
      source->focused_view = NULL;
    }
    wl_list_remove(&view->tag_link);
    char *home = preserve_home ? view->home_output : NULL;
    const uint16_t home_tag = view->home_tag;
    if (preserve_home) {
      view->home_output = NULL;
    }
    leme_ownership_replace_tag(view, source, target);
    if (preserve_home) {
      view->home_output = home;
      view->home_tag = home_tag;
    }
    wl_list_insert(target->views.prev, &view->tag_link);
    if (view->container == NULL) {
      view->box =
          view->fullscreen ? target->owner->output->full_box : item->box;
      view->saved_box = item->saved_box;
      view->placement = item->placement;
    } else if (view->fullscreen) {
      view->box = target->owner->output->full_box;
      view->saved_box = item->saved_box;
    }
  }
  if (target->id == target->owner->focused_id) {
    target->owner->focused_is_candidate = false;
  }
}

static void
leme_container_transfer_finish(struct leme_container_transfer *transfer) {
  struct leme_server *server = transfer->target->owner->server;
  struct leme_tags *source = transfer->source->owner;
  const uint16_t source_id = transfer->source->id;
  for (size_t i = 0; i < transfer->count; i++) {
    struct leme_view *view = transfer->items[i].view;
    if (view->container == NULL || view->fullscreen) {
      leme_render_view_set_box(view, view->box);
    }
    leme_render_view_update_layer(view);
    leme_view_sync_wm_state(view);
  }
  free(transfer->items);
  transfer->items = NULL;
  (void)leme_tags_remove_empty(source, source_id);
  server->view_refresh.reconciled = false;
  server->view_refresh.desktop_synced = false;
  leme_view_refresh_begin(server);
  leme_view_refresh_reconcile(server);
  leme_tags_refresh_visibility_passive(source);
  leme_tags_refresh_visibility_passive(transfer->target->owner);
  leme_public_server_invalidate(server);
  leme_publication_invalidate(server);
  leme_capture_sync(server);
  leme_view_refresh_end(server);
}

static bool leme_container_transfer_output(struct leme_container *container,
                                           struct leme_output *output,
                                           uint16_t tag_id,
                                           bool preserve_home) {
  if (container == NULL || container->tag == NULL ||
      container->tag->owner == NULL || container->count < 2 || output == NULL ||
      output->server != leme_container_server(container) ||
      output->wlr_output == NULL || !output->wlr_output->enabled ||
      output == container->tag->owner->output) {
    return false;
  }
  struct leme_tag_materialize *materialize = NULL;
  if (!leme_tags_prepare_materialize(&output->tags, tag_id, &materialize)) {
    return false;
  }
  struct leme_tag *target = leme_tags_materialize_target(materialize);
  struct leme_container_transfer transfer = {0};
  const bool translate = !leme_input_pointer_grab_moves_container(container);
  if (!leme_container_transfer_prepare(container, NULL, target, translate,
                                       &transfer)) {
    leme_tags_discard_materialize(&materialize);
    return false;
  }
  const struct leme_box from = container->tag->owner->output->full_box;
  const struct leme_box area = transfer.area;
  const struct leme_box moved =
      translate
          ? leme_placement_translate_box(container->box, from, output->full_box)
          : container->box;
  struct leme_box box =
      container->placement.kind == LEME_VIEW_PLACEMENT_NORMAL
          ? leme_view_policy_clamp_box(moved, area)
          : leme_placement_box(area, container->placement.kind);
  struct leme_placement_state placement = container->placement;
  struct leme_box normal = leme_placement_translate_box(
      leme_container_normal_box(container), from, output->full_box);
  normal = leme_view_policy_clamp_box(normal, area);
  if (moved.width <= 0 ||
      leme_container_content_box(container, box).width <= 0 ||
      leme_container_content_box(container, normal).width <= 0) {
    free(transfer.items);
    leme_tags_discard_materialize(&materialize);
    return false;
  }
  if (placement.restore_valid) {
    placement.restore_box = leme_container_content_box(container, normal);
  }
  leme_render_output_animations_finish(container->tag->owner->output);
  leme_render_output_animations_finish(output);
  leme_desk_buttons_invalidate_container(container);
  leme_tags_commit_materialize(&materialize);
  leme_container_transfer_commit(&transfer, preserve_home);
  wl_list_remove(&container->link);
  wl_list_insert(target->containers.prev, &container->link);
  container->tag = target;
  container->box = box;
  container->placement = placement;
  container->output_box = output->full_box;
  container->generation++;
  target->focused_view = container->focused;
  leme_container_arrange(container, false);
  leme_container_transfer_finish(&transfer);
  leme_render_container_restack(container, false);
  return true;
}

bool leme_container_adopt_output(struct leme_container *container,
                                 struct leme_output *output, uint16_t tag_id) {
  if (output == NULL) {
    return false;
  }
  if (tag_id > output->tags.max_tags) {
    tag_id = output->tags.max_tags;
  }
  return leme_container_transfer_output(container, output, tag_id, true);
}

bool leme_container_move_to_output(struct leme_container *container,
                                   struct leme_output *output, bool follow) {
  if (container == NULL || container->tag == NULL ||
      container->tag->owner == NULL || output == NULL) {
    return false;
  }
  struct leme_server *server = leme_container_server(container);
  const struct leme_output *source = container->tag->owner->output;
  if (source == NULL || source->wlr_output == NULL ||
      !source->wlr_output->enabled || output->tags.table == NULL ||
      output->tags.focused_id == 0 ||
      output->tags.focused_id > output->tags.max_tags) {
    return false;
  }
  if (leme_tag_fullscreen_view(output->tags.table[output->tags.focused_id]) !=
          NULL ||
      leme_tag_fullscreen_view(container->tag) != NULL || server == NULL ||
      leme_session_locked(server) || leme_layer_keyboard_is_exclusive(server) ||
      leme_input_protocols_shortcuts_inhibited(server) ||
      leme_input_protocols_pointer_locked(server) ||
      (leme_input_pointer_grab_active(server) &&
       !leme_input_pointer_grab_moves_container(container))) {
    return false;
  }
  if (!leme_container_transfer_output(container, output,
                                      output->tags.focused_id, false)) {
    return false;
  }
  if (follow) {
    leme_output_set_focused(server, output, false);
    if (!container->minimized) {
      leme_view_focus(container->focused);
    }
  }
  return true;
}

static bool leme_container_drop_ownership(const struct leme_view *target,
                                          const struct leme_view *added) {
  if (target->server != added->server) {
    return false;
  }
  if (leme_ownership_tag(target) == leme_ownership_tag(added)) {
    return true;
  }
  return leme_view_output(target) != leme_view_output(added) &&
         (target->server->config == NULL ||
          target->server->config->output_policy.cross_output_drag);
}

struct leme_container_insertion {
  struct leme_layout layout;
  struct leme_view **views;
  size_t count;
};

static bool
leme_container_prepare_insert(struct leme_container *container,
                              struct leme_view *target, struct leme_view *added,
                              enum leme_direction direction,
                              struct leme_container_insertion *out) {
  if (container == NULL || !leme_container_member_eligible(target) ||
      !leme_container_member_eligible(added) ||
      target->container != container || added->container != NULL ||
      !leme_container_drop_ownership(target, added) ||
      direction < LEME_DIRECTION_LEFT || direction > LEME_DIRECTION_DOWN ||
      container->count >= SIZE_MAX / sizeof(*container->views) - 1) {
    return false;
  }
  size_t index = leme_container_index(container, target);
  if (index == container->count) {
    return false;
  }
  if (direction == LEME_DIRECTION_RIGHT || direction == LEME_DIRECTION_DOWN) {
    index++;
  }
  struct leme_container_insertion prepared = {.count = container->count + 1};
  prepared.views = calloc(prepared.count, sizeof(*prepared.views));
  if (prepared.views == NULL) {
    return false;
  }
  memcpy(prepared.views, container->views, index * sizeof(*prepared.views));
  prepared.views[index] = added;
  memcpy(&prepared.views[index + 1], &container->views[index],
         (container->count - index) * sizeof(*prepared.views));
  if (!leme_layout_prepare_insert(&container->layout, target, added, direction,
                                  &prepared.layout)) {
    free(prepared.views);
    return false;
  }
  *out = prepared;
  return true;
}

static bool leme_container_insert(struct leme_container *container,
                                  struct leme_view *target,
                                  struct leme_view *added,
                                  enum leme_direction direction) {
  struct leme_container_insertion prepared = {0};
  if (!leme_container_prepare_insert(container, target, added, direction,
                                     &prepared)) {
    return false;
  }
  struct leme_container_transfer transfer = {0};
  if (leme_ownership_tag(added) != container->tag &&
      !leme_container_transfer_prepare(NULL, added, container->tag, false,
                                       &transfer)) {
    leme_layout_finish(&prepared.layout);
    free(prepared.views);
    return false;
  }
  leme_render_output_animations_finish(leme_view_output(added));
  leme_render_output_animations_finish(container->tag->owner->output);
  if (transfer.items != NULL) {
    leme_container_transfer_commit(&transfer, false);
  }
  leme_layout_finish(&container->layout);
  free(container->views);
  container->layout = prepared.layout;
  container->views = prepared.views;
  container->count = prepared.count;
  container->focused = added;
  leme_container_bind(container, added);
  if (transfer.items != NULL) {
    container->generation++;
    leme_container_invalidate(container);
    leme_container_arrange(container, false);
    leme_container_transfer_finish(&transfer);
  } else {
    leme_container_geometry_changed(container, true);
  }
  leme_render_container_restack(container, true);
  return true;
}

struct leme_container_preview {
  struct leme_view *view;
  struct leme_box box;
};

static void leme_container_collect_preview(struct leme_view *view,
                                           struct leme_box box, void *data) {
  struct leme_container_preview *preview = data;
  if (view == preview->view) {
    preview->box = box;
  }
}

bool leme_container_prepare_drop(struct leme_view *target,
                                 struct leme_view *added, double lx, double ly,
                                 struct leme_container_drop *out) {
  if (out == NULL) {
    return false;
  }
  *out = (struct leme_container_drop){0};
  if (!isfinite(lx) || !isfinite(ly) || target == added ||
      !leme_container_member_eligible(target) ||
      !leme_container_member_eligible(added) || added->container != NULL ||
      !leme_container_drop_ownership(target, added)) {
    return false;
  }
  struct leme_container *container = target->container;
  const struct leme_container candidate = {.tag = leme_ownership_tag(target)};
  const struct leme_box content =
      container == NULL ? leme_container_content_box(&candidate, target->box)
                        : leme_render_view_content_box(target, target->box);
  struct leme_layout_drop direction = {0};
  if (!leme_container_drop_at(
          content,
          fmax(content.x, fmin(lx, (double)content.x + content.width - 1)),
          fmax(content.y, fmin(ly, (double)content.y + content.height - 1)),
          &direction)) {
    return false;
  }
  struct leme_box frame = container == NULL ? target->box : container->box;
  if (leme_ownership_tag(target) != leme_ownership_tag(added)) {
    const struct leme_box area =
        leme_container_transfer_area(NULL, added, candidate.tag);
    const enum leme_view_placement placement =
        container == NULL ? target->placement.kind : container->placement.kind;
    if (placement != LEME_VIEW_PLACEMENT_NORMAL) {
      frame = leme_placement_box(area, placement);
    }
    if (area.width <= 0 || area.height <= 0 ||
        leme_container_content_box(&candidate, frame).width <= 0) {
      return false;
    }
  }
  struct leme_container_preview preview = {.view = added};
  if (container != NULL) {
    struct leme_container_insertion prepared = {0};
    if (!leme_container_prepare_insert(container, target, added,
                                       direction.direction, &prepared)) {
      return false;
    }
    leme_layout_arrange_subject(
        &prepared.layout, prepared.views, prepared.count,
        leme_container_content_box(container, frame), LEME_CONTAINER_GAP,
        leme_container_collect_preview, &preview);
    leme_layout_finish(&prepared.layout);
    free(prepared.views);
  } else {
    const bool before = direction.direction == LEME_DIRECTION_LEFT ||
                        direction.direction == LEME_DIRECTION_UP;
    struct leme_view *views[2] = {before ? added : target,
                                  before ? target : added};
    struct leme_layout_node first = {.is_leaf = true, .data.view = views[0]};
    struct leme_layout_node second = {.is_leaf = true, .data.view = views[1]};
    struct leme_layout_node root = {
        .data.branch = {.first = &first,
                        .second = &second,
                        .split =
                            direction.direction == LEME_DIRECTION_LEFT ||
                                    direction.direction == LEME_DIRECTION_RIGHT
                                ? LEME_SPLIT_HORIZONTAL
                                : LEME_SPLIT_VERTICAL,
                        .ratio = LEME_CONTAINER_SPLIT_RATIO}};
    struct leme_layout layout = {.kind = LEME_LAYOUT_DWINDLE, .root = &root};
    leme_layout_arrange_subject(
        &layout, views, 2, leme_container_content_box(&candidate, frame),
        LEME_CONTAINER_GAP, leme_container_collect_preview, &preview);
  }
  if (preview.box.width <= 0 || preview.box.height <= 0) {
    return false;
  }
  *out = (struct leme_container_drop){.source = added->public_meta.id,
                                      .target = target->public_meta.id,
                                      .direction = direction.direction,
                                      .preview = preview.box,
                                      .x = lx,
                                      .y = ly};
  return true;
}

bool leme_container_commit_drop(struct leme_server *server,
                                const struct leme_container_drop *drop) {
  if (server == NULL || drop == NULL || leme_session_locked(server) ||
      leme_input_protocols_shortcuts_inhibited(server) ||
      leme_layer_keyboard_is_exclusive(server)) {
    return false;
  }
  struct leme_view *target = leme_view_by_public_id(server, drop->target);
  struct leme_view *added = leme_view_by_public_id(server, drop->source);
  struct leme_container_drop current = {0};
  if (!leme_container_prepare_drop(target, added, drop->x, drop->y, &current)) {
    return false;
  }
  if (target->container != NULL) {
    return leme_container_insert(target->container, target, added,
                                 current.direction);
  }
  struct leme_container *container = NULL;
  return leme_container_create(target, added, current.direction, &container);
}

static bool leme_container_create(struct leme_view *target,
                                  struct leme_view *added,
                                  enum leme_direction direction,
                                  struct leme_container **out) {
  if (out == NULL || *out != NULL || target == added ||
      !leme_container_member_eligible(target) ||
      !leme_container_member_eligible(added) || target->container != NULL ||
      added->container != NULL ||
      !leme_container_drop_ownership(target, added) ||
      direction < LEME_DIRECTION_LEFT || direction > LEME_DIRECTION_DOWN) {
    return false;
  }
  struct leme_container *container = calloc(1, sizeof(*container));
  if (container == NULL) {
    return false;
  }
  struct leme_container_transfer transfer = {0};
  container->tag = leme_ownership_tag(target);
  if (leme_ownership_tag(added) != container->tag &&
      !leme_container_transfer_prepare(NULL, added, container->tag, false,
                                       &transfer)) {
    goto fail;
  }
  container->box = target->box;
  container->output_box = leme_view_output(target)->full_box;
  container->placement = target->placement;
  if (container->placement.restore_valid) {
    container->placement.restore_box =
        leme_container_content_box(container, leme_view_normal_box(target));
  }
  wl_list_init(&container->link);
  leme_layout_init(&container->layout, LEME_LAYOUT_DWINDLE);
  if (leme_container_content_box(container, container->box).width <= 0 ||
      (container->placement.restore_valid &&
       container->placement.restore_box.width <= 0)) {
    goto fail;
  }
  container->views = calloc(2, sizeof(*container->views));
  if (container->views == NULL) {
    goto fail;
  }
  const bool before =
      direction == LEME_DIRECTION_LEFT || direction == LEME_DIRECTION_UP;
  container->views[0] = before ? added : target;
  container->views[1] = before ? target : added;
  struct leme_layout prepared = {0};
  if (!leme_layout_prepare(&container->layout, LEME_LAYOUT_DWINDLE,
                           container->views, 2, &prepared)) {
    goto fail;
  }
  container->layout = prepared;
  container->layout.root->data.branch.split =
      direction == LEME_DIRECTION_LEFT || direction == LEME_DIRECTION_RIGHT
          ? LEME_SPLIT_HORIZONTAL
          : LEME_SPLIT_VERTICAL;
  container->layout.root->data.branch.ratio = LEME_CONTAINER_SPLIT_RATIO;
  if (!leme_render_container_create(container) ||
      leme_public_model_issue_id(target->server->public_model,
                                 &container->id) != LEME_PUBLIC_OK) {
    goto fail;
  }
  leme_render_output_animations_finish(leme_view_output(added));
  leme_render_output_animations_finish(leme_view_output(target));
  if (transfer.items != NULL) {
    leme_container_transfer_commit(&transfer, false);
  }
  wl_list_insert(container->tag->containers.prev, &container->link);
  container->count = 2;
  container->focused = added;
  container->generation = 1;
  leme_container_bind(container, container->views[0]);
  leme_container_bind(container, container->views[1]);
  if (transfer.items != NULL) {
    leme_container_arrange(container, false);
    leme_container_transfer_finish(&transfer);
  } else {
    leme_container_arrange(container, true);
  }
  leme_render_container_set_visible(container, true);
  leme_render_container_restack(container, true);
  leme_container_invalidate(container);
  *out = container;
  return true;
fail:
  free(transfer.items);
  leme_render_container_destroy(container);
  leme_layout_finish(&container->layout);
  free(container->views);
  free(container);
  return false;
}

void leme_container_remove_view(struct leme_view *view) {
  if (view == NULL || view->container == NULL) {
    return;
  }
  const bool animate = !leme_input_pointer_grab_active(view->server);
  struct leme_container *container = view->container;
  const size_t index = leme_container_index(container, view);
  if (index == container->count) {
    return;
  }
  leme_input_pointer_grab_cancel_view(view);
  leme_layout_remove_view(&container->layout, view);
  memmove(&container->views[index], &container->views[index + 1],
          (container->count - index - 1) * sizeof(*container->views));
  container->count--;
  container->generation++;
  if (container->focused == view) {
    container->focused = container->count == 0
                             ? NULL
                             : container->views[index % container->count];
  }
  leme_container_unbind(view);
  if (container->count > 1) {
    leme_container_arrange(container, animate);
    leme_container_invalidate(container);
    return;
  }
  if (container->count == 1) {
    struct leme_view *survivor = container->views[0];
    const struct leme_box normal = leme_container_normal_box(container);
    leme_container_unbind(survivor);
    survivor->placement = container->placement;
    if (survivor->placement.restore_valid) {
      survivor->placement.restore_box =
          leme_render_view_content_box_for_state(survivor, normal, false);
    }
    survivor->minimized = container->minimized;
    survivor->minimized_order = container->minimized_order;
    if (survivor->fullscreen) {
      survivor->saved_box = container->box;
    } else {
      survivor->box = container->box;
      leme_render_view_set_box(survivor, survivor->box);
    }
    leme_view_sync_wm_state(survivor);
  }
  leme_container_destroy(container);
}

static bool leme_container_dissolve_internal(struct leme_container *container,
                                             bool refresh) {
  if (container == NULL) {
    return false;
  }
  struct leme_tags *tags = container->tag->owner;
  const bool minimized = container->minimized;
  container->minimized = false;
  leme_render_output_animations_finish(container->tag->owner->output);
  for (size_t i = 0; i < container->count; i++) {
    leme_container_unbind(container->views[i]);
  }
  leme_container_destroy(container);
  if (minimized && refresh) {
    leme_tags_refresh_visibility_passive(tags);
  }
  return true;
}

bool leme_container_dissolve(struct leme_container *container) {
  return leme_container_dissolve_internal(container, true);
}

bool leme_container_close(struct leme_container *container) {
  if (container == NULL ||
      container->count > SIZE_MAX / sizeof(struct leme_public_id)) {
    return false;
  }
  const size_t count = container->count;
  struct leme_public_id *ids = calloc(count, sizeof(*ids));
  if (ids == NULL) {
    return false;
  }
  struct leme_server *server = leme_container_server(container);
  for (size_t i = 0; i < count; i++) {
    ids[i] = container->views[i]->public_meta.id;
  }
  for (size_t i = 0; i < count; i++) {
    struct leme_view *view = leme_view_by_public_id(server, ids[i]);
    if (view != NULL) {
      leme_view_protocol_close(view);
    }
  }
  free(ids);
  return true;
}

void leme_container_tag_finish(struct leme_tag *tag) {
  if (tag == NULL || tag->containers.next == NULL) {
    return;
  }
  struct leme_container *container = NULL;
  struct leme_container *next = NULL;
  wl_list_for_each_safe(container, next, &tag->containers, link) {
    (void)leme_container_dissolve_internal(container, false);
  }
}

struct leme_container *leme_container_by_public_id(struct leme_server *server,
                                                   struct leme_public_id id) {
  if (server == NULL || id.serial == 0 || server->views.next == NULL) {
    return NULL;
  }
  struct leme_view *view = NULL;
  wl_list_for_each(view, &server->views, link) {
    if (view->container != NULL && view->container->id.serial == id.serial) {
      return view->container;
    }
  }
  return NULL;
}
