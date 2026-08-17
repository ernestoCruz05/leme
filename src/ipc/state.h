#ifndef LEME_IPC_STATE_H
#define LEME_IPC_STATE_H

#include "workspace/layout.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct leme_json;
struct leme_server;

enum leme_ipc_event {
    LEME_IPC_EVENT_LAYOUT = 1u << 0,
    LEME_IPC_EVENT_KEYBOARD_LAYOUT = 1u << 1,
    LEME_IPC_EVENT_MODE = 1u << 2,
    LEME_IPC_EVENT_FOCUSED_OUTPUT = 1u << 3,
    LEME_IPC_EVENT_VIEW = 1u << 4,
    LEME_IPC_EVENT_CONFIG = 1u << 5,
    LEME_IPC_EVENT_ALL = 0x3fu,
};

bool leme_ipc_event_from_name(const char *name, uint32_t *mask);
const char *leme_ipc_layout_name(enum leme_layout_kind kind);

struct leme_ipc_workspace {
    char *id;
    enum leme_layout_kind layout;
};

struct leme_ipc_diagnostic {
    char *message;
    int line;
};

struct leme_ipc_state {
    char *config_path;
    struct leme_ipc_diagnostic *config_diagnostics;
    size_t config_diagnostic_count;
    bool config_truncated;
    char *focused_output;
    char *mode;
    char *keyboard_active;
    char **keyboard_available;
    size_t keyboard_count;
    struct leme_ipc_workspace *workspaces;
    size_t workspace_count;
    bool has_focused_view;
    bool focused_floating;
    bool focused_scratchpad;
    bool focused_sticky;
    char *focused_view_output;
};

bool leme_ipc_state_capture(struct leme_ipc_state *state,
    struct leme_server *server);
void leme_ipc_state_finish(struct leme_ipc_state *state);
bool leme_ipc_state_write(const struct leme_ipc_state *state,
    struct leme_json *json, char **path, size_t path_length, char **error);

#endif
