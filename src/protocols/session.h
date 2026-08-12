#ifndef LEME_SESSION_H
#define LEME_SESSION_H

#include <stdbool.h>
#include <wayland-server-core.h>

struct leme_box;
struct leme_command;
struct leme_server;
struct leme_lock_surface;
struct wlr_session_lock_v1;
struct wlr_surface;
struct wlr_idle_inhibit_manager_v1;
struct wlr_idle_notifier_v1;
struct wlr_scene_rect;
struct wlr_scene_tree;
struct wlr_session_lock_manager_v1;
struct wlr_session_lock_surface_v1;

struct leme_session {
    struct leme_server *server;
    struct wlr_session_lock_manager_v1 *lock_manager;
    struct wlr_idle_notifier_v1 *idle_notifier;
    struct wlr_idle_inhibit_manager_v1 *idle_inhibit_manager;
    struct wlr_scene_rect *lock_blocker;
    struct wlr_session_lock_v1 *lock;
    struct wl_list inhibitors;
    struct wl_list lock_surfaces;
    struct wl_listener new_lock;
    struct wl_listener new_inhibitor;
    struct wl_listener lock_new_surface;
    struct wl_listener lock_unlock;
    struct wl_listener lock_destroy;
    bool locked;
    bool abandoned;
    bool unlocking;
};

struct leme_lock_surface {
    struct leme_session *session;
    struct wlr_session_lock_surface_v1 *wlr_lock_surface;
    struct wlr_scene_tree *scene_tree;
    struct wl_list link;
    struct wl_listener commit;
    struct wl_listener destroy;
    bool configured;
};

bool leme_session_init(struct leme_server *server);
void leme_session_finish(struct leme_server *server);
void leme_session_notify_activity(struct leme_server *server);
bool leme_session_locked(const struct leme_server *server);
void leme_session_refresh_idle_inhibitors(struct leme_server *server);
bool leme_session_surface_allowed(const struct leme_server *server,
    struct wlr_surface *surface);
bool leme_session_command_allowed(const struct leme_server *server,
    const struct leme_command *command);
void leme_session_output_changed(struct leme_server *server);
bool leme_session_prepare_output_wake(
    struct leme_server *server, const struct leme_box *box);
void leme_session_restore_output_wake(struct leme_server *server);
void leme_session_restore_focus(struct leme_server *server);

#endif
