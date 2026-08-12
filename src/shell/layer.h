#ifndef LEME_LAYER_H
#define LEME_LAYER_H

#include <stdbool.h>
#include <wayland-server-core.h>

struct leme_output;
struct leme_server;
struct wlr_layer_surface_v1;
struct wlr_output;
struct wlr_scene_layer_surface_v1;
struct wlr_xdg_popup;
struct wlr_scene_tree;

struct leme_layer_popup;

struct leme_layer_surface {
    struct leme_server *server;
    struct leme_output *output;
    struct wlr_layer_surface_v1 *wlr_layer_surface;
    struct wlr_scene_tree *render_tree;
    struct wlr_scene_layer_surface_v1 *scene_layer_surface;
    struct wl_list link;
    struct wl_list popups;
    struct wl_listener commit;
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener new_popup;
    struct wl_listener destroy;
    bool mapped;
};

struct leme_layer_popup {
    struct leme_layer_surface *layer;
    struct wlr_xdg_popup *wlr_popup;
    struct wlr_scene_tree *scene_tree;
    struct wl_list link;
    struct wl_listener commit;
    struct wl_listener destroy;
};

bool leme_layer_init(struct leme_server *server);
void leme_layer_finish(struct leme_server *server);
void leme_layer_handle_output_destroy(struct leme_server *server,
    struct wlr_output *output);
bool leme_layer_keyboard_is_exclusive(const struct leme_server *server);
void leme_layer_focus_on_button(struct leme_layer_surface *layer);
void leme_layer_release_for_view(struct leme_server *server);
void leme_layer_restore_keyboard_focus(struct leme_server *server);

#endif
