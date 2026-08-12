#include "ipc/state.h"

#include "config/config.h"
#include "core/server.h"
#include "ipc/json.h"
#include "ipc/request.h"
#include "output/output.h"
#include "shell/view.h"
#include "workspace/tag.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_output.h>

#define LEME_IPC_STATE_MAX_IDS 64
#define LEME_IPC_STATE_MAX_FIELDS 8

const char *
leme_ipc_layout_name(enum leme_layout_kind kind)
{
    switch (kind) {
    case LEME_LAYOUT_MASTER_STACK:
        return "master_stack";
    case LEME_LAYOUT_ACCORDION:
        return "accordion";
    case LEME_LAYOUT_DWINDLE:
    default:
        return "dwindle";
    }
}

bool
leme_ipc_event_from_name(const char *name, uint32_t *mask)
{
    if (strcmp(name, "layout") == 0) {
        *mask = LEME_IPC_EVENT_LAYOUT;
    } else if (strcmp(name, "keyboard_layout") == 0) {
        *mask = LEME_IPC_EVENT_KEYBOARD_LAYOUT;
    } else if (strcmp(name, "mode") == 0) {
        *mask = LEME_IPC_EVENT_MODE;
    } else if (strcmp(name, "focused_output") == 0) {
        *mask = LEME_IPC_EVENT_FOCUSED_OUTPUT;
    } else if (strcmp(name, "view") == 0) {
        *mask = LEME_IPC_EVENT_VIEW;
    } else if (strcmp(name, "config") == 0) {
        *mask = LEME_IPC_EVENT_CONFIG;
    } else {
        return false;
    }
    return true;
}

static char *
leme_ipc_layout_label(const struct leme_keyboard_layout *layout)
{
    char buffer[128];
    int written;

    if (layout->variant == NULL || layout->variant[0] == '\0') {
        return strdup(layout->name);
    }
    written = snprintf(buffer, sizeof(buffer), "%s(%s)", layout->name,
        layout->variant);
    if (written < 0 || (size_t)written >= sizeof(buffer)) {
        return NULL;
    }
    return strdup(buffer);
}

static bool
leme_ipc_capture_keyboard(struct leme_ipc_state *state,
    const struct leme_server *server)
{
    const struct leme_config *config = server->config;
    size_t index;

    if (config == NULL || config->keyboard_layout_count == 0) {
        return true;
    }
    state->keyboard_available = calloc(config->keyboard_layout_count,
        sizeof(*state->keyboard_available));
    if (state->keyboard_available == NULL) {
        return false;
    }
    for (index = 0; index < config->keyboard_layout_count; index++) {
        state->keyboard_available[index] =
            leme_ipc_layout_label(&config->keyboard_layouts[index]);
        if (state->keyboard_available[index] == NULL) {
            return false;
        }
        state->keyboard_count++;
    }
    if (server->keyboard_layout < state->keyboard_count) {
        state->keyboard_active =
            strdup(state->keyboard_available[server->keyboard_layout]);
        if (state->keyboard_active == NULL) {
            return false;
        }
    }
    return true;
}

static bool
leme_ipc_capture_workspaces(struct leme_ipc_state *state,
    struct leme_server *server)
{
    struct leme_output *output;

    wl_list_for_each(output, &server->outputs, link) {
        struct leme_tags *tags;
        uint16_t ids[LEME_IPC_STATE_MAX_IDS] = {0};
        size_t count;
        size_t index;

        if (output->wlr_output == NULL || !output->wlr_output->enabled) {
            continue;
        }
        tags = leme_output_tags(output);
        if (tags == NULL) {
            continue;
        }
        count = leme_tags_navigable(tags, ids, LEME_IPC_STATE_MAX_IDS);
        if (count > LEME_IPC_STATE_MAX_IDS) {
            count = LEME_IPC_STATE_MAX_IDS;
        }
        for (index = 0; index < count; index++) {
            struct leme_ipc_workspace *entries;
            struct leme_ipc_workspace *entry;
            char id[128];
            int written;

            written = snprintf(id, sizeof(id), "%s:%u",
                output->wlr_output->name, ids[index]);
            if (written < 0 || (size_t)written >= sizeof(id)) {
                return false;
            }
            entries = realloc(state->workspaces,
                (state->workspace_count + 1) * sizeof(*entries));
            if (entries == NULL) {
                return false;
            }
            state->workspaces = entries;
            entry = &state->workspaces[state->workspace_count];
            entry->id = strdup(id);
            if (entry->id == NULL) {
                return false;
            }
            entry->layout = tags->table[ids[index]] == NULL ?
                LEME_LAYOUT_DWINDLE : tags->table[ids[index]]->layout.kind;
            state->workspace_count++;
        }
    }
    return true;
}

static bool
leme_ipc_capture_config(struct leme_ipc_state *state,
    const struct leme_server *server)
{
    const struct leme_config *config = server->config;
    size_t index;

    if (config == NULL) {
        return true;
    }
    if (config->path != NULL) {
        state->config_path = strdup(config->path);
        if (state->config_path == NULL) {
            return false;
        }
    }
    state->config_truncated = config->diagnostics.truncated;
    if (config->diagnostics.count == 0) {
        return true;
    }
    state->config_diagnostics = calloc(config->diagnostics.count,
        sizeof(*state->config_diagnostics));
    if (state->config_diagnostics == NULL) {
        return false;
    }
    for (index = 0; index < config->diagnostics.count; index++) {
        const struct leme_diagnostic *entry =
            &config->diagnostics.entries[index];

        state->config_diagnostics[index].message = strdup(entry->message);
        if (state->config_diagnostics[index].message == NULL) {
            return false;
        }
        state->config_diagnostics[index].line = entry->line;
        state->config_diagnostic_count++;
    }
    return true;
}

bool
leme_ipc_state_capture(struct leme_ipc_state *state,
    struct leme_server *server)
{
    struct leme_output *focused = leme_output_focused(server);

    *state = (struct leme_ipc_state){0};
    if (focused != NULL && focused->wlr_output != NULL) {
        state->focused_output = strdup(focused->wlr_output->name);
        if (state->focused_output == NULL) {
            goto error;
        }
    }
    state->mode = strdup(server->active_mode == NULL ?
        "common" : server->active_mode->name);
    if (state->mode == NULL) {
        goto error;
    }
    if (!leme_ipc_capture_keyboard(state, server)) {
        goto error;
    }
    if (!leme_ipc_capture_workspaces(state, server)) {
        goto error;
    }
    if (!leme_ipc_capture_config(state, server)) {
        goto error;
    }
    if (server->focused_view != NULL) {
        state->has_focused_view = true;
        state->focused_floating = server->focused_view->floating;
        state->focused_scratchpad =
            leme_view_is_scratchpad(server->focused_view);
    }
    return true;

error:
    leme_ipc_state_finish(state);
    return false;
}

void
leme_ipc_state_finish(struct leme_ipc_state *state)
{
    size_t index;

    free(state->config_path);
    for (index = 0; index < state->config_diagnostic_count; index++) {
        free(state->config_diagnostics[index].message);
    }
    free(state->config_diagnostics);
    free(state->focused_output);
    free(state->mode);
    free(state->keyboard_active);
    for (index = 0; index < state->keyboard_count; index++) {
        free(state->keyboard_available[index]);
    }
    free(state->keyboard_available);
    for (index = 0; index < state->workspace_count; index++) {
        free(state->workspaces[index].id);
    }
    free(state->workspaces);
    *state = (struct leme_ipc_state){0};
}

static const struct leme_ipc_workspace *
leme_ipc_find_workspace(const struct leme_ipc_state *state, const char *id)
{
    size_t index;

    for (index = 0; index < state->workspace_count; index++) {
        if (strcmp(state->workspaces[index].id, id) == 0) {
            return &state->workspaces[index];
        }
    }
    return NULL;
}

static void
leme_ipc_write_keyboard(const struct leme_ipc_state *state,
    struct leme_json *json)
{
    size_t index;

    leme_json_object_begin(json);
    leme_json_key(json, "active");
    leme_json_string(json, state->keyboard_active);
    leme_json_key(json, "available");
    leme_json_array_begin(json);
    for (index = 0; index < state->keyboard_count; index++) {
        leme_json_string(json, state->keyboard_available[index]);
    }
    leme_json_array_end(json);
    leme_json_object_end(json);
}

static void
leme_ipc_write_workspace(const struct leme_ipc_workspace *workspace,
    struct leme_json *json)
{
    leme_json_object_begin(json);
    leme_json_key(json, "id");
    leme_json_string(json, workspace->id);
    leme_json_key(json, "layout");
    leme_json_string(json, leme_ipc_layout_name(workspace->layout));
    leme_json_object_end(json);
}

static void
leme_ipc_write_focused_view(const struct leme_ipc_state *state,
    struct leme_json *json)
{
    if (!state->has_focused_view) {
        leme_json_null(json);
        return;
    }
    leme_json_object_begin(json);
    leme_json_key(json, "floating");
    leme_json_bool(json, state->focused_floating);
    leme_json_key(json, "scratchpad");
    leme_json_bool(json, state->focused_scratchpad);
    leme_json_object_end(json);
}

static void
leme_ipc_write_config(const struct leme_ipc_state *state,
    struct leme_json *json)
{
    size_t index;

    leme_json_object_begin(json);
    leme_json_key(json, "path");
    if (state->config_path == NULL) {
        leme_json_null(json);
    } else {
        leme_json_string(json, state->config_path);
    }
    leme_json_key(json, "truncated");
    leme_json_bool(json, state->config_truncated);
    leme_json_key(json, "diagnostics");
    leme_json_array_begin(json);
    for (index = 0; index < state->config_diagnostic_count; index++) {
        leme_json_object_begin(json);
        leme_json_key(json, "line");
        leme_json_number(json, state->config_diagnostics[index].line);
        leme_json_key(json, "message");
        leme_json_string(json, state->config_diagnostics[index].message);
        leme_json_object_end(json);
    }
    leme_json_array_end(json);
    leme_json_object_end(json);
}

static void
leme_ipc_write_tree(const struct leme_ipc_state *state,
    struct leme_json *json)
{
    size_t index;

    leme_json_object_begin(json);
    leme_json_key(json, "focused_output");
    leme_json_string(json, state->focused_output);
    leme_json_key(json, "mode");
    leme_json_string(json, state->mode);
    leme_json_key(json, "keyboard_layout");
    leme_ipc_write_keyboard(state, json);
    leme_json_key(json, "workspaces");
    leme_json_array_begin(json);
    for (index = 0; index < state->workspace_count; index++) {
        leme_ipc_write_workspace(&state->workspaces[index], json);
    }
    leme_json_array_end(json);
    leme_json_key(json, "focused_view");
    leme_ipc_write_focused_view(state, json);
    leme_json_key(json, "config");
    leme_ipc_write_config(state, json);
    leme_json_object_end(json);
}

static void
leme_ipc_set_error(char **error, const char *field)
{
    char buffer[192];
    int written;

    if (error == NULL || *error != NULL) {
        return;
    }
    written = snprintf(buffer, sizeof(buffer), "no such path: %s", field);
    if (written < 0) {
        return;
    }
    *error = strdup(buffer);
}

static bool
leme_ipc_write_top_field(const struct leme_ipc_state *state,
    struct leme_json *json, const char *field, char **error)
{
    if (strcmp(field, "focused_output") == 0) {
        leme_json_string(json, state->focused_output);
    } else if (strcmp(field, "mode") == 0) {
        leme_json_string(json, state->mode);
    } else if (strcmp(field, "keyboard_layout") == 0) {
        leme_ipc_write_keyboard(state, json);
    } else if (strcmp(field, "focused_view") == 0) {
        leme_ipc_write_focused_view(state, json);
    } else if (strcmp(field, "workspaces") == 0) {
        size_t index;

        leme_json_array_begin(json);
        for (index = 0; index < state->workspace_count; index++) {
            leme_ipc_write_workspace(&state->workspaces[index], json);
        }
        leme_json_array_end(json);
    } else {
        leme_ipc_set_error(error, field);
        return false;
    }
    return true;
}

static bool
leme_ipc_write_keyboard_field(const struct leme_ipc_state *state,
    struct leme_json *json, const char *field, char **error)
{
    if (strcmp(field, "active") == 0) {
        leme_json_string(json, state->keyboard_active);
    } else if (strcmp(field, "available") == 0) {
        size_t index;

        leme_json_array_begin(json);
        for (index = 0; index < state->keyboard_count; index++) {
            leme_json_string(json, state->keyboard_available[index]);
        }
        leme_json_array_end(json);
    } else {
        leme_ipc_set_error(error, field);
        return false;
    }
    return true;
}

static bool
leme_ipc_write_workspace_field(const struct leme_ipc_workspace *workspace,
    struct leme_json *json, const char *field, char **error)
{
    if (strcmp(field, "id") == 0) {
        leme_json_string(json, workspace->id);
    } else if (strcmp(field, "layout") == 0) {
        leme_json_string(json, leme_ipc_layout_name(workspace->layout));
    } else {
        leme_ipc_set_error(error, field);
        return false;
    }
    return true;
}

static bool
leme_ipc_write_view_field(const struct leme_ipc_state *state,
    struct leme_json *json, const char *field, char **error)
{
    if (!state->has_focused_view) {
        if (strcmp(field, "floating") != 0 &&
                strcmp(field, "scratchpad") != 0) {
            leme_ipc_set_error(error, field);
            return false;
        }
        leme_json_null(json);
        return true;
    }
    if (strcmp(field, "floating") == 0) {
        leme_json_bool(json, state->focused_floating);
    } else if (strcmp(field, "scratchpad") == 0) {
        leme_json_bool(json, state->focused_scratchpad);
    } else {
        leme_ipc_set_error(error, field);
        return false;
    }
    return true;
}

enum leme_ipc_scope {
    LEME_IPC_SCOPE_TOP,
    LEME_IPC_SCOPE_KEYBOARD,
    LEME_IPC_SCOPE_WORKSPACE,
    LEME_IPC_SCOPE_VIEW,
};

struct leme_ipc_cursor {
    const struct leme_ipc_state *state;
    const struct leme_ipc_workspace *workspace;
    enum leme_ipc_scope scope;
};

static bool
leme_ipc_write_field(struct leme_json *json,
    const struct leme_ipc_cursor *cursor, const char *field, char **error)
{
    switch (cursor->scope) {
    case LEME_IPC_SCOPE_KEYBOARD:
        return leme_ipc_write_keyboard_field(cursor->state, json, field,
            error);
    case LEME_IPC_SCOPE_WORKSPACE:
        return leme_ipc_write_workspace_field(cursor->workspace, json, field,
            error);
    case LEME_IPC_SCOPE_VIEW:
        return leme_ipc_write_view_field(cursor->state, json, field, error);
    case LEME_IPC_SCOPE_TOP:
    default:
        return leme_ipc_write_top_field(cursor->state, json, field, error);
    }
}

static bool
leme_ipc_write_projection(struct leme_json *json, char *token,
    const struct leme_ipc_cursor *cursor, char **error)
{
    char *fields[LEME_IPC_STATE_MAX_FIELDS];
    size_t count = leme_request_fields(token, fields,
        LEME_IPC_STATE_MAX_FIELDS);
    size_t index;

    if (count == 0) {
        leme_ipc_set_error(error, token);
        return false;
    }
    if (count == 1) {
        return leme_ipc_write_field(json, cursor, fields[0], error);
    }
    leme_json_object_begin(json);
    for (index = 0; index < count; index++) {
        leme_json_key(json, fields[index]);
        if (!leme_ipc_write_field(json, cursor, fields[index], error)) {
            return false;
        }
    }
    leme_json_object_end(json);
    return true;
}

bool
leme_ipc_state_write(const struct leme_ipc_state *state,
    struct leme_json *json, char **path, size_t path_length, char **error)
{
    struct leme_ipc_cursor cursor = {
        .state = state,
        .workspace = NULL,
        .scope = LEME_IPC_SCOPE_TOP,
    };

    if (path_length == 0) {
        leme_ipc_write_tree(state, json);
        return true;
    }
    if (path_length == 1 && strcmp(path[0], "config") == 0) {
        leme_ipc_write_config(state, json);
        return true;
    }
    if (path_length == 1) {
        return leme_ipc_write_projection(json, path[0], &cursor, error);
    }
    if (strcmp(path[0], "keyboard_layout") == 0 && path_length == 2) {
        cursor.scope = LEME_IPC_SCOPE_KEYBOARD;
        return leme_ipc_write_projection(json, path[1], &cursor, error);
    }
    if (strcmp(path[0], "focused_view") == 0 && path_length == 2) {
        cursor.scope = LEME_IPC_SCOPE_VIEW;
        return leme_ipc_write_projection(json, path[1], &cursor, error);
    }
    if (strcmp(path[0], "workspaces") == 0 &&
            (path_length == 2 || path_length == 3)) {
        cursor.workspace = leme_ipc_find_workspace(state, path[1]);
        if (cursor.workspace == NULL) {
            leme_ipc_set_error(error, path[1]);
            return false;
        }
        if (path_length == 2) {
            leme_ipc_write_workspace(cursor.workspace, json);
            return true;
        }
        cursor.scope = LEME_IPC_SCOPE_WORKSPACE;
        return leme_ipc_write_projection(json, path[2], &cursor, error);
    }
    leme_ipc_set_error(error, path[0]);
    return false;
}
