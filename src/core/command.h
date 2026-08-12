#ifndef LEME_COMMAND_H
#define LEME_COMMAND_H

#include "workspace/layout.h"

#include <stdint.h>

enum leme_command_type {
    LEME_COMMAND_FOCUS_NEXT_TAG,
    LEME_COMMAND_FOCUS_PREVIOUS_TAG,
    LEME_COMMAND_FOCUS_TAG,
    LEME_COMMAND_FOCUS_DIRECTION,
    LEME_COMMAND_FOCUS_LAST_TAG,
    LEME_COMMAND_FOCUS_PREVIOUS_VIEW,
    LEME_COMMAND_MOVE_DIRECTION,
    LEME_COMMAND_MOVE_VIEW_TO_TAG,
    LEME_COMMAND_FOCUS_OUTPUT,
    LEME_COMMAND_MOVE_VIEW_TO_OUTPUT,
    LEME_COMMAND_SET_LAYOUT,
    LEME_COMMAND_SWITCH_LAYOUT,
    LEME_COMMAND_REMOVE_EMPTY_TAG,
    LEME_COMMAND_TOGGLE_FLOATING,
    LEME_COMMAND_TOGGLE_FULLSCREEN,
    LEME_COMMAND_RESIZE,
    LEME_COMMAND_CLOSE_VIEW,
    LEME_COMMAND_SPAWN,
    LEME_COMMAND_RELOAD_CONFIG,
    LEME_COMMAND_SET_MODE,
    LEME_COMMAND_CYCLE_KEYBOARD_LAYOUT,
    LEME_COMMAND_SWITCH_VT,
    LEME_COMMAND_QUIT,
    LEME_COMMAND_SCRATCHPAD_SEND,
    LEME_COMMAND_SCRATCHPAD_TOGGLE,
    LEME_COMMAND_SCRATCHPAD_RETRIEVE,
};

struct leme_command {
    enum leme_command_type type;
    uint16_t tag_id;
    uint16_t vt;
    enum leme_direction direction;
    enum leme_resize_edge edge;
    enum leme_layout_kind layout;
    int amount;
    bool follow;
    bool has_direction;
    char *text;
    char **argv;
};

struct leme_server;
bool leme_command_parse(struct leme_command *command,
    char *const *params, size_t params_len, char **error);
bool leme_command_execute(struct leme_server *server,
    const struct leme_command *command);

#endif
