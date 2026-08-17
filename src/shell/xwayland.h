#ifndef LEME_XWAYLAND_H
#define LEME_XWAYLAND_H

#include <stdbool.h>
#include <wayland-server-core.h>

struct leme_server;
struct wlr_xcursor_manager;
struct wlr_xwayland;

struct leme_xwayland {
    struct leme_server *server;
    struct wlr_xwayland *wlr_xwayland;
    struct wl_listener ready;
    struct wl_listener new_surface;
    struct wl_listener destroy;
    struct wl_list views;
    int startup_fd;
    bool is_ready;
};

void leme_xwayland_init(struct leme_server *server);
bool leme_xwayland_start(struct leme_server *server);
void leme_xwayland_finish(struct leme_server *server);
const char *leme_xwayland_display(const struct leme_server *server);
bool leme_xwayland_ready(const struct leme_server *server);
void leme_xwayland_update_workarea(struct leme_server *server);
void leme_xwayland_set_root_cursor(struct leme_server *server,
    struct wlr_xcursor_manager *manager, const char *name, float scale);

#endif
