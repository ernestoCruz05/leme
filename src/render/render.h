#ifndef LEME_RENDER_H
#define LEME_RENDER_H

#include "core/leme.h"
#include "render/animation.h"

struct leme_config;
struct leme_drag_icon;
struct leme_layer_popup;
struct leme_layer_surface;
struct leme_output;
struct leme_server;
struct leme_session;
struct leme_lock_surface;
struct leme_view;
struct leme_view_popup;
struct leme_view_rules;
struct wlr_output_state;
struct wlr_scene_rect;
struct wlr_scene_tree;
struct wlr_surface;

struct leme_render_view_frame_nodes {
    struct wlr_scene_rect *border[4];
    struct wlr_scene_tree *content;
};

struct leme_render_hit {
    struct wlr_surface *surface;
    struct leme_view *view;
    struct leme_layer_surface *layer;
    double surface_x;
    double surface_y;
};

void leme_render_init(struct leme_server *server);
void leme_render_finish(struct leme_server *server);
struct wlr_scene_tree *leme_render_durable_group_create(
    struct leme_server *server);
void leme_render_durable_group_raise(struct wlr_scene_tree *group);
void leme_render_durable_group_destroy(struct wlr_scene_tree **group);
bool leme_render_prepare_output_attachment(struct leme_output *output);
bool leme_render_attach_output(struct leme_output *output);
void leme_render_position_output(struct leme_output *output);
void leme_render_detach_output(struct leme_output *output);
bool leme_render_prepare_output_state(struct leme_output *output,
    struct wlr_output_state *state, bool testing);
void leme_render_output_frame(struct leme_output *output);
void leme_render_handle_session_active(
    struct leme_server *server, bool active);
bool leme_render_drag_icon_create(struct leme_drag_icon *icon);
void leme_render_drag_icon_update(struct leme_drag_icon *icon,
    double layout_x, double layout_y);
void leme_render_drag_icon_destroy(struct leme_drag_icon *icon);
bool leme_render_layer_create(struct leme_layer_surface *layer);
void leme_render_layer_destroy(struct leme_layer_surface *layer);
void leme_render_layer_update_tree(struct leme_layer_surface *layer);
void leme_render_layers_refresh_output(struct leme_server *server);
void leme_render_layer_configure(struct leme_layer_surface *layer,
    struct leme_box full, struct leme_box *usable);
bool leme_render_layer_popup_create(struct leme_layer_popup *popup);
void leme_render_layer_popup_update(struct leme_layer_popup *popup);
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
struct leme_box leme_render_view_popup_constraint_box(
    struct leme_box full, int view_x, int view_y,
    struct leme_box root_geometry);
void leme_render_view_popup_unconstrain(struct leme_view_popup *popup,
    struct leme_box full);
void leme_render_view_popup_destroy(struct leme_view_popup *popup);
int leme_render_view_corner_radius(const struct leme_view *view,
    struct leme_box frame);
int leme_render_view_blur(const struct leme_view *view);
void leme_render_view_layout_frame(
    const struct leme_render_view_frame_nodes *nodes,
    struct leme_box box, int border_width, int corner_radius);
void leme_render_view_set_box(struct leme_view *view, struct leme_box box);
void leme_render_view_clip_to_geometry(struct leme_view *view);
struct leme_box leme_render_view_local_box(
    const struct leme_view *view, struct leme_box global);
bool leme_render_view_show_drop_preview(
    struct leme_view *view, struct leme_box global);
void leme_render_view_hide_drop_preview(struct leme_view *view);
void leme_render_set_view_visible(struct leme_view *view, bool visible);
void leme_render_view_focus(struct leme_view *view);
void leme_render_view_set_activated(struct leme_view *view, bool activated);
void leme_render_view_apply_active_snapshot(
    const struct leme_view *view, struct wlr_scene_tree *snapshot);
float leme_render_view_opacity(const struct leme_config *config,
    bool activated, bool fullscreen, const struct leme_view_rules *rules);
void leme_render_view_update_layer(struct leme_view *view);
struct leme_box leme_render_view_content_box(
    const struct leme_view *view, struct leme_box frame);
struct leme_box leme_render_view_frame_box(
    const struct leme_view *view, struct leme_box content);
void leme_render_refresh_views(struct leme_server *server);
bool leme_render_xwayland_create(struct leme_view *view);
void leme_render_xwayland_set_geometry(struct leme_view *view);
void leme_render_xwayland_destroy(struct leme_view *view);
bool leme_render_at(struct leme_server *server,
    double lx, double ly, struct leme_render_hit *hit);
bool leme_render_surface_visible(struct leme_server *server,
    struct wlr_surface *surface);
bool leme_render_session_create(struct leme_session *session);
void leme_render_session_destroy(struct leme_session *session);
void leme_render_session_set_locked(
    struct leme_session *session, bool locked);
bool leme_render_session_prepare_output_wake(
    struct leme_session *session, struct leme_box box);
bool leme_render_lock_surface_create(struct leme_lock_surface *surface);
void leme_render_lock_surface_configure(struct leme_lock_surface *surface);
void leme_render_lock_surface_destroy(struct leme_lock_surface *surface);

#endif
