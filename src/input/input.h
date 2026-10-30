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
  bool repeat;
};

struct leme_mode {
  char *name;
  struct leme_binding *bindings;
  size_t binding_count;
  bool escape_exits;
};

struct leme_config;
struct leme_server;
struct leme_tags;
struct leme_view;
struct leme_container;
struct wlr_surface;

struct xkb_keymap *leme_input_compile_keymap(const struct leme_config *config);
bool leme_input_apply_keymap(struct leme_server *server,
                             struct xkb_keymap *keymap);
bool leme_input_cycle_keyboard_layout(struct leme_server *server);
bool leme_input_select_keyboard_layout(struct leme_server *server,
                                       const char *label);
void leme_input_apply_pointer_config(struct leme_server *server,
                                     const struct leme_config *config);
void leme_input_init(struct leme_server *server);
void leme_input_finish(struct leme_server *server);
void leme_input_replace_modes(struct leme_server *server,
                              struct leme_mode *modes, size_t mode_count);
bool leme_input_set_mode(struct leme_server *server, const char *name);
void leme_input_keyboard_enter(struct leme_server *server,
                               struct wlr_surface *surface);
void leme_input_apply_keyboard_repeat(struct leme_server *server);
void leme_input_refresh_pointer_focus(struct leme_server *server,
                                      uint32_t time_msec);
bool leme_input_idle(const struct leme_server *server);
bool leme_input_interacting(const struct leme_server *server);
uint32_t leme_input_modifiers(const struct leme_server *server);
void leme_input_pointer_rebase(struct leme_server *server);
bool leme_input_pointer_grab_active(const struct leme_server *server);
bool leme_input_pointer_grabbing(const struct leme_server *server,
                                 const struct leme_view *view);
bool leme_input_pointer_grab_moves_container(
    const struct leme_container *container);
bool leme_input_pointer_grab_start_container(struct leme_container *container,
                                             uint32_t button);
bool leme_input_pointer_grab_start_container_resize(
    struct leme_container *container, enum leme_grab_edge edges,
    uint32_t button);
bool leme_input_pointer_grab_start_decoration(struct leme_view *view,
                                              uint32_t button);
bool leme_input_pointer_grab_start_xdg(struct leme_view *view, bool resize,
                                       uint32_t serial, uint32_t wlr_edges);
bool leme_input_pointer_grab_start_xwayland(struct leme_view *view, bool resize,
                                            uint32_t wlr_edges);
void leme_input_pointer_grab_cancel(struct leme_server *server);
void leme_input_pointer_grab_modifiers_changed(struct leme_server *server);
bool leme_input_pointer_grab_abort(struct leme_server *server);
void leme_input_pointer_grab_cancel_tiled(struct leme_server *server);
void leme_input_pointer_grab_cancel_tiled_on(struct leme_server *server,
                                             const struct leme_tags *tags,
                                             uint16_t tag_id);
void leme_input_pointer_grab_cancel_view(struct leme_view *view);
void leme_input_pointer_grab_finish(struct leme_server *server);
void leme_input_workspace_gesture_reset(struct leme_server *server);
void leme_input_workspace_gesture_cancel(struct leme_server *server);

#endif
