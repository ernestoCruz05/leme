#ifndef LEME_RENDER_H
#define LEME_RENDER_H

#include "core/leme.h"
#include "render/animation.h"

struct leme_config;
struct leme_container;
struct leme_drag_icon;
struct leme_layer_popup;
struct leme_layer_surface;
struct leme_output;
struct leme_server;
struct wlr_scene_node;
struct leme_session;
struct leme_lock_surface;
struct leme_view;
struct leme_view_popup;
struct leme_view_rules;
struct wlr_output_state;
struct wlr_scene_rect;
struct wlr_scene_tree;
struct wlr_surface;
struct leme_titlebar_style;

struct leme_render_preview {
  struct wlr_scene_rect *rect;
  struct leme_box target;
  struct leme_box current;
};

#define LEME_RENDER_VIEW_SHELL_MAX 8

struct leme_render_view_frame_nodes {
  struct wlr_scene_rect *border[4];
  struct wlr_scene_tree *content;
  struct wlr_scene_tree *decoration;
  struct wlr_scene_tree *shell[LEME_RENDER_VIEW_SHELL_MAX];
  struct leme_gaps margins;
  int titlebar_height;
  int titlebar_width;
  int button_width;
};

enum leme_render_decoration_part {
  LEME_RENDER_DECORATION_NONE,
  LEME_RENDER_DECORATION_TITLEBAR,
  LEME_RENDER_DECORATION_MINIMIZE,
  LEME_RENDER_DECORATION_MAXIMIZE,
  LEME_RENDER_DECORATION_CLOSE,
};

struct leme_render_hit {
  enum leme_render_decoration_part decoration_part;
  uint32_t resize_edges;
  struct wlr_surface *surface;
  struct leme_output *desk_output;
  struct leme_output *strip_output;
  struct leme_view *view;
  struct leme_container *container;
  struct leme_layer_surface *layer;
  double surface_x;
  double surface_y;
};

void leme_render_view_identify(struct leme_view *view);
bool leme_render_container_create(struct leme_container *container);
void leme_render_container_destroy(struct leme_container *container);
void leme_render_container_move(struct leme_container *container,
                                struct leme_box from, struct leme_box to);
void leme_render_container_finish_move(struct leme_container *container);
void leme_render_container_fly(struct leme_container *container,
                               struct leme_box from, struct leme_box to,
                               bool appearing);
bool leme_render_container_animation_box(const struct leme_container *container,
                                         struct leme_box *out);
bool leme_render_container_ready(const struct leme_container *container);
bool leme_render_view_move_settled(const struct leme_view *view);
uint32_t
leme_render_container_resize_edges(const struct leme_container *container,
                                   double lx, double ly);
void leme_render_container_set_box(struct leme_container *container,
                                   struct leme_box box);
void leme_render_container_set_visible(struct leme_container *container,
                                       bool visible);
void leme_render_container_restack(struct leme_container *container,
                                   bool raise);
void leme_render_container_set_activated(struct leme_container *container,
                                         bool activated);
void leme_render_init(struct leme_server *server);
void leme_render_finish(struct leme_server *server);
struct wlr_scene_tree *
leme_render_durable_group_create(struct leme_server *server);
void leme_render_durable_group_raise(struct wlr_scene_tree *group);
void leme_render_durable_group_destroy(struct wlr_scene_tree **group);
bool leme_render_prepare_output_attachment(struct leme_output *output);
bool leme_render_attach_output(struct leme_output *output);
void leme_render_position_output(struct leme_output *output);
void leme_render_detach_output(struct leme_output *output);
bool leme_render_prepare_output_state(struct leme_output *output,
                                      struct wlr_output_state *state,
                                      bool testing);
void leme_render_output_frame(struct leme_output *output);
void leme_render_handle_session_active(struct leme_server *server, bool active);
bool leme_render_drag_icon_create(struct leme_drag_icon *icon);
void leme_render_drag_icon_update(struct leme_drag_icon *icon, double layout_x,
                                  double layout_y);
void leme_render_drag_icon_destroy(struct leme_drag_icon *icon);
bool leme_render_preview_show(struct leme_server *server,
                              struct leme_render_preview *preview,
                              struct leme_box box, double cursor_x,
                              double cursor_y);
void leme_render_preview_hide(struct leme_server *server,
                              struct leme_render_preview *preview);
bool leme_render_layer_create(struct leme_layer_surface *layer);
void leme_render_layer_destroy(struct leme_layer_surface *layer);
void leme_render_layer_update_tree(struct leme_layer_surface *layer);
void leme_render_layers_refresh_output(struct leme_server *server);
void leme_render_layer_configure(struct leme_layer_surface *layer,
                                 struct leme_box full, struct leme_box *usable);
bool leme_render_layer_popup_create(struct leme_layer_popup *popup);
void leme_render_layer_popup_destroy(struct leme_layer_popup *popup);
void leme_render_layer_popup_unconstrain(struct leme_layer_popup *popup,
                                         struct leme_box full);
void leme_render_view_create(struct leme_view *view);
void leme_render_view_destroy(struct leme_view *view);
void leme_render_view_animate(struct leme_view *view,
                              enum leme_animation_event event);
void leme_render_view_finish_animation(struct leme_view *view);
void leme_render_view_sync_presentation(struct leme_view *view);
void leme_render_view_open_ready(struct leme_view *view);
bool leme_render_view_popup_create(struct leme_view_popup *popup);
struct leme_box
leme_render_view_popup_constraint_box(struct leme_box full, int view_x,
                                      int view_y,
                                      struct leme_box root_geometry);
void leme_render_view_popup_unconstrain(struct leme_view_popup *popup,
                                        struct leme_box full);
void leme_render_view_popup_destroy(struct leme_view_popup *popup);
int leme_render_view_corner_radius(const struct leme_view *view,
                                   struct leme_box frame);
int leme_render_view_blur(const struct leme_view *view);
struct wl_array;
bool leme_render_view_collect_content(struct wl_array *bases,
                                      struct wlr_scene_tree *tree, int origin_x,
                                      int origin_y);
void leme_render_view_fade_nodes(struct wl_array *bases, float alpha);
void leme_render_view_scale_nodes(struct wl_array *bases, double ratio_x,
                                  double ratio_y);
void leme_render_view_layout_frame(
    const struct leme_render_view_frame_nodes *nodes, struct leme_box box,
    int border_width, int corner_radius,
    const struct leme_titlebar_style *titlebar);
uint32_t leme_render_view_resize_edges(const struct leme_view *view, double lx,
                                       double ly);
void leme_render_view_set_box(struct leme_view *view, struct leme_box box);
void leme_render_view_move(struct leme_view *view, struct leme_box from,
                           struct leme_box to);
bool leme_render_view_animation_box(const struct leme_view *view,
                                    struct leme_box *out);
void leme_render_view_finish_move(struct leme_view *view);
void leme_render_view_fly(struct leme_view *view, struct leme_box from,
                          struct leme_box to, bool appearing);
bool leme_render_view_flying(const struct leme_view *view);
void leme_render_view_clip_to_geometry(struct leme_view *view);
struct leme_box leme_render_view_local_box(const struct leme_view *view,
                                           struct leme_box global);
bool leme_render_view_show_drop_preview(struct leme_view *view,
                                        struct leme_box global);
void leme_render_view_hide_drop_preview(struct leme_view *view);
void leme_render_set_view_visible(struct leme_view *view, bool visible);
void leme_render_view_focus(struct leme_view *view);
void leme_render_view_restack(struct leme_view *view);
bool leme_render_view_owns_node(const struct leme_view *view,
                                const struct wlr_scene_node *node);
struct wlr_scene_node *
leme_render_view_stack_above(struct leme_view *view,
                             struct wlr_scene_node *anchor);
void leme_render_view_set_activated(struct leme_view *view, bool activated);
struct wlr_scene_tree *
leme_render_view_snapshot(const struct leme_view *view,
                          struct wlr_scene_tree *parent,
                          struct leme_render_view_frame_nodes *nodes);
void leme_render_view_apply_snapshot(
    const struct leme_view *view,
    const struct leme_render_view_frame_nodes *nodes, bool activated);
float leme_render_view_opacity(const struct leme_config *config, bool activated,
                               bool fullscreen,
                               const struct leme_view_rules *rules);
void leme_render_view_update_layer(struct leme_view *view);
struct leme_box
leme_render_view_content_box_for_state(const struct leme_view *view,
                                       struct leme_box frame, bool fullscreen);
struct leme_box leme_render_view_content_box(const struct leme_view *view,
                                             struct leme_box frame);
struct leme_box leme_render_view_frame_box(const struct leme_view *view,
                                           struct leme_box content);
struct leme_box
leme_render_view_frame_box_for_state(const struct leme_view *view,
                                     struct leme_box content, bool fullscreen);
void leme_render_refresh_views(struct leme_server *server);
void leme_render_apply_fullscreen_coverage(struct leme_server *server);
bool leme_render_xwayland_create(struct leme_view *view);
void leme_render_xwayland_set_geometry(struct leme_view *view);
void leme_render_xwayland_destroy(struct leme_view *view);
void leme_render_drop_at(struct leme_server *server,
                         const struct leme_view *source,
                         const struct wlr_scene_node *preview, double lx,
                         double ly, struct leme_render_hit *hit);
bool leme_render_at(struct leme_server *server, double lx, double ly,
                    struct leme_render_hit *hit);
bool leme_render_surface_visible(struct leme_server *server,
                                 struct wlr_surface *surface);
bool leme_render_session_create(struct leme_session *session);
void leme_render_session_destroy(struct leme_session *session);
void leme_render_session_set_locked(struct leme_session *session, bool locked);
bool leme_render_session_prepare_output_wake(struct leme_session *session,
                                             struct leme_box box);
bool leme_render_lock_surface_create(struct leme_lock_surface *surface);
void leme_render_lock_surface_configure(struct leme_lock_surface *surface);
void leme_render_lock_surface_destroy(struct leme_lock_surface *surface);

#endif
