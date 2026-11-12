#ifndef LEME_DESK_H
#define LEME_DESK_H

#include "desktop/fileops.h"
#include "workspace/layout.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <xkbcommon/xkbcommon.h>

struct leme_server;
struct leme_output;
struct leme_render_hit;
struct wlr_pointer_button_event;
struct wlr_scene_tree;

enum leme_desk_grid_action {
  LEME_DESK_GRID_OPEN,
  LEME_DESK_GRID_TRASH,
  LEME_DESK_GRID_RENAME,
  LEME_DESK_GRID_NEW_FOLDER,
  LEME_DESK_GRID_SELECT_ALL,
  LEME_DESK_GRID_CLEAR,
};

bool leme_desk_init(struct leme_server *server);
void leme_desk_finish(struct leme_server *server);
void leme_desk_sync(struct leme_server *server);
bool leme_desk_refresh(struct leme_server *server);
void leme_desk_output_finish(struct leme_output *output);
bool leme_desk_output_active(const struct leme_output *output);
struct wlr_scene_tree *leme_desk_snapshot(struct leme_output *output,
                                          struct wlr_scene_tree *parent);
bool leme_desk_focused(const struct leme_server *server);
size_t leme_desk_context_entries(const struct leme_server *server);
void leme_desk_drop_feedback(struct leme_server *server, char *const *paths,
                             size_t count);
bool leme_desk_drop(struct leme_server *server, char *const *paths,
                    size_t count);
struct leme_command;
bool leme_desk_context_command(struct leme_server *server,
                               const struct leme_command *command);
void leme_desk_unfocus(struct leme_server *server);
bool leme_desk_focus(struct leme_server *server);
bool leme_desk_open_menu(struct leme_server *server);
bool leme_desk_grid_act(struct leme_server *server,
                        enum leme_desk_grid_action action);
bool leme_desk_grid_select(struct leme_server *server,
                           enum leme_direction direction, bool extend);
bool leme_desk_handle_button(struct leme_server *server,
                             const struct leme_render_hit *hit,
                             const struct wlr_pointer_button_event *event);
bool leme_desk_handle_key(struct leme_server *server, xkb_keysym_t keysym,
                          uint32_t modifiers);

#endif
