#ifndef LEME_DESK_FRAME_H
#define LEME_DESK_FRAME_H

#include "config/config.h"
#include "render/render.h"

struct leme_server;
struct wlr_scene_node;

struct leme_desk_button {
  struct wlr_scene_tree *tree;
  struct wlr_scene_rect *hit;
  struct wlr_scene_tree *face;
  uint32_t states;
  enum leme_titlebar_button kind;
};

struct leme_desk_controls {
  struct wlr_scene_tree *tree;
  struct leme_desk_button buttons[3];
  size_t count;
  double scale;
};

struct leme_desk_frame {
  struct leme_server *server;
  struct wlr_scene_tree *tree;
  struct wlr_scene_rect *background;
  struct wlr_scene_rect *corners;
  struct leme_desk_controls controls;
  struct wlr_scene_tree *text;
  struct wlr_scene_tree *variants[2];
  struct leme_titlebar_style style;
  char *title;
  int text_width;
  double scale;
  bool activated;
  bool maximized;
  bool available[3];
  enum leme_render_decoration_part hover_part;
  bool pressed;
};

bool leme_desk_frame_init(struct leme_desk_frame *frame,
                          struct leme_server *server,
                          struct wlr_scene_tree *parent, double scale,
                          const struct leme_titlebar_style *style);
void leme_desk_frame_finish(struct leme_desk_frame *frame);
bool leme_desk_frame_set_style(struct leme_desk_frame *frame, double scale,
                               const struct leme_titlebar_style *style);
bool leme_desk_frame_set_title(struct leme_desk_frame *frame, const char *title,
                               int titlebar_width, double scale);
void leme_desk_frame_set_maximized(struct leme_desk_frame *frame,
                                   bool maximized);
void leme_desk_frame_layout(struct wlr_scene_tree *tree, struct leme_box box,
                            int border, int height, int natural_width,
                            const struct leme_titlebar_style *style,
                            int radius);
enum leme_render_decoration_part
leme_desk_frame_hit(const struct leme_desk_frame *frame,
                    const struct wlr_scene_node *node);
void leme_desk_frame_set_activated(struct leme_desk_frame *frame,
                                   bool activated);
void leme_desk_frame_feedback(struct leme_desk_frame *frame,
                              enum leme_render_decoration_part part,
                              bool pressed);

#endif
