#ifndef LEME_DATA_H
#define LEME_DATA_H

#include <stdbool.h>
#include <wayland-server-core.h>

struct leme_server;
struct wlr_data_control_manager_v1;
struct wlr_data_device_manager;
struct wlr_drag_icon;
struct wlr_ext_data_control_manager_v1;
struct wlr_primary_selection_v1_device_manager;
struct wlr_scene_tree;

struct leme_drag_icon;

struct leme_data {
    struct leme_server *server;
    struct wlr_data_device_manager *data_device_manager;
    struct wlr_primary_selection_v1_device_manager *primary_selection_manager;
    struct wlr_data_control_manager_v1 *wlr_data_control_manager;
    struct wlr_ext_data_control_manager_v1 *ext_data_control_manager;
    struct wl_listener request_set_selection;
    struct wl_listener request_set_primary_selection;
    struct wl_listener request_start_drag;
    struct wl_listener start_drag;
    struct leme_drag_icon *drag_icon;
};

struct leme_drag_icon {
    struct leme_data *data;
    struct wlr_drag_icon *wlr_drag_icon;
    struct wlr_scene_tree *scene_tree;
    struct wl_listener destroy;
};

bool leme_data_init(struct leme_server *server);
void leme_data_finish(struct leme_server *server);
void leme_data_update_drag_icon(struct leme_server *server);
void leme_data_cancel_drag(struct leme_server *server);

#endif
