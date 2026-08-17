#ifndef LEME_DESKTOP_H
#define LEME_DESKTOP_H

#include <stdbool.h>

struct leme_config;
struct leme_server;
struct leme_view;

bool leme_desktop_activation_target_eligible(const struct leme_server *server,
    const struct leme_view *view);
bool leme_desktop_init(struct leme_server *server);
void leme_desktop_finish(struct leme_server *server);
void leme_desktop_output_changed(struct leme_server *server);
bool leme_desktop_apply_cursor_config(struct leme_server *server,
    const struct leme_config *config);
void leme_desktop_cursor_surface_set(struct leme_server *server);
bool leme_desktop_cursor_override(
    struct leme_server *server, const char *name);
void leme_desktop_cursor_restore(struct leme_server *server);

#endif
