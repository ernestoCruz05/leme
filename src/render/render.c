#include "render/render.h"
#include "desktop/decoration.h"
#include "render/timing.h"
#include "desktop/desktop.h"
#include "desktop/strip.h"

#include "protocols/capture.h"
#include "protocols/data.h"
#include "shell/layer.h"
#include "output/output.h"
#include "core/server.h"
#include "input/input.h"
#include "protocols/tearing.h"
#include "shell/view.h"
#include "shell/container.h"
#include "desktop/frame.h"

#include <limits.h>
#include <math.h>
#include <time.h>
#include <wlr/render/swapchain.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>

static void
leme_render_handle_output_layout_change(struct wl_listener *listener,
                                        void *data) {
  struct leme_server *server =
      wl_container_of(listener, server, render_output_layout_change);
  struct leme_output *output;

  (void)data;
  if (server->outputs.next == NULL) {
    return;
  }
  wl_list_for_each(output, &server->outputs, link) {
    struct wlr_output_layout_output *layout_output;

    if (output->scene_output == NULL) {
      continue;
    }
    layout_output =
        wlr_output_layout_get(server->output_layout, output->wlr_output);
    if (layout_output != NULL) {
      wlr_scene_output_set_position(output->scene_output, layout_output->x,
                                    layout_output->y);
    }
  }
}

void leme_render_init(struct leme_server *server) {
  server->scene = wlr_scene_create();
  if (server->scene == NULL) {
    return;
  }
  server->scene_output_layout =
      wlr_scene_attach_output_layout(server->scene, server->output_layout);
  server->scene_background = wlr_scene_tree_create(&server->scene->tree);
  server->scene_desktop = wlr_scene_tree_create(&server->scene->tree);
  server->scene_bottom = wlr_scene_tree_create(&server->scene->tree);
  server->scene_tiled = wlr_scene_tree_create(&server->scene->tree);
  server->scene_floating = wlr_scene_tree_create(&server->scene->tree);
  server->scene_minimized = wlr_scene_tree_create(&server->scene->tree);
  server->scene_durable = wlr_scene_tree_create(&server->scene->tree);
  server->scene_top = wlr_scene_tree_create(&server->scene->tree);
  server->scene_fullscreen = wlr_scene_tree_create(&server->scene->tree);
  server->scene_unmanaged = wlr_scene_tree_create(&server->scene->tree);
  server->scene_overlay = wlr_scene_tree_create(&server->scene->tree);
  server->scene_drag = wlr_scene_tree_create(&server->scene->tree);
  server->scene_lock = wlr_scene_tree_create(&server->scene->tree);
  if (server->scene_output_layout == NULL || server->scene_background == NULL ||
      server->scene_bottom == NULL || server->scene_tiled == NULL ||
      server->scene_floating == NULL || server->scene_durable == NULL ||
      server->scene_top == NULL || server->scene_fullscreen == NULL ||
      server->scene_unmanaged == NULL || server->scene_overlay == NULL ||
      server->scene_drag == NULL || server->scene_lock == NULL) {
    leme_render_finish(server);
    return;
  }
  server->render_output_layout_change.notify =
      leme_render_handle_output_layout_change;
  wl_signal_add(&server->output_layout->events.change,
                &server->render_output_layout_change);
}

void leme_render_finish(struct leme_server *server) {
  if (server->render_output_layout_change.link.next != NULL) {
    wl_list_remove(&server->render_output_layout_change.link);
    server->render_output_layout_change.link.next = NULL;
    server->render_output_layout_change.link.prev = NULL;
  }
  if (server->scene != NULL) {
    wlr_scene_node_destroy(&server->scene->tree.node);
  }
  server->scene = NULL;
  server->scene_output_layout = NULL;
  server->scene_background = NULL;
  server->scene_desktop = NULL;
  server->scene_bottom = NULL;
  server->scene_tiled = NULL;
  server->scene_floating = NULL;
  server->scene_minimized = NULL;
  server->scene_durable = NULL;
  server->scene_top = NULL;
  server->scene_fullscreen = NULL;
  server->scene_unmanaged = NULL;
  server->scene_overlay = NULL;
  server->scene_drag = NULL;
  server->scene_lock = NULL;
}

struct wlr_scene_tree *
leme_render_durable_group_create(struct leme_server *server) {
  return server == NULL || server->scene_durable == NULL
             ? NULL
             : wlr_scene_tree_create(server->scene_durable);
}

void leme_render_durable_group_raise(struct wlr_scene_tree *group) {
  if (group != NULL) {
    wlr_scene_node_raise_to_top(&group->node);
  }
}

void leme_render_durable_group_destroy(struct wlr_scene_tree **group) {
  if (group == NULL || *group == NULL) {
    return;
  }
  wlr_scene_node_destroy(&(*group)->node);
  *group = NULL;
}

bool leme_render_prepare_output_attachment(struct leme_output *output) {
  if (output->scene_output != NULL) {
    return true;
  }
  if (output->server->scene == NULL) {
    return false;
  }
  output->scene_output =
      wlr_scene_output_create(output->server->scene, output->wlr_output);
  return output->scene_output != NULL;
}

bool leme_render_attach_output(struct leme_output *output) {
  struct leme_server *server = output->server;
  struct wlr_output_layout_output *layout_output;

  if (!leme_render_prepare_output_attachment(output)) {
    return false;
  }
  layout_output =
      wlr_output_layout_get(server->output_layout, output->wlr_output);
  if (layout_output == NULL) {
    return false;
  }
  if (server->scene_output_layout != NULL) {
    wlr_scene_output_layout_add_output(server->scene_output_layout,
                                       layout_output, output->scene_output);
  }
  if (output->render_timing == NULL) {
    output->render_timing =
        leme_render_timing_create(server->render_timing_seconds);
  }
  return true;
}

void leme_render_position_output(struct leme_output *output) {
  if (output->scene_output != NULL) {
    wlr_scene_output_set_position(output->scene_output, output->layout_x,
                                  output->layout_y);
  }
}

void leme_render_detach_output(struct leme_output *output) {
  leme_desk_strip_output_finish(output);
  leme_desk_output_finish(output);
  leme_render_timing_destroy(output->render_timing);
  output->render_timing = NULL;
  if (output->scene_output != NULL) {
    wlr_scene_output_destroy(output->scene_output);
    output->scene_output = NULL;
  }
}

static bool
leme_render_prepare_isolated_output_state(struct leme_output *output,
                                          struct wlr_output_state *state) {
  struct wlr_scene *scene = wlr_scene_create();
  struct wlr_scene_output *scene_output = NULL;
  struct wlr_swapchain *swapchain = NULL;
  bool valid = false;

  if (scene == NULL) {
    return false;
  }
  scene_output = wlr_scene_output_create(scene, output->wlr_output);
  if (scene_output == NULL || !wlr_output_configure_primary_swapchain(
                                  output->wlr_output, state, &swapchain)) {
    goto cleanup;
  }
  valid = wlr_scene_output_build_state(scene_output, state,
                                       &(struct wlr_scene_output_state_options){
                                           .swapchain = swapchain,
                                       });

cleanup:
  if (scene_output != NULL) {
    wlr_scene_output_destroy(scene_output);
  }
  wlr_scene_node_destroy(&scene->tree.node);
  wlr_swapchain_destroy(swapchain);
  return valid;
}

bool leme_render_prepare_output_state(struct leme_output *output,
                                      struct wlr_output_state *state,
                                      bool testing) {
  bool enabled = output->wlr_output->enabled;

  if (state->committed & WLR_OUTPUT_STATE_ENABLED) {
    enabled = state->enabled;
  }
  if (!enabled) {
    return true;
  }
  if (testing || output->scene_output == NULL) {
    return leme_render_prepare_isolated_output_state(output, state);
  }
  return wlr_scene_output_build_state(output->scene_output, state, NULL);
}

void leme_render_handle_session_active(struct leme_server *server,
                                       bool active) {
  if (server != NULL && !active) {
    leme_input_workspace_gesture_cancel(server);
    leme_animation_manager_finish_all(&server->animations);
  }
}

void leme_render_output_frame(struct leme_output *output) {
  struct wlr_output_state state;
  struct timespec now;
  bool request_tearing;
  bool committed;

  if (output->scene_output == NULL) {
    return;
  }
  leme_capture_sync(output->server);
  leme_view_sync_suspended(output->server);
  leme_view_flush_deferred_configures(output->server);
  clock_gettime(CLOCK_MONOTONIC, &now);
  leme_animation_manager_tick(&output->server->animations, &now);
  if (leme_animation_manager_active_for_owner(&output->server->animations,
                                              output)) {
    wlr_output_schedule_frame(output->wlr_output);
  }
  if (!output->tearing_fallback &&
      !wlr_scene_output_needs_frame(output->scene_output)) {
    return;
  }
  struct wlr_scene_timer *timer =
      leme_render_timing_begin(output->render_timing, output->wlr_output->name);
  struct timespec build_start;
  clock_gettime(CLOCK_MONOTONIC, &build_start);
  wlr_output_state_init(&state);
  const bool built = wlr_scene_output_build_state(
      output->scene_output, &state,
      timer == NULL ? NULL
                    : &(struct wlr_scene_output_state_options){.timer = timer});
  leme_render_timing_end(output->render_timing, &build_start, built);
  if (!built) {
    wlr_output_state_finish(&state);
    return;
  }
  request_tearing = !output->tearing_fallback &&
                    leme_tearing_can_tear(output->server, output);
  if (output->tearing_fallback) {
    output->tearing_fallback = false;
  }
  if (request_tearing) {
    state.tearing_page_flip = true;
    if (!wlr_output_test_state(output->wlr_output, &state)) {
      state.tearing_page_flip = false;
    }
  }
  committed = wlr_output_commit_state(output->wlr_output, &state);
  if (!committed && state.tearing_page_flip) {
    output->tearing_fallback = true;
    wlr_output_schedule_frame(output->wlr_output);
  }
  wlr_output_state_finish(&state);
  if (!committed) {
    return;
  }
  clock_gettime(CLOCK_MONOTONIC, &now);
  wlr_scene_output_send_frame_done(output->scene_output, &now);
}

bool leme_render_drag_icon_create(struct leme_drag_icon *icon) {
  icon->scene_tree = wlr_scene_drag_icon_create(icon->data->server->scene_drag,
                                                icon->wlr_drag_icon);
  return icon->scene_tree != NULL;
}

void leme_render_drag_icon_update(struct leme_drag_icon *icon, double layout_x,
                                  double layout_y) {
  if (icon->scene_tree != NULL) {
    wlr_scene_node_set_position(&icon->scene_tree->node, (int)layout_x,
                                (int)layout_y);
  }
}

void leme_render_drag_icon_destroy(struct leme_drag_icon *icon) {
  if (icon->scene_tree != NULL) {
    wlr_scene_node_destroy(&icon->scene_tree->node);
    icon->scene_tree = NULL;
  }
}

struct leme_render_visibility {
  struct wlr_surface *root;
  bool visible;
};

static void leme_render_check_visible_buffer(struct wlr_scene_buffer *buffer,
                                             int sx, int sy, void *data) {
  struct leme_render_visibility *visibility = data;
  struct wlr_scene_surface *scene_surface;

  (void)sx;
  (void)sy;
  if (visibility->visible) {
    return;
  }
  scene_surface = wlr_scene_surface_try_from_buffer(buffer);
  if (scene_surface != NULL &&
      wlr_surface_get_root_surface(scene_surface->surface) ==
          visibility->root) {
    visibility->visible = true;
  }
}

bool leme_render_surface_visible(struct leme_server *server,
                                 struct wlr_surface *surface) {
  struct leme_render_visibility visibility;
  struct leme_output *output;

  if (surface == NULL || server->outputs.next == NULL) {
    return false;
  }
  visibility = (struct leme_render_visibility){
      .root = wlr_surface_get_root_surface(surface),
  };
  wl_list_for_each(output, &server->outputs, link) {
    if (output->scene_output == NULL || !output->wlr_output->enabled) {
      continue;
    }
    wlr_scene_output_for_each_buffer(
        output->scene_output, leme_render_check_visible_buffer, &visibility);
    if (visibility.visible) {
      return true;
    }
  }
  return false;
}

static void
leme_render_preview_apply(void *data,
                          const struct leme_animation_frame *frame) {
  struct leme_render_preview *preview = data;
  preview->current = frame->box;
  wlr_scene_rect_set_size(preview->rect, frame->box.width, frame->box.height);
  wlr_scene_node_set_position(&preview->rect->node, frame->box.x, frame->box.y);
}

static void leme_render_preview_done(void *data) {
  struct leme_render_preview *preview = data;
  const struct leme_animation_frame frame = {.box = preview->target};
  leme_render_preview_apply(preview, &frame);
}

bool leme_render_preview_show(struct leme_server *server,
                              struct leme_render_preview *preview,
                              struct leme_box box, double cursor_x,
                              double cursor_y) {
  static const float color[4] = {0.16f, 0.42f, 0.72f, 0.30f};
  if (server == NULL || preview == NULL || server->scene_floating == NULL ||
      box.width <= 0 || box.height <= 0 || !isfinite(cursor_x) ||
      !isfinite(cursor_y) || cursor_x < (double)INT_MIN + 16.0 ||
      cursor_y < (double)INT_MIN + 16.0 || cursor_x > (double)INT_MAX - 16.0 ||
      cursor_y > (double)INT_MAX - 16.0) {
    return false;
  }
  struct leme_box from = preview->current;
  if (preview->rect == NULL) {
    preview->rect =
        wlr_scene_rect_create(server->scene_floating, 32, 32, color);
    if (preview->rect == NULL) {
      return false;
    }
    from = (struct leme_box){(int)cursor_x - 16, (int)cursor_y - 16, 32, 32};
  } else if (preview->target.x == box.x && preview->target.y == box.y &&
             preview->target.width == box.width &&
             preview->target.height == box.height) {
    wlr_scene_node_raise_to_top(&preview->rect->node);
    return true;
  }
  leme_animation_manager_finish_data(&server->animations, preview);
  preview->target = box;
  const struct leme_animation_settings *settings =
      server->config == NULL ? NULL
                             : &server->config->animation[LEME_ANIMATION_MOVE];
  if (settings == NULL || !settings->configured) {
    leme_render_preview_done(preview);
  } else {
    const struct leme_animation_spec spec =
        leme_animation_move_spec(settings, from, box, 1.0, 1.0);
    const struct leme_animation_subject subject = {
        .data = preview,
        .apply = leme_render_preview_apply,
        .done = leme_render_preview_done};
    const struct leme_animation_frame initial = {.box = from};
    leme_render_preview_apply(preview, &initial);
    leme_animation_run(&server->animations,
                       wlr_scene_tree_create(server->scene_floating), &spec,
                       &subject);
  }
  wlr_scene_node_raise_to_top(&preview->rect->node);
  return true;
}

void leme_render_preview_hide(struct leme_server *server,
                              struct leme_render_preview *preview) {
  if (preview == NULL) {
    return;
  }
  if (server != NULL) {
    leme_animation_manager_finish_data(&server->animations, preview);
  }
  if (preview->rect != NULL) {
    wlr_scene_node_destroy(&preview->rect->node);
  }
  *preview = (struct leme_render_preview){0};
}

static bool leme_render_view_layer(const struct leme_server *server,
                                   const struct wlr_scene_tree *tree) {
  return tree == server->scene_tiled || tree == server->scene_floating ||
         tree == server->scene_durable || tree == server->scene_fullscreen;
}

static bool leme_render_drop_skip(const struct leme_server *server,
                                  const struct wlr_scene_node *node,
                                  const struct leme_view *source) {
  if (source == NULL || node->data == NULL ||
      !leme_render_view_layer(server, node->parent)) {
    return false;
  }
  const struct leme_render_owner *owner = node->data;
  if (owner->kind != LEME_RENDER_OWNER_VIEW) {
    return false;
  }
  size_t limit = (size_t)wl_list_length(&server->views);
  const struct leme_view *view = owner->value.view;
  while (view != NULL && limit > 0) {
    if (view == source) {
      return true;
    }
    view = leme_view_parent(view);
    limit--;
  }
  return false;
}

static struct wlr_scene_node *
leme_render_node_at(struct leme_server *server, struct wlr_scene_node *root,
                    double lx, double ly, struct leme_render_hit *hit,
                    const struct leme_view *skip,
                    const struct wlr_scene_node *preview) {
  if (!root->enabled || root == preview ||
      (server->scene_drag != NULL && root == &server->scene_drag->node) ||
      leme_render_drop_skip(server, root, skip)) {
    return NULL;
  }
  if (root->type == WLR_SCENE_NODE_TREE) {
    struct wlr_scene_tree *tree = wlr_scene_tree_from_node(root);
    if (tree == &server->scene->tree || leme_render_view_layer(server, tree)) {
      struct wlr_scene_node *child = NULL;
      wl_list_for_each_reverse(child, &tree->children, link) {
        struct wlr_scene_node *node =
            leme_render_node_at(server, child, lx, ly, hit, skip, preview);
        if (node != NULL) {
          return node;
        }
      }
      return NULL;
    }
  }
  struct wlr_scene_node *node =
      wlr_scene_node_at(root, lx, ly, &hit->surface_x, &hit->surface_y);
  if (root->type == WLR_SCENE_NODE_TREE && root->data != NULL &&
      leme_render_view_layer(server, root->parent) &&
      (node == NULL || node->type != WLR_SCENE_NODE_BUFFER ||
       wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node)) ==
           NULL)) {
    const struct leme_render_owner *owner = root->data;
    if (owner->kind == LEME_RENDER_OWNER_CONTAINER) {
      hit->resize_edges =
          leme_render_container_resize_edges(owner->value.container, lx, ly);
      if (hit->resize_edges != 0) {
        hit->surface_x = 0.0;
        hit->surface_y = 0.0;
        return root;
      }
    }
    const struct leme_view *view =
        owner->kind == LEME_RENDER_OWNER_VIEW ? owner->value.view : NULL;
    if (view != NULL && view->render_tree != NULL &&
        root == &view->render_tree->node) {
      hit->resize_edges = leme_render_view_resize_edges(view, lx, ly);
      if (hit->resize_edges != 0) {
        hit->surface_x = 0.0;
        hit->surface_y = 0.0;
        return root;
      }
    }
  }
  return node;
}

static bool leme_render_at_filtered(struct leme_server *server, double lx,
                                    double ly, struct leme_render_hit *hit,
                                    const struct leme_view *skip,
                                    const struct wlr_scene_node *preview) {
  struct wlr_scene_node *node;
  struct wlr_scene_node *ancestor;
  struct wlr_scene_surface *scene_surface;
  void *owner = NULL;

  *hit = (struct leme_render_hit){0};
  node = leme_render_node_at(server, &server->scene->tree.node, lx, ly, hit,
                             skip, preview);
  if (node == NULL) {
    return false;
  }
  if (node->type == WLR_SCENE_NODE_BUFFER) {
    scene_surface =
        wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
    if (scene_surface != NULL) {
      hit->surface = scene_surface->surface;
    }
  }
  for (ancestor = node; ancestor != NULL;
       ancestor = ancestor->parent == NULL ? NULL : &ancestor->parent->node) {
    if (ancestor->data != NULL) {
      owner = ancestor->data;
    }
    if (ancestor->parent == server->scene_minimized) {
      hit->strip_output = owner;
      break;
    }
    if (ancestor->parent == server->scene_desktop) {
      hit->desk_output = owner;
      break;
    }
    if (ancestor->parent == server->scene_tiled ||
        ancestor->parent == server->scene_floating ||
        ancestor->parent == server->scene_durable ||
        ancestor->parent == server->scene_fullscreen ||
        ancestor->parent == server->scene_unmanaged) {
      const struct leme_render_owner *render_owner = owner;
      if (render_owner != NULL &&
          render_owner->kind == LEME_RENDER_OWNER_CONTAINER) {
        hit->container = render_owner->value.container;
        if (hit->surface == NULL && hit->resize_edges == 0) {
          hit->decoration_part =
              leme_desk_frame_hit(&hit->container->frame, node);
        }
        break;
      }
      struct leme_view *view =
          render_owner == NULL ? NULL : render_owner->value.view;
      hit->view = leme_ownership_hit_test_eligible(view) ? view : NULL;
      if (hit->view != NULL && hit->resize_edges == 0) {
        (void)leme_desk_decoration_hit(view, node, hit);
      }
      break;
    }
    if (ancestor->parent == server->scene_background ||
        ancestor->parent == server->scene_bottom ||
        ancestor->parent == server->scene_top ||
        ancestor->parent == server->scene_overlay) {
      struct leme_layer_surface *layer = owner;

      hit->layer = layer != NULL && layer->mapped ? layer : NULL;
      break;
    }
  }
  return hit->surface != NULL;
}

bool leme_render_at(struct leme_server *server, double lx, double ly,
                    struct leme_render_hit *hit) {
  return leme_render_at_filtered(server, lx, ly, hit, NULL, NULL);
}

void leme_render_drop_at(struct leme_server *server,
                         const struct leme_view *source,
                         const struct wlr_scene_node *preview, double lx,
                         double ly, struct leme_render_hit *hit) {
  (void)leme_render_at_filtered(server, lx, ly, hit, source, preview);
}
