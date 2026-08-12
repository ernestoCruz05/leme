#ifndef LEME_INPUT_H
#define LEME_INPUT_H

#include "core/command.h"

#include <stddef.h>
#include <stdint.h>
#include <xkbcommon/xkbcommon.h>

struct leme_binding {
    uint32_t modifiers;
    xkb_keysym_t keysym;
    struct leme_command command;
    int lineno;
};

struct leme_mode {
    char *name;
    struct leme_binding *bindings;
    size_t binding_count;
    bool escape_exits;
};

struct leme_config;
struct leme_server;
struct leme_view;

struct xkb_keymap *leme_input_compile_keymap(
    const struct leme_config *config);
bool leme_input_apply_keymap(struct leme_server *server,
    struct xkb_keymap *keymap);
bool leme_input_cycle_keyboard_layout(struct leme_server *server);
void leme_input_apply_pointer_config(struct leme_server *server,
    const struct leme_config *config);
void leme_input_init(struct leme_server *server);
void leme_input_finish(struct leme_server *server);
void leme_input_replace_modes(struct leme_server *server,
    struct leme_mode *modes, size_t mode_count);
bool leme_input_set_mode(struct leme_server *server, const char *name);
void leme_input_refresh_pointer_focus(struct leme_server *server,
    uint32_t time_msec);
bool leme_input_pointer_grab_active(const struct leme_server *server);
bool leme_input_pointer_grab_start_xdg(struct leme_view *view,
    bool resize, uint32_t serial, uint32_t wlr_edges);
bool leme_input_pointer_grab_start_xwayland(struct leme_view *view,
    bool resize, uint32_t wlr_edges);
void leme_input_pointer_grab_cancel(struct leme_server *server);
void leme_input_pointer_grab_cancel_tiled(struct leme_server *server);
void leme_input_pointer_grab_cancel_view(struct leme_view *view);
void leme_input_pointer_grab_finish(struct leme_server *server);

#endif
