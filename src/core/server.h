#ifndef LEME_SERVER_H
#define LEME_SERVER_H

#include "core/leme.h"
#include "config/config.h"
#include "shell/scratchpad.h"
#include "shell/sticky.h"

#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>

struct leme_capture;
struct leme_ipc;
struct leme_data;
struct leme_desktop;
struct leme_xwayland;
struct leme_input_protocols;
struct leme_layer_surface;
struct leme_output;
struct leme_pointer_grab;
struct leme_session;
struct leme_tearing;
struct leme_view;
struct leme_tags;
struct wlr_alpha_modifier_v1;
struct wlr_compositor;
struct wlr_content_type_manager_v1;
struct wlr_cursor;
struct wlr_fractional_scale_manager_v1;
struct wlr_layer_shell_v1;
struct wlr_linux_dmabuf_v1;
struct wlr_linux_drm_syncobj_manager_v1;
struct wlr_output_manager_v1;
struct wlr_output_power_manager_v1;
struct wlr_presentation;
struct wlr_single_pixel_buffer_manager_v1;
struct wlr_viewporter;
struct wlr_xdg_wm_dialog_v1;
struct wlr_xdg_output_manager_v1;
struct wlr_ext_workspace_manager_v1;
struct wlr_ext_foreign_toplevel_list_v1;
struct wlr_foreign_toplevel_manager_v1;

struct leme_server {
    struct wl_display *display;
    struct wlr_backend *backend;
    struct wlr_backend *headless_backend;
    struct wlr_session *session;
    struct wlr_renderer *renderer;
    struct wlr_allocator *allocator;
    struct wlr_compositor *compositor;
    struct wlr_linux_dmabuf_v1 *linux_dmabuf;
    struct wlr_linux_drm_syncobj_manager_v1 *linux_drm_syncobj;
    struct wlr_presentation *presentation;
    struct wlr_single_pixel_buffer_manager_v1 *single_pixel_buffer_manager;
    struct wlr_alpha_modifier_v1 *alpha_modifier;
    struct wlr_content_type_manager_v1 *content_type_manager;
    struct wlr_scene *scene;
    struct wlr_scene_output_layout *scene_output_layout;
    struct wlr_scene_tree *scene_background;
    struct wlr_scene_tree *scene_bottom;
    struct wlr_scene_tree *scene_tiled;
    struct wlr_scene_tree *scene_floating;
    struct wlr_scene_tree *scene_durable;
    struct wlr_scene_tree *scene_top;
    struct wlr_scene_tree *scene_overlay;
    struct wlr_scene_tree *scene_drag;
    struct wlr_scene_tree *scene_lock;
    struct leme_animation_manager animations;
    struct wlr_output_layout *output_layout;
    struct wl_listener render_output_layout_change;
    struct wl_listener session_active;
    struct wlr_seat *seat;
    struct wlr_xdg_shell *xdg_shell;
    struct wlr_xdg_wm_dialog_v1 *xdg_dialog_manager;
    struct wlr_layer_shell_v1 *layer_shell;
    struct wlr_viewporter *viewporter;
    struct wlr_fractional_scale_manager_v1 *fractional_scale_manager;
    struct wlr_output_manager_v1 *output_manager;
    struct wlr_output_power_manager_v1 *output_power_manager;
    struct wlr_xdg_output_manager_v1 *xdg_output_manager;
    struct wlr_ext_workspace_manager_v1 *workspace_manager;
    struct wlr_ext_foreign_toplevel_list_v1 *foreign_toplevel_list;
    struct wlr_foreign_toplevel_manager_v1 *foreign_toplevel_manager;
    struct wl_event_source *publication_idle;
    struct wl_list workspace_entries;
    struct wl_listener workspace_commit;
    bool publication_dirty;
    struct leme_output *focused_output;
    struct wl_list outputs;
    struct leme_capture *capture;
    struct leme_ipc *ipc;
    struct leme_data *data;
    struct leme_desktop *desktop;
    struct leme_xwayland *xwayland;
    struct leme_input_protocols *input_protocols;
    struct leme_pointer_grab *pointer_grab;
    struct leme_session *session_protocols;
    struct leme_tearing *tearing;
    struct leme_view *focused_view;
    struct leme_scratchpad_manager scratchpads;
    struct leme_sticky_manager sticky;
    struct leme_layer_surface *focused_layer;
    struct wl_list views;
    struct wl_list focus_history;
    struct wl_list layer_surfaces;
    struct wl_list keyboards;
    struct wl_list pointers;
    struct wlr_cursor *cursor;
    struct leme_mode *modes;
    size_t mode_count;
    struct leme_mode *active_mode;
    xkb_layout_index_t keyboard_layout;
    struct wl_listener new_input;
    struct wl_listener cursor_motion;
    struct wl_listener cursor_motion_absolute;
    struct wl_listener cursor_button;
    struct wl_listener cursor_axis;
    struct wl_listener cursor_frame;
    struct wl_listener request_set_cursor;
    struct wl_listener new_output;
    struct wl_listener output_manager_apply;
    struct wl_listener output_manager_test;
    struct wl_listener output_power_set_mode;
    struct wl_listener new_xdg_toplevel;
    struct wl_listener new_layer_surface;
    struct wl_event_source *sigint_source;
    const char *socket;
    bool started;
    struct leme_config *config;
};

bool leme_server_init(struct leme_server *server);
int leme_server_run(struct leme_server *server);
void leme_server_finish(struct leme_server *server);

#endif
