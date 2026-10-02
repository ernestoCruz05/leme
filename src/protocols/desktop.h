#ifndef LEME_DESKTOP_H
#define LEME_DESKTOP_H

#include <stdbool.h>

struct leme_config;
struct leme_server;

bool leme_desktop_init(struct leme_server *server);
void leme_desktop_finish(struct leme_server *server);
void leme_desktop_output_changed(struct leme_server *server);
bool leme_desktop_apply_cursor_config(struct leme_server *server,
                                      const struct leme_config *config);
struct wlr_seat_client;
void leme_desktop_cursor_surface_set(struct leme_server *server,
                                     struct wlr_seat_client *client);
bool leme_desktop_cursor_owned_by(const struct leme_server *server,
                                  const struct wlr_seat_client *client);
void leme_desktop_cursor_default(struct leme_server *server);
bool leme_desktop_cursor_override(struct leme_server *server, const char *name);
void leme_desktop_cursor_restore(struct leme_server *server);
bool leme_desktop_has_cursor(const struct leme_server *server);
struct wlr_xcursor_manager;
struct wlr_xcursor_manager *
leme_desktop_prepare_cursor_config(struct leme_server *server,
                                   const char *theme, int size);
void leme_desktop_commit_cursor_config(struct leme_server *server,
                                       struct wlr_xcursor_manager *replacement);
void leme_desktop_discard_cursor_config(
    struct wlr_xcursor_manager *replacement);

#endif
