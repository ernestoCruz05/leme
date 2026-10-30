#ifndef LEME_DESK_DECORATION_H
#define LEME_DESK_DECORATION_H

#include "core/leme.h"
#include "render/render.h"

#include <stdbool.h>

struct leme_view;
struct leme_server;
struct wlr_scene_tree;
struct wlr_scene_node;
struct leme_titlebar_style;

#define LEME_DESK_RESIZE_MARGIN 8
#define LEME_DESK_RESIZE_CORNER 16

struct leme_desk_decoration_boxes {
  struct leme_box content;
  struct leme_box titlebar;
};

bool leme_desk_decoration_eligible(const struct leme_view *view);
bool leme_desk_decoration_eligible_for_state(const struct leme_view *view,
                                             bool fullscreen);
bool leme_desk_decoration_geometry(struct leme_box frame, int border_width,
                                   int titlebar_height, int button_width,
                                   struct leme_desk_decoration_boxes *boxes);

uint32_t leme_desk_decoration_resize_edges(struct leme_box frame,
                                           int border_width, double lx,
                                           double ly);

void leme_desk_decoration_init(struct leme_server *server);
void leme_desk_decoration_finish(struct leme_server *server);
bool leme_desk_decoration_sync(struct leme_view *view);
void leme_desk_decoration_refresh(struct leme_view *view);
void leme_desk_decoration_set_activated(struct leme_view *view, bool activated);
void leme_desk_decoration_view_finish(struct leme_view *view);
int leme_desk_decoration_height(const struct leme_view *view,
                                struct leme_box frame);
struct wlr_scene_tree *leme_desk_decoration_tree(const struct leme_view *view);
void leme_desk_frames_restyle(struct leme_server *server);

bool leme_desk_decoration_hit(const struct leme_view *view,
                              const struct wlr_scene_node *node,
                              struct leme_render_hit *hit);
void leme_desk_decoration_feedback(struct leme_view *view,
                                   enum leme_render_decoration_part part,
                                   bool pressed);

#endif
