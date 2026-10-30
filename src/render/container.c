#include "render/render.h"

#include "config/style.h"
#include "core/server.h"
#include "desktop/decoration.h"
#include "desktop/frame.h"
#include "output/output.h"
#include "shell/container.h"
#include "shell/container-policy.h"
#include "shell/view.h"
#include "workspace/tag.h"

#include <stdlib.h>
#include <wlr/types/wlr_scene.h>

struct leme_render_container_animation {
  struct leme_container *container;
  struct leme_box current;
  struct leme_box target;
  struct wlr_scene_tree *snapshot;
  struct leme_render_view_frame_nodes nodes;
  struct wl_array colors;
  bool flying;
  bool visible;
};

static struct leme_server *
leme_render_container_server(const struct leme_container *container) {
  return container->tag->owner->server;
}

bool leme_render_container_create(struct leme_container *container) {
  struct leme_server *server = leme_render_container_server(container);
  if (server->scene_floating == NULL || server->config == NULL) {
    return false;
  }
  container->render_tree = wlr_scene_tree_create(server->scene_floating);
  if (container->render_tree == NULL) {
    return false;
  }
  container->render_box = container->box;
  container->render_owner = (struct leme_render_owner){
      .kind = LEME_RENDER_OWNER_CONTAINER, .value.container = container};
  container->render_tree->node.data = &container->render_owner;
  wlr_scene_node_set_enabled(&container->render_tree->node, false);
  static const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  for (size_t i = 0; i < LEME_ARRAY_LENGTH(container->border); i++) {
    container->border[i] =
        wlr_scene_rect_create(container->render_tree, 1, 1, color);
    if (container->border[i] == NULL) {
      leme_render_container_destroy(container);
      return false;
    }
  }
  const double scale = leme_output_scale(container->tag->owner->output);
  struct leme_titlebar_style style = {0};
  leme_desk_style_titlebar(server->config, &style);
  if (!leme_desk_frame_init(&container->frame, server, container->render_tree,
                            scale, &style)) {
    leme_render_container_destroy(container);
    return false;
  }
  return true;
}

uint32_t
leme_render_container_resize_edges(const struct leme_container *container,
                                   double lx, double ly) {
  if (container == NULL || container->minimized ||
      container->placement.kind == LEME_VIEW_PLACEMENT_MAXIMIZED ||
      !leme_container_member_eligible(container->focused)) {
    return 0;
  }
  const struct leme_config *config =
      leme_render_container_server(container)->config;
  return leme_desk_decoration_resize_edges(
      container->box, config == NULL ? 0 : config->border_width, lx, ly);
}

void leme_render_container_destroy(struct leme_container *container) {
  leme_render_container_finish_move(container);
  leme_animation_gate_destroy(container->animation_gate);
  container->animation_gate = NULL;
  leme_desk_frame_finish(&container->frame);
  if (container->render_tree != NULL) {
    wlr_scene_node_destroy(&container->render_tree->node);
    container->render_tree = NULL;
    for (size_t i = 0; i < LEME_ARRAY_LENGTH(container->border); i++) {
      container->border[i] = NULL;
    }
  }
}

void leme_render_container_set_visible(struct leme_container *container,
                                       bool visible) {
  if (container->render_tree != NULL) {
    if (container->animation_state != NULL &&
        container->animation_state->flying) {
      container->animation_state->visible = visible;
      visible = false;
    }
    wlr_scene_node_set_enabled(&container->render_tree->node, visible);
  }
}

void leme_render_container_set_activated(struct leme_container *container,
                                         bool activated) {
  if (container == NULL || container->render_tree == NULL ||
      container->frame.tree == NULL) {
    return;
  }
  const struct leme_config *config =
      leme_render_container_server(container)->config;
  static const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  const float *color = config == NULL ? black
                       : activated    ? config->border_active
                                      : config->border_inactive;
  for (size_t i = 0; i < LEME_ARRAY_LENGTH(container->border); i++) {
    wlr_scene_rect_set_color(container->border[i], color);
  }
  leme_desk_frame_set_activated(&container->frame, activated);
  container->frame.available[0] = leme_container_minimize_available(container);
  container->frame.available[1] =
      leme_container_can_place(container, LEME_VIEW_PLACEMENT_MAXIMIZED);
  container->frame.available[2] = true;
  leme_desk_frame_feedback(&container->frame, container->frame.hover_part,
                           container->frame.pressed);
}

static void
leme_render_container_layout_border(struct leme_container *container,
                                    struct leme_box box, int border) {
  const bool shown = border > 0 && (int64_t)box.width > 2 * (int64_t)border &&
                     (int64_t)box.height > 2 * (int64_t)border;
  for (size_t i = 0; i < LEME_ARRAY_LENGTH(container->border); i++) {
    wlr_scene_node_set_enabled(&container->border[i]->node, shown);
  }
  if (!shown) {
    return;
  }
  const int inner = box.height - 2 * border;
  const struct leme_box sides[] = {{0, 0, box.width, border},
                                   {0, box.height - border, box.width, border},
                                   {0, border, border, inner},
                                   {box.width - border, border, border, inner}};
  for (size_t i = 0; i < LEME_ARRAY_LENGTH(container->border); i++) {
    struct wlr_scene_rect *rect = container->border[i];
    wlr_scene_rect_set_size(rect, sides[i].width, sides[i].height);
    wlr_scene_node_set_position(&rect->node, sides[i].x, sides[i].y);
  }
}

void leme_render_container_set_box(struct leme_container *container,
                                   struct leme_box box) {
  if (container == NULL || container->render_tree == NULL) {
    return;
  }
  if (container->animation_state != NULL &&
      !container->animation_state->flying) {
    box = container->animation_state->current;
  }
  container->render_box = box;
  struct leme_server *server = leme_render_container_server(container);
  const double scale = leme_output_scale(container->tag->owner->output);
  const int border = server->config == NULL ? 0 : server->config->border_width;
  const struct leme_box content = leme_container_content_box(container, box);
  wlr_scene_node_set_position(&container->render_tree->node, box.x, box.y);
  leme_render_container_layout_border(container, box, border);
  if (container->frame.controls.scale != scale && server->config != NULL) {
    struct leme_titlebar_style style = {0};
    leme_desk_style_titlebar(server->config, &style);
    (void)leme_desk_frame_set_style(&container->frame, scale, &style);
  }
  const char *title = leme_view_title(container->focused);
  if (title == NULL || title[0] == '\0') {
    title = LEME_CONTAINER_EMPTY_TITLE;
  }
  if (server->desk_decorations_ready) {
    (void)leme_desk_frame_set_title(&container->frame, title, content.width,
                                    scale);
  }
  leme_desk_frame_layout(container->frame.tree, box, border,
                         container->frame.style.height, content.width,
                         &container->frame.style, 0);
  leme_desk_frame_set_maximized(&container->frame,
                                container->placement.kind ==
                                    LEME_VIEW_PLACEMENT_MAXIMIZED);
  leme_render_container_set_activated(
      container, leme_container_for_view(server->focused_view) == container);
}

bool leme_render_container_ready(const struct leme_container *container) {
  if (container == NULL) {
    return true;
  }
  for (size_t i = 0; i < container->count; i++) {
    if (!leme_render_view_move_settled(container->views[i])) {
      return false;
    }
  }
  return true;
}

static bool leme_render_container_move_ready(void *data) {
  const struct leme_render_container_animation *state = data;
  return leme_render_container_ready(state->container);
}

static void
leme_render_container_move_apply(void *data,
                                 const struct leme_animation_frame *frame) {
  struct leme_render_container_animation *state = data;
  state->current = frame->box;
  leme_render_container_set_box(state->container, frame->box);
}

static void leme_render_container_move_done(void *data) {
  struct leme_render_container_animation *state = data;
  state->container->animation_state = NULL;
  leme_render_container_set_box(
      state->container, state->flying ? state->container->box : state->target);
  if (state->flying) {
    leme_render_container_set_visible(
        state->container, state->visible && !state->container->minimized);
  }
  wl_array_release(&state->colors);
  free(state);
}

void leme_render_container_finish_move(struct leme_container *container) {
  if (container != NULL && container->animation_state != NULL) {
    leme_animation_manager_finish_data(
        &leme_render_container_server(container)->animations,
        container->animation_state);
  }
}

bool leme_render_container_animation_box(const struct leme_container *container,
                                         struct leme_box *out) {
  if (container == NULL || container->animation_state == NULL || out == NULL) {
    return false;
  }
  *out = container->animation_state->current;
  return true;
}

void leme_render_container_move(struct leme_container *container,
                                struct leme_box from, struct leme_box to) {
  struct leme_server *server = leme_render_container_server(container);
  if (container->animation_state != NULL) {
    from = container->animation_state->current;
  }
  leme_render_container_finish_move(container);
  leme_animation_gate_destroy(container->animation_gate);
  container->animation_gate = NULL;
  if (server->config == NULL || container->minimized ||
      !server->config->animation[LEME_ANIMATION_MOVE].configured) {
    leme_render_container_set_box(container, to);
    return;
  }
  container->animation_gate = leme_animation_gate_create(200);
  if (container->animation_gate == NULL || leme_box_equal(from, to)) {
    leme_render_container_set_box(container, to);
    return;
  }
  struct leme_render_container_animation *state = calloc(1, sizeof(*state));
  if (state == NULL) {
    leme_animation_gate_destroy(container->animation_gate);
    container->animation_gate = NULL;
    leme_render_container_set_box(container, to);
    return;
  }
  *state = (struct leme_render_container_animation){
      .container = container, .current = from, .target = to};
  container->animation_state = state;
  const struct leme_animation_settings *settings =
      &server->config->animation[LEME_ANIMATION_MOVE];
  const struct leme_animation_spec spec =
      leme_animation_move_spec(settings, from, to, 1.0, 1.0);
  const struct leme_animation_subject subject = {
      .gate = container->animation_gate,
      .data = state,
      .owner = container->tag->owner->output,
      .apply = leme_render_container_move_apply,
      .done = leme_render_container_move_done,
      .ready = leme_render_container_move_ready};
  leme_render_container_set_box(container, from);
  leme_animation_run(&server->animations,
                     wlr_scene_tree_create(server->scene_floating), &spec,
                     &subject);
  if (container->animation_state == NULL) {
    leme_animation_gate_destroy(container->animation_gate);
    container->animation_gate = NULL;
  }
}

static void
leme_render_container_fly_apply(void *data,
                                const struct leme_animation_frame *frame) {
  struct leme_render_container_animation *state = data;
  const struct leme_config *config =
      leme_render_container_server(state->container)->config;
  state->current = frame->box;
  wlr_scene_node_set_position(&state->snapshot->node, frame->box.x,
                              frame->box.y);
  const int side = frame->box.width < frame->box.height ? frame->box.width
                                                        : frame->box.height;
  const int limit = side > 0 ? (side - 1) / 2 : 0;
  const int border =
      config->border_width < limit ? config->border_width : limit;
  struct leme_titlebar_style style = {0};
  leme_desk_style_titlebar(config, &style);
  leme_render_view_layout_frame(&state->nodes, frame->box, border, 0, &style);
  leme_render_view_fade_nodes(&state->colors, (float)frame->opacity);
}

void leme_render_container_fly(struct leme_container *container,
                               struct leme_box from, struct leme_box to,
                               bool appearing) {
  if (container == NULL || container->render_tree == NULL) {
    return;
  }
  struct leme_server *server = leme_render_container_server(container);
  if (server->config == NULL ||
      !server->config->animation[LEME_ANIMATION_MOVE].configured ||
      container->tag->owner->focused_is_candidate ||
      container->tag->id != container->tag->owner->focused_id) {
    return;
  }
  leme_render_container_finish_move(container);
  leme_animation_gate_destroy(container->animation_gate);
  container->animation_gate = leme_animation_gate_create(200);
  if (container->animation_gate == NULL) {
    return;
  }
  struct leme_render_container_animation *state = calloc(1, sizeof(*state));
  if (state == NULL) {
    return;
  }
  *state = (struct leme_render_container_animation){
      .container = container,
      .current = from,
      .target = to,
      .flying = true,
      .visible = container->render_tree->node.enabled};
  wl_array_init(&state->colors);
  state->snapshot =
      leme_animation_snapshot(container->render_tree, server->scene_floating);
  if (state->snapshot == NULL || !leme_render_view_collect_content(
                                     &state->colors, state->snapshot, 0, 0)) {
    leme_animation_snapshot_destroy(state->snapshot);
    wl_array_release(&state->colors);
    free(state);
    return;
  }
  struct wlr_scene_node *node = NULL;
  size_t side = 0;
  wl_list_for_each(node, &state->snapshot->children, link) {
    if (node->type == WLR_SCENE_NODE_RECT && side < 4) {
      struct wlr_scene_rect *rect = wl_container_of(node, rect, node);
      state->nodes.border[side++] = rect;
    } else if (node->type == WLR_SCENE_NODE_TREE) {
      state->nodes.decoration =
          wl_container_of(node, state->nodes.decoration, node);
    }
  }
  const struct leme_config *config =
      leme_render_container_server(container)->config;
  state->nodes.titlebar_height = leme_desk_style_titlebar_height(config);
  state->nodes.button_width = leme_desk_style_button_width(config);
  state->nodes.titlebar_width =
      leme_container_content_box(container, container->box).width;
  container->animation_state = state;
  wlr_scene_node_set_enabled(&container->render_tree->node, false);
  const struct leme_animation_settings *settings =
      &server->config->animation[LEME_ANIMATION_MOVE];
  const struct leme_animation_spec spec = leme_animation_move_spec(
      settings, from, to, appearing ? 0.0 : 1.0, appearing ? 1.0 : 0.0);
  const struct leme_animation_subject subject = {
      .gate = container->animation_gate,
      .data = state,
      .owner = container->tag->owner->output,
      .apply = leme_render_container_fly_apply,
      .done = leme_render_container_move_done,
      .ready = appearing ? leme_render_container_move_ready : NULL};
  const struct leme_animation_frame initial =
      leme_animation_frame_at(&spec, 0.0, 0.0);
  leme_render_container_fly_apply(state, &initial);
  leme_animation_run(&server->animations, state->snapshot, &spec, &subject);
  if (container->animation_state == NULL) {
    return;
  }
  for (size_t i = 0; i < container->count; i++) {
    struct leme_view *view = container->views[i];
    leme_render_view_fly(view, appearing ? from : view->box,
                         appearing ? view->box : to, appearing);
  }
}

static bool
leme_render_container_owns_node(const struct leme_container *container,
                                const struct wlr_scene_node *node) {
  if (node == &container->render_tree->node ||
      (container->animation_state != NULL &&
       container->animation_state->snapshot != NULL &&
       node == &container->animation_state->snapshot->node)) {
    return true;
  }
  for (size_t i = 0; i < container->count; i++) {
    if (container->views[i]->render_tree != NULL &&
        leme_render_view_owns_node(container->views[i], node)) {
      return true;
    }
  }
  const struct leme_render_owner *owner = node->data;
  return owner != NULL && owner->kind == LEME_RENDER_OWNER_VIEW &&
         leme_container_for_view(owner->value.view) == container;
}

static size_t
leme_render_container_depth(const struct leme_view *view,
                            const struct leme_container *container,
                            size_t limit) {
  size_t depth = 0;
  while (view != NULL && view->container != container && depth < limit) {
    if (leme_ownership_tag(view) != container->tag) {
      return SIZE_MAX;
    }
    view = leme_view_parent(view);
    depth++;
  }
  return view != NULL && view->container == container ? depth : SIZE_MAX;
}

void leme_render_container_restack(struct leme_container *container,
                                   bool raise) {
  if (container == NULL || container->render_tree == NULL) {
    return;
  }
  struct leme_server *server = leme_render_container_server(container);
  struct wlr_scene_tree *layer = container->render_tree->node.parent;
  struct wlr_scene_node *above = NULL;
  struct wlr_scene_node *node = NULL;
  if (!raise) {
    wl_list_for_each_reverse(node, &layer->children, link) {
      if (leme_render_container_owns_node(container, node)) {
        break;
      }
      above = node;
    }
  }
  struct wlr_scene_node *anchor = &container->render_tree->node;
  if (above != NULL) {
    wlr_scene_node_place_below(anchor, above);
  } else {
    wlr_scene_node_raise_to_top(anchor);
  }
  if (container->animation_state != NULL &&
      container->animation_state->snapshot != NULL) {
    wlr_scene_node_place_above(&container->animation_state->snapshot->node,
                               anchor);
    anchor = &container->animation_state->snapshot->node;
  }
  const size_t limit = (size_t)wl_list_length(&server->views);
  struct leme_view *view = NULL;
  for (size_t i = 0; i < container->count; i++) {
    view = container->views[i];
    if (view->render_tree != NULL && view->render_tree->node.parent == layer) {
      anchor = leme_render_view_stack_above(view, anchor);
    }
  }
  for (size_t depth = 1; depth < limit; depth++) {
    bool found = false;
    wl_list_for_each(view, &server->views, link) {
      if (view->mapped && view->render_tree != NULL &&
          leme_render_container_depth(view, container, limit) == depth) {
        found = true;
        if (view->render_tree->node.parent == layer) {
          anchor = leme_render_view_stack_above(view, anchor);
        }
      }
    }
    if (!found) {
      break;
    }
  }
  if (raise) {
    wl_list_for_each(view, &server->views, link) {
      if (view->mapped && view->fullscreen && view->render_tree != NULL &&
          view->render_tree->node.parent != layer &&
          leme_container_for_view(view) == container) {
        wlr_scene_node_raise_to_top(&view->render_tree->node);
      }
    }
  }
}
