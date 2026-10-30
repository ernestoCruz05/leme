#include "desktop/frame.h"

#include "config/style.h"
#include "core/server.h"
#include "desktop/decoration.h"
#include "desktop/icons.h"
#include "desktop/pixels.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <pango/pangocairo.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>

static const float leme_desk_frame_clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};

static enum leme_render_decoration_part
leme_desk_frame_part(enum leme_titlebar_button kind) {
  switch (kind) {
  case LEME_TITLEBAR_BUTTON_MINIMIZE:
    return LEME_RENDER_DECORATION_MINIMIZE;
  case LEME_TITLEBAR_BUTTON_MAXIMIZE:
    return LEME_RENDER_DECORATION_MAXIMIZE;
  case LEME_TITLEBAR_BUTTON_CLOSE:
  default:
    return LEME_RENDER_DECORATION_CLOSE;
  }
}

static enum leme_desk_control_icon
leme_desk_frame_icon(enum leme_titlebar_button kind) {
  switch (kind) {
  case LEME_TITLEBAR_BUTTON_MINIMIZE:
    return LEME_DESK_CONTROL_MINIMIZE;
  case LEME_TITLEBAR_BUTTON_MAXIMIZE:
    return LEME_DESK_CONTROL_MAXIMIZE;
  case LEME_TITLEBAR_BUTTON_CLOSE:
  default:
    return LEME_DESK_CONTROL_CLOSE;
  }
}

static enum leme_desk_element
leme_desk_frame_element(enum leme_titlebar_button kind) {
  switch (kind) {
  case LEME_TITLEBAR_BUTTON_MINIMIZE:
    return LEME_DESK_ELEMENT_MINIMIZE;
  case LEME_TITLEBAR_BUTTON_MAXIMIZE:
    return LEME_DESK_ELEMENT_MAXIMIZE;
  case LEME_TITLEBAR_BUTTON_CLOSE:
  default:
    return LEME_DESK_ELEMENT_CLOSE;
  }
}

static uint32_t
leme_desk_frame_button_states(const struct leme_desk_frame *frame,
                              const struct leme_desk_button *button) {
  uint32_t states = 0;
  if (!frame->activated) {
    states |= LEME_DESK_STATE_BIT(LEME_DESK_STATE_INACTIVE);
  }
  if (frame->maximized && button->kind == LEME_TITLEBAR_BUTTON_MAXIMIZE) {
    states |= LEME_DESK_STATE_BIT(LEME_DESK_STATE_MAXIMIZED);
  }
  if (frame->hover_part == leme_desk_frame_part(button->kind)) {
    states |= LEME_DESK_STATE_BIT(LEME_DESK_STATE_HOVER);
    if (frame->pressed) {
      states |= LEME_DESK_STATE_BIT(LEME_DESK_STATE_PRESSED);
    }
  }
  if (!frame->available[button->kind]) {
    states |= LEME_DESK_STATE_BIT(LEME_DESK_STATE_DISABLED);
  }
  return states;
}

static struct wlr_buffer *
leme_desk_frame_image(const struct leme_desk_frame *frame,
                      const struct leme_desk_look *look, int pixels,
                      bool *owned) {
  struct leme_desk_icons *icons =
      frame->server == NULL ? NULL : frame->server->desk_chrome_icons;
  *owned = icons == NULL;
  return icons == NULL ? leme_desk_pixels_load(look->icon, pixels)
                       : leme_desk_icons_get(icons, look->icon, pixels);
}

static void leme_desk_frame_opacity(struct wlr_scene_tree *tree,
                                    float opacity) {
  struct wlr_scene_node *node = NULL;
  wl_list_for_each(node, &tree->children, link) {
    if (node->type == WLR_SCENE_NODE_BUFFER) {
      struct wlr_scene_buffer *buffer = wl_container_of(node, buffer, node);
      wlr_scene_buffer_set_opacity(buffer, opacity);
    }
  }
}

static bool leme_desk_frame_draw_button(struct leme_desk_frame *frame,
                                        struct leme_desk_button *button) {
  const uint32_t states = leme_desk_frame_button_states(frame, button);
  if (button->face != NULL && button->states == states) {
    return true;
  }
  struct leme_desk_look look = {0};
  leme_desk_style_resolve(frame->server == NULL ? NULL : frame->server->config,
                          leme_desk_frame_element(button->kind), states, &look);
  struct wlr_scene_tree *face = wlr_scene_tree_create(button->tree);
  if (face == NULL) {
    return false;
  }
  const int width = frame->style.button_width;
  const int height = frame->style.height;
  const double scale = frame->controls.scale;
  bool ok = look.background[3] <= 0.0f ||
            leme_desk_pixels_rect(face, (struct leme_box){0, 0, width, height},
                                  look.background, look.radius, false);
  bool owned = true;
  struct wlr_buffer *buffer = NULL;
  if (ok && look.icon != NULL) {
    buffer = leme_desk_frame_image(
        frame, &look, (int)ceil((double)look.size * scale), &owned);
  }
  if (ok && buffer == NULL) {
    owned = true;
    const bool restore =
        (states & LEME_DESK_STATE_BIT(LEME_DESK_STATE_MAXIMIZED)) != 0;
    buffer =
        leme_desk_pixels_control(restore ? LEME_DESK_CONTROL_RESTORE
                                         : leme_desk_frame_icon(button->kind),
                                 look.color, look.size, scale);
  }
  ok = ok && buffer != NULL &&
       leme_desk_pixels_place_buffer(face, buffer,
                                     (struct leme_box){(width - look.size) / 2,
                                                       (height - look.size) / 2,
                                                       look.size, look.size});
  if (buffer != NULL && owned) {
    wlr_buffer_drop(buffer);
  }
  if (!ok) {
    wlr_scene_node_destroy(&face->node);
    return false;
  }
  leme_desk_frame_opacity(face, (float)look.opacity);
  if (button->face != NULL) {
    wlr_scene_node_destroy(&button->face->node);
  }
  button->face = face;
  button->states = states;
  return true;
}

static bool leme_desk_frame_button(struct leme_desk_button *button,
                                   struct wlr_scene_tree *parent,
                                   enum leme_titlebar_button kind,
                                   const struct leme_titlebar_style *style,
                                   int x) {
  *button = (struct leme_desk_button){.kind = kind};
  button->tree = wlr_scene_tree_create(parent);
  if (button->tree == NULL) {
    return false;
  }
  wlr_scene_node_set_position(&button->tree->node, x, 0);
  button->hit = wlr_scene_rect_create(button->tree, style->button_width,
                                      style->height, leme_desk_frame_clear);
  return button->hit != NULL;
}

static void leme_desk_frame_show(struct leme_desk_frame *frame) {
  const size_t current = frame->activated ? 1 : 0;
  for (size_t state = 0; state < LEME_ARRAY_LENGTH(frame->variants); state++) {
    if (frame->variants[state] != NULL) {
      wlr_scene_node_set_enabled(&frame->variants[state]->node,
                                 state == current);
    }
  }
  for (size_t i = 0; i < frame->controls.count; i++) {
    (void)leme_desk_frame_draw_button(frame, &frame->controls.buttons[i]);
  }
}

bool leme_desk_frame_set_style(struct leme_desk_frame *frame, double scale,
                               const struct leme_titlebar_style *style) {
  if (frame == NULL || frame->tree == NULL || frame->corners == NULL ||
      style == NULL || style->height < 1 || style->button_width < 1 ||
      style->button_count > LEME_ARRAY_LENGTH(style->buttons)) {
    return false;
  }
  struct leme_desk_controls controls = {.scale = scale};
  controls.tree = wlr_scene_tree_create(frame->tree);
  if (controls.tree == NULL) {
    return false;
  }
  for (size_t i = 0; i < style->button_count; i++) {
    if (!leme_desk_frame_button(&controls.buttons[i], controls.tree,
                                style->buttons[i], style,
                                (int)i * style->button_width)) {
      wlr_scene_node_destroy(&controls.tree->node);
      return false;
    }
    controls.count++;
  }
  wlr_scene_node_place_above(&controls.tree->node, &frame->corners->node);
  if (frame->controls.tree != NULL) {
    wlr_scene_node_destroy(&frame->controls.tree->node);
  }
  frame->controls = controls;
  frame->style = *style;
  free(frame->title);
  frame->title = NULL;
  frame->text_width = -1;
  leme_desk_frame_set_activated(frame, frame->activated);
  leme_desk_frame_feedback(frame, frame->hover_part, frame->pressed);
  return true;
}

bool leme_desk_frame_init(struct leme_desk_frame *frame,
                          struct leme_server *server,
                          struct wlr_scene_tree *parent, double scale,
                          const struct leme_titlebar_style *style) {
  *frame = (struct leme_desk_frame){.server = server, .text_width = -1};
  frame->tree = wlr_scene_tree_create(parent);
  if (frame->tree == NULL) {
    return false;
  }
  wlr_scene_node_set_enabled(&frame->tree->node, false);
  frame->background =
      wlr_scene_rect_create(frame->tree, 1, 1, leme_desk_frame_clear);
  frame->corners =
      frame->background == NULL
          ? NULL
          : wlr_scene_rect_create(frame->tree, 1, 1, leme_desk_frame_clear);
  if (frame->corners == NULL ||
      !leme_desk_frame_set_style(frame, scale, style)) {
    leme_desk_frame_finish(frame);
    return false;
  }
  return true;
}

void leme_desk_frame_finish(struct leme_desk_frame *frame) {
  if (frame->tree != NULL) {
    wlr_scene_node_destroy(&frame->tree->node);
  }
  free(frame->title);
  *frame = (struct leme_desk_frame){0};
}

static void
leme_desk_frame_layout_text(struct wlr_scene_tree *text, int titlebar_width,
                            int left_width, int right_width, int natural_width,
                            const struct leme_titlebar_style *style) {
  const int controls_width = left_width + right_width;
  int shown = -1;
  struct wlr_scene_node *variant_node = NULL;
  wl_list_for_each(variant_node, &text->children, link) {
    if (variant_node->type != WLR_SCENE_NODE_TREE) {
      continue;
    }
    struct wlr_scene_tree *variant =
        wl_container_of(variant_node, variant, node);
    struct wlr_scene_node *child = NULL;
    wl_list_for_each(child, &variant->children, link) {
      if (child->type != WLR_SCENE_NODE_BUFFER) {
        continue;
      }
      struct wlr_scene_buffer *buffer = wl_container_of(child, buffer, node);
      int width = natural_width > controls_width + 16
                      ? natural_width - controls_width - 16
                      : 0;
      width = width > 1024 ? 1024 : width;
      const int available = titlebar_width - controls_width - 16;
      width = width > available ? available : width;
      const bool visible =
          width > 0 && buffer->dst_width > 0 && buffer->src_box.width > 0.0;
      wlr_scene_node_set_enabled(child, visible);
      if (!visible) {
        continue;
      }
      struct wlr_fbox source = buffer->src_box;
      source.width *= (double)width / (double)buffer->dst_width;
      if (buffer->buffer != NULL && source.width > buffer->buffer->width) {
        width =
            (int)lround((double)width * buffer->buffer->width / source.width);
        source.width = buffer->buffer->width;
      }
      wlr_scene_buffer_set_source_box(buffer, &source);
      wlr_scene_buffer_set_dest_size(buffer, width, buffer->dst_height);
      shown = width;
    }
  }
  if (shown < 0) {
    return;
  }
  int x = left_width + 8;
  if (style != NULL && style->align == LEME_TITLEBAR_ALIGN_CENTER) {
    x = (titlebar_width - shown) / 2;
    const int limit = titlebar_width - right_width - 8 - shown;
    x = x > limit ? limit : x;
    x = x < left_width + 8 ? left_width + 8 : x;
  }
  wlr_scene_node_set_position(&text->node, x, text->node.y);
}

static void
leme_desk_frame_place_buttons(struct wlr_scene_tree *controls, int width,
                              const struct leme_titlebar_style *style,
                              int *left_width, int *right_width) {
  size_t count = 0;
  struct wlr_scene_node *button = NULL;
  wl_list_for_each(button, &controls->children, link) {
    count += button->type == WLR_SCENE_NODE_TREE ? 1 : 0;
  }
  const size_t left = style->left < count ? style->left : count;
  size_t index = 0;
  wl_list_for_each(button, &controls->children, link) {
    if (button->type != WLR_SCENE_NODE_TREE) {
      continue;
    }
    const int x = index < left
                      ? (int)index * style->button_width
                      : width - (int)(count - index) * style->button_width;
    wlr_scene_node_set_position(button, x, 0);
    index++;
  }
  wlr_scene_node_set_position(&controls->node, 0, 0);
  *left_width = (int)left * style->button_width;
  *right_width = (int)(count - left) * style->button_width;
}

void leme_desk_frame_layout(struct wlr_scene_tree *tree, struct leme_box frame,
                            int border_width, int height, int natural_width,
                            const struct leme_titlebar_style *style,
                            int radius) {
  if (tree == NULL) {
    return;
  }
  struct leme_desk_decoration_boxes boxes = {0};
  const bool enabled =
      height > 0 && style != NULL &&
      leme_desk_decoration_geometry(frame, border_width, height,
                                    style->button_width, &boxes);
  wlr_scene_node_set_enabled(&tree->node, enabled);
  if (!enabled) {
    return;
  }
  wlr_scene_node_set_position(&tree->node, border_width, border_width);
  const int width = boxes.titlebar.width;
#ifdef LEME_HAVE_EFFECTS
  int inner = radius > border_width ? radius - border_width : 0;
  const int limit = (width < height ? width : height) / 2;
  inner = inner > limit ? limit : inner;
#else
  (void)radius;
  const int inner = 0;
#endif
  size_t rects = 0;
  size_t trees = 0;
  int left_width = 0;
  int right_width = 0;
  struct wlr_scene_tree *text = NULL;
  struct wlr_scene_node *node = NULL;
  wl_list_for_each(node, &tree->children, link) {
    if (node->type == WLR_SCENE_NODE_RECT) {
      struct wlr_scene_rect *rect = wl_container_of(node, rect, node);
      if (rects == 0) {
        wlr_scene_rect_set_size(rect, width, height);
#ifdef LEME_HAVE_EFFECTS
        wlr_scene_rect_set_corner_radius(rect, (float)inner, 0.0f);
#endif
      } else if (rects == 1) {
        wlr_scene_node_set_enabled(node, inner > 0);
        if (inner > 0) {
          wlr_scene_rect_set_size(rect, width, inner);
          wlr_scene_node_set_position(node, 0, height - inner);
        }
      }
      rects++;
    } else if (node->type == WLR_SCENE_NODE_TREE) {
      struct wlr_scene_tree *child = wl_container_of(node, child, node);
      if (trees == 0) {
        leme_desk_frame_place_buttons(child, width, style, &left_width,
                                      &right_width);
      } else if (trees == 1) {
        text = child;
      }
      trees++;
    }
  }
  if (text != NULL) {
    leme_desk_frame_layout_text(text, width, left_width, right_width,
                                natural_width, style);
  }
}

static int leme_desk_frame_controls_width(const struct leme_desk_frame *frame) {
  return (int)frame->controls.count * frame->style.button_width;
}

bool leme_desk_frame_set_title(struct leme_desk_frame *frame, const char *raw,
                               int titlebar_width, double scale) {
  if (raw == NULL || raw[0] == '\0') {
    raw = "Window";
  }
  const size_t length = strnlen(raw, 4096);
  int width = titlebar_width - leme_desk_frame_controls_width(frame) - 16;
  width = width < 0 ? 0 : (width > 1024 ? 1024 : width);
  if (frame->title != NULL && strlen(frame->title) == length &&
      strncmp(frame->title, raw, length) == 0 && frame->text_width == width &&
      frame->scale == scale) {
    return false;
  }
  char *title = strndup(raw, length);
  struct wlr_scene_tree *text = wlr_scene_tree_create(frame->tree);
  if (frame->text != NULL) {
    wlr_scene_node_destroy(&frame->text->node);
  }
  frame->text = NULL;
  frame->variants[0] = NULL;
  frame->variants[1] = NULL;
  free(frame->title);
  frame->title = title;
  frame->text_width = width;
  frame->scale = scale;
  if (text == NULL || title == NULL) {
    if (text != NULL) {
      wlr_scene_node_destroy(&text->node);
    }
    return true;
  }
  frame->text = text;
  if (width == 0) {
    return true;
  }
  const char *label = g_utf8_validate(title, -1, NULL) ? title : "Window";
  const float *colors[2] = {frame->style.text_inactive,
                            frame->style.text_active};
  int height = 0;
  for (size_t state = 0; state < LEME_ARRAY_LENGTH(colors); state++) {
    struct wlr_scene_tree *variant = wlr_scene_tree_create(text);
    if (variant == NULL) {
      continue;
    }
    frame->variants[state] = variant;
    int line_width = 0;
    int line_height = 0;
    struct wlr_buffer *buffer =
        leme_desk_pixels_line(label, frame->style.font, colors[state], width,
                              scale, &line_width, &line_height);
    if (buffer == NULL) {
      continue;
    }
    if (line_height <= frame->style.height &&
        leme_desk_pixels_place_buffer(
            variant, buffer,
            (struct leme_box){0, 0, line_width, line_height})) {
      struct wlr_scene_node *node = NULL;
      wl_list_for_each(node, &variant->children, link) {
        if (node->type == WLR_SCENE_NODE_BUFFER) {
          struct wlr_scene_buffer *placed = wl_container_of(node, placed, node);
          const struct wlr_fbox source = {.width = buffer->width,
                                          .height = buffer->height};
          wlr_scene_buffer_set_source_box(placed, &source);
        }
      }
      height = line_height;
    }
    wlr_buffer_drop(buffer);
  }
  const int top =
      height < frame->style.height ? (frame->style.height - height) / 2 : 0;
  wlr_scene_node_set_position(&text->node, 8, top);
  leme_desk_frame_show(frame);
  return true;
}

void leme_desk_frame_set_maximized(struct leme_desk_frame *frame,
                                   bool maximized) {
  frame->maximized = maximized;
  leme_desk_frame_show(frame);
}

enum leme_render_decoration_part
leme_desk_frame_hit(const struct leme_desk_frame *frame,
                    const struct wlr_scene_node *node) {
  for (const struct wlr_scene_node *ancestor = node; ancestor != NULL;
       ancestor = ancestor->parent == NULL ? NULL : &ancestor->parent->node) {
    for (size_t i = 0; i < frame->controls.count; i++) {
      if (ancestor == &frame->controls.buttons[i].tree->node) {
        return leme_desk_frame_part(frame->controls.buttons[i].kind);
      }
    }
    if (ancestor == &frame->tree->node) {
      return LEME_RENDER_DECORATION_TITLEBAR;
    }
  }
  return LEME_RENDER_DECORATION_NONE;
}

void leme_desk_frame_set_activated(struct leme_desk_frame *frame,
                                   bool activated) {
  frame->activated = activated;
  const float *color = activated ? frame->style.background_active
                                 : frame->style.background_inactive;
  if (frame->background != NULL) {
    wlr_scene_rect_set_color(frame->background, color);
  }
  if (frame->corners != NULL) {
    wlr_scene_rect_set_color(frame->corners, color);
  }
  leme_desk_frame_show(frame);
}

void leme_desk_frame_feedback(struct leme_desk_frame *frame,
                              enum leme_render_decoration_part part,
                              bool pressed) {
  frame->hover_part = part;
  frame->pressed = pressed;
  for (size_t i = 0; i < frame->controls.count; i++) {
    (void)leme_desk_frame_draw_button(frame, &frame->controls.buttons[i]);
  }
}
