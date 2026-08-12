#ifndef LEME_VIEW_H
#define LEME_VIEW_H

#include "shell/scratchpad.h"
#include "workspace/layout.h"

#include <wayland-server-core.h>
#include <wlr/types/wlr_xdg_shell.h>

struct leme_output;
struct leme_publication_toplevel;
struct leme_render_view_state;
struct leme_server;
struct leme_tag;
struct leme_tags;
struct leme_view;
struct wlr_scene_rect;
struct wlr_scene_tree;
struct wlr_surface;
struct wlr_xwayland_surface;

enum leme_view_kind {
    LEME_VIEW_XDG,
    LEME_VIEW_XWAYLAND,
};

struct leme_view_map_options {
    struct leme_tags *tags;
    uint16_t tag_id;
    const struct leme_view *parent;
    bool floating;
    bool unmanaged;
    bool scratchpad_direct;
};

struct leme_view_popup {
    struct leme_view *view;
    struct wlr_xdg_popup *wlr_popup;
    struct wlr_scene_tree *scene_tree;
    struct wl_listener commit;
    struct wl_listener reposition;
    struct wl_listener new_popup;
    struct wl_listener destroy;
    struct wl_list link;
};

struct leme_view {
    struct leme_server *server;
    enum leme_view_kind kind;
    struct wlr_xdg_toplevel *xdg_toplevel;
    struct wlr_xwayland_surface *xwayland_surface;
    struct wlr_scene_tree *render_tree;
    struct wlr_scene_tree *scene_tree;
    struct wlr_scene_tree *capture_epoch;
    struct wl_listener capture_epoch_destroy;
    size_t capture_epoch_destroy_count;
    struct wlr_scene_rect *border[4];
    struct wlr_scene_rect *drop_preview;
    struct wl_list link;
    struct wl_list tag_link;
    struct wl_list focus_link;
    struct wl_list scratchpad_link;
    struct wl_list popups;
    struct leme_tag *tag;
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener commit;
    struct wl_listener set_title;
    struct wl_listener set_app_id;
    struct wl_listener request_fullscreen;
    struct wl_listener request_move;
    struct wl_listener request_resize;
    struct wl_listener new_popup;
    struct wl_listener destroy;
    enum leme_scratchpad_state scratchpad_state;
    char *scratchpad_name;
    struct leme_box scratchpad_anchor_area;
    /*
     * Um cliente XWayland leva um ConfigureNotify por cada evento de
     * ponteiro, o que o afoga durante um arrasto. Enquanto está adiado, a
     * caixa acumula aqui e sai um único configure por frame.
     */
    struct leme_box deferred_configure_box;
    bool configure_deferred;
    bool configure_dirty;
    bool mapped;
    bool floating;
    bool detached; /* Tiled drag transaction only; never scratchpad ownership. */
    bool unmanaged;
    bool fullscreen;
    bool open_animation_pending;
    uint32_t open_animation_serial;
    struct wl_event_source *open_animation_timeout;
    float render_opacity;
    void *animation_state;
    struct leme_render_view_state *render_state;
    struct leme_publication_toplevel *publication;
    struct leme_box box;
    struct leme_box saved_box;
};

void leme_view_init(struct leme_server *server);
void leme_view_finish(struct leme_server *server);
struct wlr_surface *leme_view_surface(const struct leme_view *view);
const char *leme_view_identity(const struct leme_view *view);
const char *leme_view_title(const struct leme_view *view);
struct leme_view *leme_view_from_surface(
    struct leme_server *server, struct wlr_surface *surface);
void leme_view_set_activated(struct leme_view *view, bool activated);
void leme_view_configure(struct leme_view *view, struct leme_box box);
void leme_view_set_configure_deferred(struct leme_view *view, bool deferred);
void leme_view_flush_deferred_configure(struct leme_view *view);
void leme_view_flush_deferred_configures(struct leme_server *server);
void leme_view_ack_fullscreen(struct leme_view *view, bool fullscreen);
void leme_view_protocol_close(struct leme_view *view);
bool leme_view_map(struct leme_view *view,
    const struct leme_view_map_options *options);
void leme_view_unmap(struct leme_view *view);
void leme_view_destroy_core(struct leme_view *view);
void leme_view_focus(struct leme_view *view);
void leme_view_focus_history_remove(struct leme_view *view);
void leme_view_clear_focus(struct leme_server *server);
void leme_view_focus_next(struct leme_server *server);
bool leme_view_focus_direction(
    struct leme_server *server, enum leme_direction direction);
bool leme_view_focus_previous(struct leme_server *server);
bool leme_view_move_direction(
    struct leme_server *server, enum leme_direction direction, int amount);
bool leme_view_begin_tiled_drag(struct leme_view *view,
    struct leme_layout_detach **detach);
bool leme_view_commit_tiled_drag(struct leme_view *view,
    struct leme_layout_detach **detach, struct leme_view *target,
    enum leme_direction direction);
bool leme_view_restore_tiled_drag(struct leme_view *view,
    struct leme_layout_detach **detach);
void leme_view_discard_tiled_drag(struct leme_view *view,
    struct leme_layout_detach **detach);
void leme_view_close(struct leme_view *view);
void leme_view_arrange(struct leme_server *server);
void leme_view_refresh_tag_focus(struct leme_server *server);
bool leme_view_move_to_output(struct leme_view *view,
    struct leme_output *output, bool follow);
bool leme_view_drop_to_output(struct leme_view *view,
    struct leme_layout_detach **detach, struct leme_output *output,
    struct leme_view *target, enum leme_direction direction);
bool leme_view_set_floating(struct leme_view *view, bool floating);
bool leme_view_set_fullscreen(struct leme_view *view, bool fullscreen);
bool leme_view_resize(struct leme_view *view,
    enum leme_resize_edge edge, int amount);
bool leme_view_apply_interactive_box(
    struct leme_view *view, struct leme_box box, bool resizing);
void leme_view_apply_layout_box(struct leme_view *view, struct leme_box box);

#endif
