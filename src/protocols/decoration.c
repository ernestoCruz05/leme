#include "protocols/decoration.h"

#include "core/server.h"
#include "config/style.h"
#include "desktop/decoration.h"
#include "desktop/menu.h"
#include "desktop/target.h"
#include "input/input.h"
#include "protocols/desktop.h"
#include "protocols/session.h"
#include "public/model.h"
#include "render/render.h"
#include "output/output.h"
#include "shell/container.h"
#include "shell/ownership.h"
#include "shell/policy.h"
#include "shell/view.h"
#include "workspace/tag.h"

#include "leme-decoration-v1-protocol.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>

#define LEME_DECORATION_VERSION 1
#define LEME_DECORATION_MARGIN_MAX 4096
#define LEME_DECORATION_STATES_DEFAULT                                         \
  (LEME_DECORATION_V1_STATES_FLOATING | LEME_DECORATION_V1_STATES_TILED |      \
   LEME_DECORATION_V1_STATES_MAXIMIZED | LEME_DECORATION_V1_STATES_DESKTOP)
#define LEME_DECORATION_STATES_ALL                                             \
  (LEME_DECORATION_STATES_DEFAULT | LEME_DECORATION_V1_STATES_FULLSCREEN)

struct leme_decoration_protocol {
  struct leme_server *server;
  struct wl_global *global;
  struct wl_resource *active;
  struct wl_list windows;
  struct wl_event_source *idle;
};

struct leme_decoration_window {
  struct wl_resource *resource;
  struct leme_view *view;
  struct leme_container *container;
  struct wl_list link;
  struct wl_list decorations;
  char *title;
  char *app_id;
  uint32_t state;
  int width;
  int height;
  uint32_t serial;
  uint32_t first_serial;
  bool first_sent;
  bool acked;
  struct leme_box frame;
  struct leme_gaps insets;
  bool border_replaced;
  bool shown;
};

struct leme_decoration_settings {
  int x;
  int y;
  struct leme_gaps margins;
  uint32_t states;
  bool replace_border;
};

struct leme_decoration {
  struct wl_resource *resource;
  struct leme_decoration_window *window;
  struct wlr_surface *surface;
  struct wlr_scene_tree *tree;
  struct wl_listener tree_destroy;
  struct wl_list link;
  bool below;
  struct leme_decoration_settings pending;
  struct leme_decoration_settings current;
};

static bool leme_decoration_view_on_desktop(const struct leme_view *view) {
  const struct leme_tag *tag = leme_ownership_tag(view);

  return tag != NULL && tag->layout.kind == LEME_LAYOUT_DESKTOP;
}

static uint32_t leme_decoration_view_state(const struct leme_view *view,
                                           bool fullscreen) {
  if (fullscreen) {
    return LEME_DECORATION_V1_STATES_FULLSCREEN;
  }
  if (leme_view_is_maximized(view)) {
    return LEME_DECORATION_V1_STATES_MAXIMIZED;
  }
  if (leme_view_geometry_owner(view) == LEME_VIEW_GEOMETRY_TAG_LAYOUT) {
    return LEME_DECORATION_V1_STATES_TILED;
  }
  return leme_decoration_view_on_desktop(view)
             ? LEME_DECORATION_V1_STATES_DESKTOP
             : LEME_DECORATION_V1_STATES_FLOATING;
}

static uint32_t
leme_decoration_container_state(const struct leme_container *container) {
  if (container->placement.kind == LEME_VIEW_PLACEMENT_MAXIMIZED) {
    return LEME_DECORATION_V1_STATES_MAXIMIZED;
  }
  return container->tag != NULL &&
                 container->tag->layout.kind == LEME_LAYOUT_DESKTOP
             ? LEME_DECORATION_V1_STATES_DESKTOP
             : LEME_DECORATION_V1_STATES_FLOATING;
}

static struct leme_server *
leme_decoration_window_server(const struct leme_decoration_window *window) {
  if (window->view != NULL) {
    return window->view->server;
  }
  return window->container == NULL ? NULL
                                   : window->container->tag->owner->server;
}

static bool leme_decoration_shown(const struct leme_decoration *decoration,
                                  bool fullscreen) {
  const struct leme_decoration_window *window = decoration->window;

  if (window == NULL || decoration->tree == NULL ||
      !decoration->surface->mapped) {
    return false;
  }
  uint32_t state = 0;
  if (window->view != NULL) {
    if (window->view->container != NULL) {
      return false;
    }
    state = leme_decoration_view_state(window->view, fullscreen);
  } else if (window->container != NULL) {
    state = leme_decoration_container_state(window->container);
  }
  return (decoration->current.states & state) != 0;
}

static bool
leme_decoration_window_any_shown(const struct leme_decoration_window *window,
                                 bool fullscreen) {
  const struct leme_decoration *decoration;

  if (window == NULL) {
    return false;
  }
  wl_list_for_each(decoration, &window->decorations, link) {
    if (leme_decoration_shown(decoration, fullscreen)) {
      return true;
    }
  }
  return false;
}

static bool
leme_decoration_window_takes_border(const struct leme_decoration_window *window,
                                    bool fullscreen) {
  const struct leme_decoration *decoration;

  if (window == NULL) {
    return false;
  }
  wl_list_for_each(decoration, &window->decorations, link) {
    if (decoration->current.replace_border &&
        leme_decoration_shown(decoration, fullscreen)) {
      return true;
    }
  }
  return false;
}

static int leme_decoration_margin(int64_t sum) {
  return sum > LEME_DECORATION_MARGIN_MAX ? LEME_DECORATION_MARGIN_MAX
                                          : (int)sum;
}

static struct leme_gaps
leme_decoration_window_insets(const struct leme_decoration_window *window,
                              bool fullscreen) {
  const struct leme_decoration *decoration;
  int64_t top = 0;
  int64_t right = 0;
  int64_t bottom = 0;
  int64_t left = 0;

  if (window == NULL) {
    return (struct leme_gaps){0};
  }
  wl_list_for_each(decoration, &window->decorations, link) {
    if (leme_decoration_shown(decoration, fullscreen)) {
      top += decoration->current.margins.top;
      right += decoration->current.margins.right;
      bottom += decoration->current.margins.bottom;
      left += decoration->current.margins.left;
    }
  }
  return (struct leme_gaps){
      .top = leme_decoration_margin(top),
      .right = leme_decoration_margin(right),
      .bottom = leme_decoration_margin(bottom),
      .left = leme_decoration_margin(left),
  };
}

static size_t
leme_decoration_window_trees(const struct leme_decoration_window *window,
                             bool fullscreen, struct wlr_scene_tree **trees,
                             size_t capacity) {
  const struct leme_decoration *decoration;
  size_t count = 0;

  if (window == NULL) {
    return 0;
  }
  wl_list_for_each(decoration, &window->decorations, link) {
    if (count < capacity && leme_decoration_shown(decoration, fullscreen)) {
      trees[count++] = decoration->tree;
    }
  }
  return count;
}

static void
leme_decoration_window_arrange(struct leme_decoration_window *window,
                               int content_x, int content_y, bool fullscreen) {
  struct leme_decoration *decoration;

  if (window == NULL) {
    return;
  }
  wl_list_for_each(decoration, &window->decorations, link) {
    if (decoration->tree == NULL) {
      continue;
    }
    struct wlr_scene_node *node = &decoration->tree->node;
    wlr_scene_node_set_position(node, content_x + decoration->current.x,
                                content_y + decoration->current.y);
    wlr_scene_node_set_enabled(node,
                               leme_decoration_shown(decoration, fullscreen));
    if (decoration->below) {
      wlr_scene_node_lower_to_bottom(node);
    } else {
      wlr_scene_node_raise_to_top(node);
    }
  }
}

bool leme_view_has_shell_decoration(const struct leme_view *view,
                                    bool fullscreen) {
  return view != NULL &&
         leme_decoration_window_any_shown(view->decoration_window, fullscreen);
}

bool leme_decoration_protocol_replaces_border(const struct leme_view *view,
                                              bool fullscreen) {
  return view != NULL && leme_decoration_window_takes_border(
                             view->decoration_window, fullscreen);
}

struct leme_gaps leme_decoration_protocol_insets(const struct leme_view *view,
                                                 bool fullscreen) {
  return view == NULL ? (struct leme_gaps){0}
                      : leme_decoration_window_insets(view->decoration_window,
                                                      fullscreen);
}

void leme_decoration_protocol_view_arrange(struct leme_view *view,
                                           int content_x, int content_y) {
  if (view == NULL) {
    return;
  }
  leme_decoration_window_arrange(view->decoration_window, content_x, content_y,
                                 view->fullscreen);
  if (view->drop_preview != NULL) {
    wlr_scene_node_raise_to_top(&view->drop_preview->node);
  }
}

bool leme_decoration_protocol_container_decorated(
    const struct leme_container *container) {
  return container != NULL &&
         leme_decoration_window_any_shown(container->decoration_window, false);
}

bool leme_decoration_protocol_container_replaces_border(
    const struct leme_container *container) {
  return container != NULL && leme_decoration_window_takes_border(
                                  container->decoration_window, false);
}

struct leme_gaps leme_decoration_protocol_container_insets(
    const struct leme_container *container) {
  return container == NULL ? (struct leme_gaps){0}
                           : leme_decoration_window_insets(
                                 container->decoration_window, false);
}

void leme_decoration_protocol_container_arrange(
    struct leme_container *container, int content_x, int content_y) {
  if (container != NULL) {
    leme_decoration_window_arrange(container->decoration_window, content_x,
                                   content_y, false);
  }
}

size_t
leme_decoration_protocol_container_trees(const struct leme_container *container,
                                         struct wlr_scene_tree **trees,
                                         size_t capacity) {
  return container == NULL
             ? 0
             : leme_decoration_window_trees(container->decoration_window, false,
                                            trees, capacity);
}

static bool leme_decoration_gaps_equal(struct leme_gaps a, struct leme_gaps b) {
  return a.top == b.top && a.right == b.right && a.bottom == b.bottom &&
         a.left == b.left;
}

static bool leme_decoration_view_floats(const struct leme_view *view) {
  return leme_view_geometry_owner(view) == LEME_VIEW_GEOMETRY_FLOATING &&
         !view->fullscreen && !leme_view_is_maximized(view) &&
         !leme_view_is_placed(view);
}

static struct leme_gaps leme_decoration_frame(const struct leme_view *view,
                                              struct leme_gaps insets,
                                              bool border_replaced) {
  const int border = border_replaced || view->server->config == NULL
                         ? 0
                         : view->server->config->border_width;

  return (struct leme_gaps){
      .top = insets.top + border,
      .right = insets.right + border,
      .bottom = insets.bottom + border,
      .left = insets.left + border,
  };
}

static void leme_decoration_reframe_box(struct leme_box *box,
                                        const struct leme_output *output,
                                        struct leme_gaps from,
                                        struct leme_gaps to) {
  const int64_t x = (int64_t)box->x - (to.left - from.left);
  const int64_t y = (int64_t)box->y - (to.top - from.top);
  const int64_t width =
      (int64_t)box->width + (to.left - from.left) + (to.right - from.right);
  const int64_t height =
      (int64_t)box->height + (to.top - from.top) + (to.bottom - from.bottom);

  if (x < INT_MIN || x > INT_MAX || y < INT_MIN || y > INT_MAX || width < 1 ||
      width > INT_MAX || height < 1 || height > INT_MAX) {
    return;
  }
  const struct leme_box frame = {(int)x, (int)y, (int)width, (int)height};
  *box = output == NULL ? frame
                        : leme_view_policy_clamp_box(frame, output->usable_box);
}

static struct leme_gaps
leme_decoration_container_frame(const struct leme_container *container,
                                struct leme_gaps insets, bool border_replaced,
                                bool shown) {
  const struct leme_config *config = container->tag->owner->server->config;
  const int border =
      border_replaced || config == NULL ? 0 : config->border_width;
  const int titlebar =
      shown || config == NULL ? 0 : leme_desk_style_titlebar_height(config);

  return (struct leme_gaps){
      .top = insets.top + border + titlebar,
      .right = insets.right + border,
      .bottom = insets.bottom + border,
      .left = insets.left + border,
  };
}

static void
leme_decoration_container_update(struct leme_decoration_window *window) {
  struct leme_container *container = window->container;
  const bool shown = leme_decoration_window_any_shown(window, false);
  const struct leme_gaps insets = leme_decoration_window_insets(window, false);
  const bool border_replaced =
      leme_decoration_window_takes_border(window, false);
  const bool changed = shown != window->shown ||
                       !leme_decoration_gaps_equal(insets, window->insets) ||
                       border_replaced != window->border_replaced;

  if (!changed) {
    const struct leme_box box = container->render_box;
    const struct leme_box content = leme_container_content_box(container, box);
    leme_decoration_window_arrange(window, content.x - box.x, content.y - box.y,
                                   false);
    window->frame = container->box;
    return;
  }
  const bool reframe =
      container->placement.kind == LEME_VIEW_PLACEMENT_NORMAL &&
      leme_box_equal(window->frame, container->box);
  const struct leme_gaps previous = leme_decoration_container_frame(
      container, window->insets, window->border_replaced, window->shown);
  window->shown = shown;
  window->insets = insets;
  window->border_replaced = border_replaced;
  if (reframe) {
    leme_decoration_reframe_box(&container->box, container->tag->owner->output,
                                previous,
                                leme_decoration_container_frame(
                                    container, insets, border_replaced, shown));
  }
  leme_container_arrange(container, false);
  window->frame = container->box;
}

static void leme_decoration_view_configure(struct leme_view *view) {
  const struct leme_box content = leme_render_view_content_box(view, view->box);

  if (view->configure_deferred) {
    view->deferred_configure_box = content;
    view->configure_dirty = true;
    return;
  }
  leme_view_configure(view, content);
}

static void
leme_decoration_window_update(struct leme_decoration_window *window) {
  struct leme_view *view = window->view;

  if (window->container != NULL) {
    leme_decoration_container_update(window);
    return;
  }
  if (view == NULL) {
    return;
  }
  const bool shown = leme_decoration_window_any_shown(window, view->fullscreen);
  const struct leme_gaps insets =
      leme_decoration_window_insets(window, view->fullscreen);
  const bool border_replaced =
      leme_decoration_window_takes_border(window, view->fullscreen);
  const bool resized = !leme_decoration_gaps_equal(insets, window->insets) ||
                       border_replaced != window->border_replaced;
  if (!view->mapped || (shown == window->shown && !resized)) {
    const struct leme_box content =
        leme_render_view_content_box(view, view->box);
    leme_decoration_protocol_view_arrange(view, content.x - view->box.x,
                                          content.y - view->box.y);
    window->frame = view->box;
    return;
  }
  const bool reframe = resized && leme_decoration_view_floats(view) &&
                       leme_box_equal(window->frame, view->box);
  const struct leme_gaps previous =
      leme_decoration_frame(view, window->insets, window->border_replaced);
  window->shown = shown;
  window->insets = insets;
  window->border_replaced = border_replaced;
  leme_desk_decoration_refresh(view);
  if (reframe) {
    leme_decoration_reframe_box(
        &view->box, leme_view_output(view), previous,
        leme_decoration_frame(view, insets, border_replaced));
  }
  leme_render_view_set_box(view, view->box);
  if (resized) {
    leme_decoration_view_configure(view);
  }
  window->frame = view->box;
}

static void leme_decoration_handle_tree_destroy(struct wl_listener *listener,
                                                void *data) {
  struct leme_decoration *decoration =
      wl_container_of(listener, decoration, tree_destroy);

  (void)data;
  wl_list_remove(&decoration->tree_destroy.link);
  decoration->tree = NULL;
}

static void leme_decoration_detach(struct leme_decoration *decoration) {
  if (decoration->tree != NULL) {
    wl_list_remove(&decoration->tree_destroy.link);
    wlr_scene_node_destroy(&decoration->tree->node);
    decoration->tree = NULL;
  }
  wl_list_remove(&decoration->link);
  wl_list_init(&decoration->link);
  decoration->window = NULL;
}

static void leme_decoration_window_free(struct leme_decoration_window *window) {
  free(window->title);
  free(window->app_id);
  free(window);
}

static void leme_decoration_window_detach(struct leme_decoration_window *window,
                                          bool refresh) {
  struct leme_view *view = window->view;
  struct leme_decoration *decoration;
  struct leme_decoration *next;

  wl_list_for_each_safe(decoration, next, &window->decorations, link) {
    leme_decoration_detach(decoration);
  }
  if ((view != NULL || window->container != NULL) && refresh) {
    leme_decoration_window_update(window);
  }
  if (window->container != NULL) {
    window->container->decoration_window = NULL;
    window->container = NULL;
  }
  if (view != NULL) {
    view->decoration_window = NULL;
    window->view = NULL;
    if (refresh && view->open_animation_pending) {
      leme_render_view_open_ready(view);
    }
  }
  window->shown = false;
  wl_list_remove(&window->link);
  wl_list_init(&window->link);
  if (window->resource == NULL) {
    leme_decoration_window_free(window);
  }
}

static void leme_decoration_window_close(struct leme_decoration_window *window,
                                         bool refresh) {
  if (window->resource != NULL) {
    leme_decoration_window_v1_send_closed(window->resource);
  }
  leme_decoration_window_detach(window, refresh);
}

static void
leme_decoration_window_handle_resource_destroy(struct wl_resource *resource) {
  struct leme_decoration_window *window = wl_resource_get_user_data(resource);
  struct leme_decoration *decoration;
  struct leme_decoration *next;

  window->resource = NULL;
  if (window->view == NULL && window->container == NULL) {
    leme_decoration_window_free(window);
    return;
  }
  wl_list_for_each_safe(decoration, next, &window->decorations, link) {
    leme_decoration_detach(decoration);
  }
  leme_decoration_window_update(window);
}

static void leme_decoration_handle_destroy(struct wl_client *client,
                                           struct wl_resource *resource) {
  (void)client;
  wl_resource_destroy(resource);
}

static struct leme_decoration *
leme_decoration_from_resource(struct wl_resource *resource) {
  return wl_resource_get_user_data(resource);
}

static void leme_decoration_handle_set_offset(struct wl_client *client,
                                              struct wl_resource *resource,
                                              int32_t x, int32_t y) {
  struct leme_decoration *decoration = leme_decoration_from_resource(resource);

  (void)client;
  decoration->pending.x = x;
  decoration->pending.y = y;
}

static void leme_decoration_handle_set_margins(struct wl_client *client,
                                               struct wl_resource *resource,
                                               int32_t top, int32_t right,
                                               int32_t bottom, int32_t left) {
  struct leme_decoration *decoration = leme_decoration_from_resource(resource);

  (void)client;
  if (top < 0 || right < 0 || bottom < 0 || left < 0) {
    wl_resource_post_error(resource, LEME_DECORATION_V1_ERROR_INVALID_MARGINS,
                           "margins must not be negative");
    return;
  }
  decoration->pending.margins = (struct leme_gaps){
      .top = top, .right = right, .bottom = bottom, .left = left};
}

static void leme_decoration_handle_set_states(struct wl_client *client,
                                              struct wl_resource *resource,
                                              uint32_t states) {
  struct leme_decoration *decoration = leme_decoration_from_resource(resource);

  (void)client;
  decoration->pending.states = states & LEME_DECORATION_STATES_ALL;
}

static void leme_decoration_handle_set_border(struct wl_client *client,
                                              struct wl_resource *resource,
                                              uint32_t border) {
  struct leme_decoration *decoration = leme_decoration_from_resource(resource);

  (void)client;
  decoration->pending.replace_border =
      border == LEME_DECORATION_V1_BORDER_REPLACED;
}

static const struct leme_decoration_v1_interface
    leme_decoration_implementation = {
        .destroy = leme_decoration_handle_destroy,
        .set_offset = leme_decoration_handle_set_offset,
        .set_margins = leme_decoration_handle_set_margins,
        .set_states = leme_decoration_handle_set_states,
        .set_border = leme_decoration_handle_set_border,
};

static void leme_decoration_role_commit(struct wlr_surface *surface) {
  struct leme_decoration *decoration =
      leme_decoration_from_resource(surface->role_resource);

  decoration->current = decoration->pending;
  if (wlr_surface_has_buffer(surface)) {
    wlr_surface_map(surface);
  } else {
    wlr_surface_unmap(surface);
  }
  if (decoration->window != NULL) {
    leme_decoration_window_update(decoration->window);
  }
}

static void leme_decoration_role_destroy(struct wlr_surface *surface) {
  struct leme_decoration *decoration =
      leme_decoration_from_resource(surface->role_resource);
  struct leme_decoration_window *window = decoration->window;

  leme_decoration_detach(decoration);
  decoration->surface = NULL;
  if (window != NULL) {
    leme_decoration_window_update(window);
  }
}

static const struct wlr_surface_role leme_decoration_role = {
    .name = "leme_decoration_v1",
    .commit = leme_decoration_role_commit,
    .destroy = leme_decoration_role_destroy,
};

static void
leme_decoration_handle_resource_destroy(struct wl_resource *resource) {
  struct leme_decoration *decoration = leme_decoration_from_resource(resource);

  wl_list_remove(&decoration->link);
  free(decoration);
}

static void leme_decoration_window_handle_get_decoration(
    struct wl_client *client, struct wl_resource *resource, uint32_t id,
    struct wl_resource *surface_resource, uint32_t layer) {
  struct leme_decoration_window *window = wl_resource_get_user_data(resource);
  struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);

  if (wlr_surface_has_buffer(surface)) {
    wl_resource_post_error(resource, LEME_DECORATION_WINDOW_V1_ERROR_ROLE,
                           "the surface already has a buffer");
    return;
  }
  if (!wlr_surface_set_role(surface, &leme_decoration_role, resource,
                            LEME_DECORATION_WINDOW_V1_ERROR_ROLE)) {
    return;
  }
  struct leme_decoration *decoration = calloc(1, sizeof(*decoration));
  if (decoration == NULL) {
    wl_client_post_no_memory(client);
    return;
  }
  decoration->resource =
      wl_resource_create(client, &leme_decoration_v1_interface,
                         wl_resource_get_version(resource), id);
  if (decoration->resource == NULL) {
    free(decoration);
    wl_client_post_no_memory(client);
    return;
  }
  wl_resource_set_implementation(decoration->resource,
                                 &leme_decoration_implementation, decoration,
                                 leme_decoration_handle_resource_destroy);
  decoration->surface = surface;
  decoration->below = layer == LEME_DECORATION_WINDOW_V1_LAYER_BELOW;
  decoration->pending.states = LEME_DECORATION_STATES_DEFAULT;
  decoration->current = decoration->pending;
  wl_list_init(&decoration->link);
  wlr_surface_set_role_object(surface, decoration->resource);
  struct wlr_scene_tree *parent =
      window->view != NULL        ? window->view->render_tree
      : window->container != NULL ? window->container->render_tree
                                  : NULL;
  if (parent == NULL) {
    return;
  }
  decoration->tree = wlr_scene_subsurface_tree_create(parent, surface);
  if (decoration->tree == NULL) {
    wl_client_post_no_memory(client);
    return;
  }
  decoration->tree_destroy.notify = leme_decoration_handle_tree_destroy;
  wl_signal_add(&decoration->tree->node.events.destroy,
                &decoration->tree_destroy);
  wlr_scene_node_set_enabled(&decoration->tree->node, false);
  decoration->window = window;
  wl_list_insert(window->decorations.prev, &decoration->link);
}

static struct leme_decoration_window *
leme_decoration_window_target(struct wl_resource *resource) {
  struct leme_decoration_window *window = wl_resource_get_user_data(resource);
  const struct leme_server *server = leme_decoration_window_server(window);

  if (server == NULL || leme_session_locked(server) ||
      (window->view != NULL &&
       !leme_ownership_publication_eligible(window->view))) {
    return NULL;
  }
  return window;
}

static struct wlr_seat_client *
leme_decoration_seat_client(const struct leme_decoration_window *window,
                            struct wl_resource *seat) {
  struct wlr_seat_client *client = wlr_seat_client_from_resource(seat);

  return client != NULL &&
                 client->seat == leme_decoration_window_server(window)->seat
             ? client
             : NULL;
}

static struct wlr_surface *leme_decoration_window_pointer_origin(
    const struct leme_decoration_window *window) {
  struct wlr_surface *focused = leme_decoration_window_server(window)
                                    ->seat->pointer_state.focused_surface;
  const struct leme_decoration *decoration;

  if (focused == NULL) {
    return NULL;
  }
  wl_list_for_each(decoration, &window->decorations, link) {
    if (decoration->surface == wlr_surface_get_root_surface(focused)) {
      return focused;
    }
  }
  return NULL;
}

static void leme_decoration_window_grab(struct wl_resource *resource,
                                        struct wl_resource *seat,
                                        uint32_t serial, bool resize,
                                        uint32_t edges) {
  const struct leme_decoration_window *window =
      leme_decoration_window_target(resource);
  bool started = false;

  if (window != NULL && leme_decoration_seat_client(window, seat) != NULL) {
    struct wlr_surface *origin = leme_decoration_window_pointer_origin(window);
    started = window->container != NULL
                  ? leme_input_pointer_grab_start_container_surface(
                        window->container, origin, resize, serial, edges)
                  : leme_input_pointer_grab_start_surface(
                        window->view, origin, resize, serial, edges);
  }
  if (!started) {
    wlr_log(WLR_DEBUG, "%s", "leme: rejected a decoration grab");
  }
}

static void leme_decoration_window_handle_move(struct wl_client *client,
                                               struct wl_resource *resource,
                                               struct wl_resource *seat,
                                               uint32_t serial) {
  (void)client;
  leme_decoration_window_grab(resource, seat, serial, false, WLR_EDGE_NONE);
}

static void leme_decoration_window_handle_resize(struct wl_client *client,
                                                 struct wl_resource *resource,
                                                 struct wl_resource *seat,
                                                 uint32_t serial,
                                                 uint32_t edges) {
  const uint32_t valid =
      WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT;

  (void)client;
  if ((edges & valid) != 0) {
    leme_decoration_window_grab(resource, seat, serial, true, edges & valid);
  }
}

static void leme_decoration_window_handle_show_window_menu(
    struct wl_client *client, struct wl_resource *resource,
    struct wl_resource *seat, uint32_t serial, int32_t x, int32_t y) {
  const struct leme_decoration_window *window =
      leme_decoration_window_target(resource);
  struct wlr_seat_client *seat_client =
      window == NULL ? NULL : leme_decoration_seat_client(window, seat);

  (void)client;
  if (seat_client == NULL ||
      !wlr_seat_client_validate_event_serial(seat_client, serial)) {
    return;
  }
  const struct leme_container *container =
      window->container != NULL ? window->container : window->view->container;
  struct leme_box content = {0};
  if (window->container != NULL) {
    content = leme_container_content_box(container, container->box);
  } else if (window->view->fullscreen) {
    return;
  } else {
    content = leme_render_view_content_box(window->view, window->view->box);
  }
  const struct leme_desk_target target =
      container != NULL
          ? (struct leme_desk_target){LEME_DESK_TARGET_CONTAINER, container->id}
          : (struct leme_desk_target){LEME_DESK_TARGET_VIEW,
                                      window->view->public_meta.id};
  (void)leme_desk_menu_open(leme_decoration_window_server(window), target,
                            (double)content.x + (double)x,
                            (double)content.y + (double)y);
}

static void leme_decoration_window_handle_close(struct wl_client *client,
                                                struct wl_resource *resource) {
  struct leme_decoration_window *window =
      leme_decoration_window_target(resource);

  (void)client;
  if (window != NULL && window->container != NULL) {
    (void)leme_container_close(window->container);
  } else if (window != NULL) {
    leme_view_protocol_close(window->view);
  }
}

static void
leme_decoration_window_handle_set_minimized(struct wl_client *client,
                                            struct wl_resource *resource) {
  struct leme_decoration_window *window =
      leme_decoration_window_target(resource);

  (void)client;
  if (window != NULL && window->container != NULL) {
    (void)leme_container_set_minimized(window->container, true);
  } else if (window != NULL) {
    (void)leme_view_set_minimized(window->view, true);
  }
}

static void leme_decoration_window_maximize(struct wl_resource *resource,
                                            bool maximized) {
  struct leme_decoration_window *window =
      leme_decoration_window_target(resource);

  if (window != NULL && window->container != NULL) {
    (void)leme_container_set_placement(window->container,
                                       maximized ? LEME_VIEW_PLACEMENT_MAXIMIZED
                                                 : LEME_VIEW_PLACEMENT_NORMAL);
  } else if (window != NULL) {
    (void)leme_view_set_maximized(window->view, maximized);
  }
}

static void
leme_decoration_window_handle_set_maximized(struct wl_client *client,
                                            struct wl_resource *resource) {
  (void)client;
  leme_decoration_window_maximize(resource, true);
}

static void
leme_decoration_window_handle_unset_maximized(struct wl_client *client,
                                              struct wl_resource *resource) {
  (void)client;
  leme_decoration_window_maximize(resource, false);
}

static void leme_decoration_window_handle_ack_configure(
    struct wl_client *client, struct wl_resource *resource, uint32_t serial) {
  struct leme_decoration_window *window = wl_resource_get_user_data(resource);

  (void)client;
  if (window->first_sent && serial - window->first_serial < 0x80000000U) {
    window->acked = true;
  }
  if (window->view != NULL && window->view->open_animation_pending) {
    leme_render_view_open_ready(window->view);
  }
}

static const struct leme_decoration_window_v1_interface
    leme_decoration_window_implementation = {
        .destroy = leme_decoration_handle_destroy,
        .get_decoration = leme_decoration_window_handle_get_decoration,
        .move = leme_decoration_window_handle_move,
        .resize = leme_decoration_window_handle_resize,
        .show_window_menu = leme_decoration_window_handle_show_window_menu,
        .close = leme_decoration_window_handle_close,
        .set_minimized = leme_decoration_window_handle_set_minimized,
        .set_maximized = leme_decoration_window_handle_set_maximized,
        .unset_maximized = leme_decoration_window_handle_unset_maximized,
        .ack_configure = leme_decoration_window_handle_ack_configure,
};

static bool leme_decoration_replace(char **stored, const char *value) {
  if (value == NULL) {
    value = "";
  }
  if (*stored != NULL && strcmp(*stored, value) == 0) {
    return false;
  }
  char *copy = strdup(value);
  if (copy == NULL) {
    return false;
  }
  free(*stored);
  *stored = copy;
  return true;
}

static uint32_t leme_decoration_view_flags(const struct leme_view *view) {
  uint32_t state = 0;

  if (view->server->focused_view == view) {
    state |= LEME_DECORATION_WINDOW_V1_STATE_ACTIVATED;
  }
  if (leme_view_is_maximized(view)) {
    state |= LEME_DECORATION_WINDOW_V1_STATE_MAXIMIZED;
  }
  if (view->fullscreen) {
    state |= LEME_DECORATION_WINDOW_V1_STATE_FULLSCREEN;
  }
  if (leme_view_geometry_owner(view) == LEME_VIEW_GEOMETRY_TAG_LAYOUT) {
    state |= LEME_DECORATION_WINDOW_V1_STATE_TILED;
  }
  if (!leme_desktop_view_server_decorated(view)) {
    state |= LEME_DECORATION_WINDOW_V1_STATE_CLIENT_DECORATED;
  }
  if (view->container != NULL) {
    state |= LEME_DECORATION_WINDOW_V1_STATE_GROUPED;
  }
  if (leme_decoration_view_on_desktop(view)) {
    state |= LEME_DECORATION_WINDOW_V1_STATE_DESKTOP;
  }
  return state;
}

static uint32_t
leme_decoration_container_flags(const struct leme_container *container) {
  const struct leme_server *server = container->tag->owner->server;
  uint32_t state = LEME_DECORATION_WINDOW_V1_STATE_GROUP;

  if (leme_container_for_view(server->focused_view) == container) {
    state |= LEME_DECORATION_WINDOW_V1_STATE_ACTIVATED;
  }
  if (container->placement.kind == LEME_VIEW_PLACEMENT_MAXIMIZED) {
    state |= LEME_DECORATION_WINDOW_V1_STATE_MAXIMIZED;
  }
  if (container->tag->layout.kind == LEME_LAYOUT_DESKTOP) {
    state |= LEME_DECORATION_WINDOW_V1_STATE_DESKTOP;
  }
  return state;
}

struct leme_decoration_target {
  struct leme_view *view;
  struct leme_container *container;
  const char *title;
  const char *app_id;
  uint32_t state;
  struct leme_box content;
};

static struct leme_decoration_target
leme_decoration_view_target(struct leme_view *view) {
  return (struct leme_decoration_target){
      .view = view,
      .title = leme_view_title(view),
      .app_id = leme_view_identity(view),
      .state = leme_decoration_view_flags(view),
      .content = leme_render_view_content_box(view, view->box),
  };
}

static struct leme_decoration_target
leme_decoration_container_target(struct leme_container *container) {
  return (struct leme_decoration_target){
      .container = container,
      .title = leme_view_title(container->focused),
      .app_id = leme_view_identity(container->focused),
      .state = leme_decoration_container_flags(container),
      .content = leme_container_content_box(container, container->box),
  };
}

static void
leme_decoration_window_send_id(struct leme_decoration_window *window,
                               const struct leme_decoration_target *target,
                               const struct leme_server *server) {
  char id[96] = {0};
  const struct leme_public_text instance =
      leme_public_model_instance(server->public_model);
  int written = -1;

  if (instance.data != NULL && target->view != NULL &&
      target->view->public_meta.id.serial != 0) {
    written = snprintf(id, sizeof(id), "v:%s:%016" PRIx64, instance.data,
                       target->view->public_meta.id.serial);
  } else if (instance.data != NULL && target->container != NULL) {
    written = snprintf(id, sizeof(id), "container:%s:%" PRIu64, instance.data,
                       target->container->id.serial);
  }
  if (written < 0 || (size_t)written >= sizeof(id)) {
    id[0] = '\0';
  }
  leme_decoration_window_v1_send_id(window->resource, id);
}

static struct leme_decoration_window *
leme_decoration_window_create(struct leme_decoration_protocol *protocol,
                              const struct leme_decoration_target *target) {
  struct wl_client *client = wl_resource_get_client(protocol->active);
  struct leme_decoration_window *window = calloc(1, sizeof(*window));

  if (window == NULL) {
    wl_client_post_no_memory(client);
    return NULL;
  }
  window->resource =
      wl_resource_create(client, &leme_decoration_window_v1_interface,
                         wl_resource_get_version(protocol->active), 0);
  if (window->resource == NULL) {
    free(window);
    wl_client_post_no_memory(client);
    return NULL;
  }
  wl_resource_set_implementation(
      window->resource, &leme_decoration_window_implementation, window,
      leme_decoration_window_handle_resource_destroy);
  window->view = target->view;
  window->container = target->container;
  wl_list_init(&window->decorations);
  window->width = -1;
  window->height = -1;
  wl_list_insert(protocol->windows.prev, &window->link);
  if (target->view != NULL) {
    target->view->decoration_window = window;
  } else {
    target->container->decoration_window = window;
  }
  leme_decoration_manager_v1_send_window(protocol->active, window->resource);
  leme_decoration_window_send_id(window, target, protocol->server);
  return window;
}

static void leme_decoration_sync(struct leme_decoration_protocol *protocol,
                                 const struct leme_decoration_target *target) {
  struct leme_decoration_window *window =
      target->view != NULL ? target->view->decoration_window
                           : target->container->decoration_window;
  bool changed = false;

  if (window == NULL) {
    window = leme_decoration_window_create(protocol, target);
    if (window == NULL) {
      return;
    }
    changed = true;
  } else if (window->resource == NULL) {
    return;
  }
  if (leme_decoration_replace(&window->title, target->title)) {
    leme_decoration_window_v1_send_title(window->resource, window->title);
    changed = true;
  }
  if (leme_decoration_replace(&window->app_id, target->app_id)) {
    leme_decoration_window_v1_send_app_id(window->resource, window->app_id);
    changed = true;
  }
  if (changed || target->state != window->state) {
    window->state = target->state;
    leme_decoration_window_v1_send_state(window->resource, target->state);
    changed = true;
  }
  const struct leme_box content = target->content;
  if (content.width != window->width || content.height != window->height) {
    window->width = content.width;
    window->height = content.height;
    leme_decoration_window_v1_send_size(window->resource, content.width,
                                        content.height);
    changed = true;
  }
  if (changed) {
    window->serial = wl_display_next_serial(protocol->server->display);
    if (!window->first_sent) {
      window->first_serial = window->serial;
      window->first_sent = true;
    }
    leme_decoration_window_v1_send_done(window->resource, window->serial);
  }
  leme_decoration_window_update(window);
}

static bool
leme_decoration_container_eligible(const struct leme_container *container) {
  return container->render_tree != NULL && container->focused != NULL &&
         container->count > 1;
}

static void leme_decoration_reconcile_containers(
    struct leme_decoration_protocol *protocol) {
  struct leme_output *output;

  wl_list_for_each(output, &protocol->server->outputs, link) {
    if (output->tags.table == NULL) {
      continue;
    }
    for (uint16_t id = 1; id <= output->tags.max_tags; id++) {
      struct leme_tag *tag = output->tags.table[id];
      struct leme_container *container;
      if (tag == NULL) {
        continue;
      }
      wl_list_for_each(container, &tag->containers, link) {
        if (leme_decoration_container_eligible(container)) {
          const struct leme_decoration_target target =
              leme_decoration_container_target(container);
          leme_decoration_sync(protocol, &target);
        } else if (container->decoration_window != NULL) {
          leme_decoration_window_close(container->decoration_window, true);
        }
      }
    }
  }
}

static void
leme_decoration_protocol_reconcile(struct leme_decoration_protocol *protocol) {
  struct leme_view *view;

  if (protocol->active == NULL) {
    return;
  }
  wl_list_for_each(view, &protocol->server->views, link) {
    if (leme_ownership_publication_eligible(view)) {
      const struct leme_decoration_target target =
          leme_decoration_view_target(view);
      leme_decoration_sync(protocol, &target);
    } else if (view->decoration_window != NULL) {
      leme_decoration_window_close(view->decoration_window, true);
    }
  }
  leme_decoration_reconcile_containers(protocol);
}

static void leme_decoration_protocol_handle_idle(void *data) {
  struct leme_decoration_protocol *protocol = data;

  protocol->idle = NULL;
  leme_decoration_protocol_reconcile(protocol);
}

static void
leme_decoration_protocol_deactivate(struct leme_decoration_protocol *protocol,
                                    bool refresh) {
  struct leme_decoration_window *window;
  struct leme_decoration_window *next;

  wl_list_for_each_safe(window, next, &protocol->windows, link) {
    leme_decoration_window_detach(window, refresh);
  }
  protocol->active = NULL;
  if (protocol->idle != NULL) {
    wl_event_source_remove(protocol->idle);
    protocol->idle = NULL;
  }
}

static void
leme_decoration_manager_handle_resource_destroy(struct wl_resource *resource) {
  struct leme_decoration_protocol *protocol =
      wl_resource_get_user_data(resource);

  if (protocol != NULL && protocol->active == resource) {
    leme_decoration_protocol_deactivate(protocol, true);
  }
}

static const struct leme_decoration_manager_v1_interface
    leme_decoration_manager_implementation = {
        .destroy = leme_decoration_handle_destroy,
};

static void leme_decoration_protocol_handle_bind(struct wl_client *client,
                                                 void *data, uint32_t version,
                                                 uint32_t id) {
  struct leme_decoration_protocol *protocol = data;
  struct wl_resource *resource = wl_resource_create(
      client, &leme_decoration_manager_v1_interface, (int)version, id);

  if (resource == NULL) {
    wl_client_post_no_memory(client);
    return;
  }
  if (protocol->active != NULL) {
    wl_resource_set_implementation(
        resource, &leme_decoration_manager_implementation, NULL, NULL);
    leme_decoration_manager_v1_send_unavailable(resource);
    return;
  }
  wl_resource_set_implementation(
      resource, &leme_decoration_manager_implementation, protocol,
      leme_decoration_manager_handle_resource_destroy);
  protocol->active = resource;
  leme_decoration_protocol_reconcile(protocol);
}

bool leme_decoration_protocol_init(struct leme_server *server) {
  struct leme_decoration_protocol *protocol = calloc(1, sizeof(*protocol));

  if (protocol == NULL) {
    return false;
  }
  protocol->server = server;
  wl_list_init(&protocol->windows);
  protocol->global = wl_global_create(
      server->display, &leme_decoration_manager_v1_interface,
      LEME_DECORATION_VERSION, protocol, leme_decoration_protocol_handle_bind);
  if (protocol->global == NULL) {
    free(protocol);
    return false;
  }
  server->decoration_protocol = protocol;
  return true;
}

void leme_decoration_protocol_finish(struct leme_server *server) {
  struct leme_decoration_protocol *protocol = server->decoration_protocol;

  if (protocol == NULL) {
    return;
  }
  if (protocol->active != NULL) {
    wl_resource_set_user_data(protocol->active, NULL);
    leme_decoration_protocol_deactivate(protocol, false);
  }
  wl_global_destroy(protocol->global);
  free(protocol);
  server->decoration_protocol = NULL;
}

void leme_decoration_protocol_schedule(struct leme_server *server) {
  struct leme_decoration_protocol *protocol =
      server == NULL ? NULL : server->decoration_protocol;

  if (protocol == NULL || protocol->active == NULL || protocol->idle != NULL) {
    return;
  }
  protocol->idle =
      wl_event_loop_add_idle(wl_display_get_event_loop(server->display),
                             leme_decoration_protocol_handle_idle, protocol);
}

bool leme_decoration_protocol_holds_open(const struct leme_view *view) {
  const struct leme_decoration_protocol *protocol =
      view == NULL || view->server == NULL ? NULL
                                           : view->server->decoration_protocol;

  if (protocol == NULL || protocol->active == NULL ||
      !leme_ownership_publication_eligible(view)) {
    return false;
  }
  const struct leme_decoration_window *window = view->decoration_window;
  return window == NULL || (window->resource != NULL && !window->acked);
}

size_t leme_decoration_protocol_trees(const struct leme_view *view,
                                      struct wlr_scene_tree **trees,
                                      size_t capacity) {
  return view == NULL
             ? 0
             : leme_decoration_window_trees(view->decoration_window,
                                            view->fullscreen, trees, capacity);
}

void leme_decoration_protocol_container_destroy(
    struct leme_container *container) {
  if (container != NULL && container->decoration_window != NULL) {
    leme_decoration_window_close(container->decoration_window, false);
  }
}

void leme_decoration_protocol_view_destroy(struct leme_view *view) {
  if (view != NULL && view->decoration_window != NULL) {
    leme_decoration_window_close(view->decoration_window, false);
  }
}
