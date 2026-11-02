#include "desktop/decoration.h"
#include "desktop/frame.h"

#include "protocols/decoration.h"
#include "protocols/desktop.h"
#include "core/server.h"
#include "config/config.h"
#include "config/style.h"
#include "desktop/icons.h"
#include "desktop/pixels.h"
#include "desktop/buttons.h"
#include "input/input.h"
#include "output/output.h"
#include "shell/policy.h"
#include "render/render.h"
#include "shell/view.h"
#include "shell/container.h"
#include "workspace/tag.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <pango/pangocairo.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/edges.h>

struct leme_desk_decoration {
  struct leme_desk_frame chrome;
  int height;
  int normal_height;
  int border;
  struct leme_box frame;
  bool placed;
  bool fullscreen;
  enum leme_view_placement placement;
};

bool leme_desk_decoration_eligible(const struct leme_view *view) {
  return leme_desk_decoration_eligible_for_state(view, view != NULL &&
                                                           view->fullscreen);
}

bool leme_desk_decoration_eligible_for_state(const struct leme_view *view,
                                             bool fullscreen) {
  if (view == NULL || view->server == NULL || !view->mapped ||
      view->unmanaged || fullscreen || view->detached ||
      leme_view_geometry_owner(view) == LEME_VIEW_GEOMETRY_CONTAINER ||
      leme_view_has_shell_decoration(view, fullscreen)) {
    return false;
  }
  const struct leme_tag *tag = leme_ownership_tag(view);
  return tag != NULL && tag->layout.kind == LEME_LAYOUT_DESKTOP &&
         leme_desktop_view_server_decorated(view);
}

bool leme_desk_decoration_geometry(struct leme_box frame, int border_width,
                                   int titlebar_height, int button_width,
                                   struct leme_desk_decoration_boxes *boxes) {
  if (boxes == NULL) {
    return false;
  }
  *boxes = (struct leme_desk_decoration_boxes){0};
  if (titlebar_height < 1 || button_width < 1 || button_width > INT_MAX / 3) {
    return false;
  }
  const int controls_width = 3 * button_width;
  if (frame.width < controls_width || frame.height <= titlebar_height ||
      border_width < 0 || border_width > (frame.width - controls_width) / 2 ||
      border_width > (frame.height - titlebar_height - 1) / 2 ||
      frame.x > INT_MAX - frame.width || frame.y > INT_MAX - frame.height) {
    return false;
  }
  const struct leme_box titlebar = {
      .x = frame.x + border_width,
      .y = frame.y + border_width,
      .width = frame.width - border_width * 2,
      .height = titlebar_height,
  };
  *boxes = (struct leme_desk_decoration_boxes){
      .content =
          {
              .x = titlebar.x,
              .y = titlebar.y + titlebar.height,
              .width = titlebar.width,
              .height = frame.height - border_width * 2 - titlebar.height,
          },
      .titlebar = titlebar,
  };
  return true;
}

uint32_t leme_desk_decoration_resize_edges(struct leme_box frame,
                                           int border_width, double lx,
                                           double ly) {
  if (frame.width <= 0 || frame.height <= 0 || border_width < 0 ||
      border_width > (frame.width - 1) / 2 ||
      border_width > (frame.height - 1) / 2 ||
      frame.x > INT_MAX - frame.width || frame.y > INT_MAX - frame.height ||
      !isfinite(lx) || !isfinite(ly)) {
    return WLR_EDGE_NONE;
  }
  const double x = lx - frame.x;
  const double y = ly - frame.y;
  if (x < -LEME_DESK_RESIZE_MARGIN || y < -LEME_DESK_RESIZE_MARGIN ||
      x >= (double)frame.width + LEME_DESK_RESIZE_MARGIN ||
      y >= (double)frame.height + LEME_DESK_RESIZE_MARGIN ||
      (x >= border_width && y >= border_width &&
       x < frame.width - border_width && y < frame.height - border_width)) {
    return WLR_EDGE_NONE;
  }
  const double corner_x = fmin(LEME_DESK_RESIZE_CORNER, frame.width / 2.0);
  const double corner_y = fmin(LEME_DESK_RESIZE_CORNER, frame.height / 2.0);
  uint32_t edges = WLR_EDGE_NONE;
  if (x < fmax(border_width, corner_x)) {
    edges |= WLR_EDGE_LEFT;
  } else if (x >= frame.width - fmax(border_width, corner_x)) {
    edges |= WLR_EDGE_RIGHT;
  }
  if (y < fmax(border_width, corner_y)) {
    edges |= WLR_EDGE_TOP;
  } else if (y >= frame.height - fmax(border_width, corner_y)) {
    edges |= WLR_EDGE_BOTTOM;
  }
  return edges;
}

static int leme_desk_decoration_border(const struct leme_view *view) {
  return view->server->config == NULL ? 0 : view->server->config->border_width;
}

void leme_desk_decoration_init(struct leme_server *server) {
  server->desk_decorations_ready = true;
  server->desk_chrome_icons = leme_desk_icons_create();
}

void leme_desk_decoration_view_finish(struct leme_view *view) {
  struct leme_desk_decoration *state =
      view == NULL ? NULL : view->desk_decoration;
  if (state == NULL) {
    return;
  }
  leme_desk_buttons_invalidate_view(view);
  view->desk_decoration = NULL;
  leme_desk_frame_finish(&state->chrome);
  free(state);
}

void leme_desk_decoration_finish(struct leme_server *server) {
  if (server == NULL) {
    return;
  }
  server->desk_decorations_ready = false;
  if (server->views.next != NULL) {
    struct leme_view *view = NULL;
    wl_list_for_each(view, &server->views, link) {
      leme_desk_decoration_view_finish(view);
    }
  }
  leme_desk_icons_destroy(server->desk_chrome_icons);
  server->desk_chrome_icons = NULL;
  if (server->desk == NULL) {
    pango_cairo_font_map_set_default(NULL);
  }
}

static double leme_desk_decoration_scale(const struct leme_view *view) {
  return leme_output_scale(leme_view_output(view));
}

static bool leme_desk_decoration_create(struct leme_view *view) {
  if (view->server->config == NULL) {
    return false;
  }
  struct leme_desk_decoration *state = calloc(1, sizeof(*state));
  if (state == NULL) {
    return false;
  }
  struct leme_titlebar_style style = {0};
  leme_desk_style_titlebar(view->server->config, &style);
  if (!leme_desk_frame_init(&state->chrome, view->server, view->render_tree,
                            leme_desk_decoration_scale(view), &style)) {
    free(state);
    return false;
  }
  view->desk_decoration = state;
  return true;
}

int leme_desk_decoration_height(const struct leme_view *view,
                                struct leme_box frame) {
  if (view == NULL || view->server == NULL ||
      !view->server->desk_decorations_ready || view->desk_decoration == NULL ||
      view->fullscreen || view->desk_decoration->height == 0) {
    return 0;
  }
  const struct leme_config *config = view->server->config;
  const int height = leme_desk_style_titlebar_height(config);
  struct leme_desk_decoration_boxes boxes = {0};
  return leme_desk_decoration_geometry(
             frame, leme_desk_decoration_border(view), height,
             leme_desk_style_button_width(config), &boxes)
             ? height
             : 0;
}

struct wlr_scene_tree *leme_desk_decoration_tree(const struct leme_view *view) {
  return view == NULL || view->desk_decoration == NULL
             ? NULL
             : view->desk_decoration->chrome.tree;
}

static bool leme_desk_decoration_text(struct leme_view *view,
                                      int titlebar_width) {
  return !leme_input_pointer_grabbing(view->server, view) &&
         leme_desk_frame_set_title(&view->desk_decoration->chrome,
                                   leme_view_title(view), titlebar_width,
                                   leme_desk_decoration_scale(view));
}

static bool leme_desk_decoration_reframe(struct leme_box *box,
                                         int64_t border_delta,
                                         int64_t titlebar_delta) {
  const int64_t x = (int64_t)box->x - border_delta;
  const int64_t y = (int64_t)box->y - border_delta - titlebar_delta;
  const int64_t width = (int64_t)box->width + border_delta * 2;
  const int64_t height =
      (int64_t)box->height + border_delta * 2 + titlebar_delta;
  if (x < INT_MIN || x > INT_MAX || y < INT_MIN || y > INT_MAX || width <= 0 ||
      width > INT_MAX || height <= 0 || height > INT_MAX) {
    return false;
  }
  *box = (struct leme_box){(int)x, (int)y, (int)width, (int)height};
  return true;
}

bool leme_desk_decoration_sync(struct leme_view *view) {
  if (view == NULL || view->server == NULL ||
      !view->server->desk_decorations_ready || view->render_tree == NULL) {
    return false;
  }
  const bool eligible = leme_desk_decoration_eligible(view);
  const bool fresh = view->desk_decoration == NULL;
  if (fresh && (!eligible || !leme_desk_decoration_create(view))) {
    return false;
  }
  struct leme_desk_decoration *state = view->desk_decoration;
  const int border = leme_desk_decoration_border(view);
  const int titlebar = leme_desk_style_titlebar_height(view->server->config);
  const int button = leme_desk_style_button_width(view->server->config);
  const int height = eligible ? titlebar : 0;
  const int normal_height =
      leme_desk_decoration_eligible_for_state(view, false) ? titlebar : 0;
  const int previous_border = fresh ? border : state->border;
  bool changed = fresh || state->height != height || state->border != border ||
                 !leme_box_equal(state->frame, view->box) ||
                 state->fullscreen != view->fullscreen ||
                 state->placement != view->placement.kind;
  const double scale = leme_desk_decoration_scale(view);
  if (changed || state->chrome.controls.scale != scale) {
    leme_desk_buttons_invalidate_view(view);
  }
  if (state->chrome.controls.scale != scale && view->server->config != NULL) {
    struct leme_titlebar_style style = {0};
    leme_desk_style_titlebar(view->server->config, &style);
    changed =
        leme_desk_frame_set_style(&state->chrome, scale, &style) || changed;
  }
  const bool saved =
      view->fullscreen && !leme_view_is_placed(view) &&
      leme_view_geometry_owner(view) != LEME_VIEW_GEOMETRY_CONTAINER &&
      state->placement == LEME_VIEW_PLACEMENT_NORMAL &&
      (state->normal_height != normal_height || previous_border != border);
  const bool live =
      (state->height != height || previous_border != border) &&
      !view->fullscreen && !leme_view_is_placed(view) &&
      leme_view_geometry_owner(view) == LEME_VIEW_GEOMETRY_FLOATING &&
      (fresh ||
       (!state->fullscreen && state->placement == LEME_VIEW_PLACEMENT_NORMAL &&
        leme_box_equal(state->frame, view->box)));
  struct leme_box *target = saved ? &view->saved_box : &view->box;
  if ((saved || live) && target->width > 0 && target->height > 0) {
    const int64_t border_delta = (int64_t)border - previous_border;
    struct leme_desk_decoration_boxes previous = {0};
    const int previous_titlebar = saved ? state->normal_height : state->height;
    const int previous_height =
        previous_titlebar > 0 && leme_desk_decoration_geometry(
                                     *target, previous_border,
                                     previous_titlebar, button, &previous)
            ? previous_titlebar
            : 0;
    const int wanted = saved ? normal_height : height;
    struct leme_box frame = *target;
    bool valid = leme_desk_decoration_reframe(
        &frame, border_delta, (int64_t)wanted - previous_height);
    struct leme_desk_decoration_boxes next = {0};
    if (valid && wanted > 0 &&
        !leme_desk_decoration_geometry(frame, border, wanted, button, &next)) {
      frame = *target;
      valid = leme_desk_decoration_reframe(&frame, border_delta,
                                           -(int64_t)previous_height);
    }
    if (valid) {
      const struct leme_output *output = leme_view_output(view);
      *target = output == NULL
                    ? frame
                    : leme_view_policy_clamp_box(frame, output->usable_box);
    }
  }
  state->height = height;
  state->normal_height = normal_height;
  state->border = border;
  state->frame = view->box;
  state->fullscreen = view->fullscreen;
  state->placement = view->placement.kind;
  state->placed = view->box.width > 0 && view->box.height > 0;
  struct leme_desk_decoration_boxes boxes = {0};
  const bool fits = leme_desk_decoration_geometry(view->box, border, titlebar,
                                                  button, &boxes);
  if (height > 0 && state->placed && fits) {
    changed = leme_desk_decoration_text(view, boxes.titlebar.width) || changed;
  }
  leme_desk_frame_layout(state->chrome.tree, view->box, border, height,
                         boxes.titlebar.width, &state->chrome.style,
                         leme_render_view_corner_radius(view, view->box));
  leme_desk_frame_set_maximized(&state->chrome, leme_view_is_maximized(view));
  leme_desk_decoration_set_activated(view, view == view->server->focused_view);
  leme_render_view_sync_presentation(view);
  return changed;
}

bool leme_desk_decoration_hit(const struct leme_view *view,
                              const struct wlr_scene_node *node,
                              struct leme_render_hit *hit) {
  if (!leme_desk_decoration_eligible(view) || leme_view_is_minimized(view) ||
      leme_desk_decoration_height(view, view->box) == 0) {
    return false;
  }
  hit->decoration_part =
      leme_desk_frame_hit(&view->desk_decoration->chrome, node);
  return hit->decoration_part != LEME_RENDER_DECORATION_NONE;
}

void leme_desk_decoration_set_activated(struct leme_view *view,
                                        bool activated) {
  if (view == NULL || view->desk_decoration == NULL) {
    return;
  }
  struct leme_desk_frame *frame = &view->desk_decoration->chrome;
  leme_desk_frame_set_activated(frame, activated);
  leme_desk_decoration_feedback(view, frame->hover_part, frame->pressed);
}

void leme_desk_decoration_feedback(struct leme_view *view,
                                   enum leme_render_decoration_part part,
                                   bool pressed) {
  if (view == NULL || view->desk_decoration == NULL) {
    return;
  }
  struct leme_desk_frame *frame = &view->desk_decoration->chrome;
  frame->available[0] =
      leme_desk_buttons_available(view, LEME_RENDER_DECORATION_MINIMIZE);
  frame->available[1] =
      leme_desk_buttons_available(view, LEME_RENDER_DECORATION_MAXIMIZE);
  frame->available[2] =
      leme_desk_buttons_available(view, LEME_RENDER_DECORATION_CLOSE);
  leme_desk_frame_feedback(frame, part, pressed);
}

void leme_desk_decoration_refresh(struct leme_view *view) {
  if (view == NULL || view->server == NULL ||
      !view->server->desk_decorations_ready || !view->mapped) {
    return;
  }
  if (view->container != NULL) {
    leme_render_container_set_box(view->container, view->container->box);
  }
  const struct leme_box before = leme_render_view_content_box(view, view->box);
  if (!leme_desk_decoration_sync(view)) {
    return;
  }
  leme_render_view_set_box(view, view->box);
  const struct leme_box after = leme_render_view_content_box(view, view->box);
  if (view->kind == LEME_VIEW_XDG &&
      (before.width != after.width || before.height != after.height)) {
    leme_view_configure(view, after);
  }
}

void leme_desk_frames_restyle(struct leme_server *server) {
  if (server == NULL || server->config == NULL || server->views.next == NULL) {
    return;
  }
  leme_desk_icons_clear(server->desk_chrome_icons);
  struct leme_titlebar_style titlebar = {0};
  leme_desk_style_titlebar(server->config, &titlebar);
  const struct leme_titlebar_style *style = &titlebar;
  struct leme_view *view = NULL;
  wl_list_for_each(view, &server->views, link) {
    if (view->desk_decoration != NULL) {
      (void)leme_desk_frame_set_style(&view->desk_decoration->chrome,
                                      leme_desk_decoration_scale(view), style);
      leme_desk_decoration_refresh(view);
    }
    struct leme_container *container = view->container;
    if (container != NULL && container->focused == view) {
      (void)leme_desk_frame_set_style(&container->frame,
                                      container->frame.controls.scale, style);
      leme_render_container_set_box(container, container->box);
    }
  }
}
