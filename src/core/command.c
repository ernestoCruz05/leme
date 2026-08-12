#include "core/command.h"
#include "core/process.h"

#include "config/config.h"
#include "input/input.h"
#include "core/server.h"
#include "output/output.h"
#include "workspace/tag.h"
#include "shell/scratchpad.h"
#include "shell/view.h"
#include "shell/xwayland.h"
#include "protocols/session.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/backend/session.h>
#include <wlr/util/log.h>

#include "config/internal.h"

#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

static void
leme_command_set_error(char **error, const char *format, ...)
{
    va_list arguments;
    char buffer[256];
    int written;

    if (error == NULL || *error != NULL) {
        return;
    }
    va_start(arguments, format);
    written = vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    if (written < 0) {
        return;
    }
    *error = strdup(buffer);
}

static bool
leme_command_parse_u16(const char *text, uint16_t *value)
{
    char *end;
    unsigned long parsed;

    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || text[0] == '\0' || *end != '\0' ||
            parsed == 0 || parsed > UINT16_MAX) {
        return false;
    }
    *value = (uint16_t)parsed;
    return true;
}

static bool
leme_command_parse_nonnegative(const char *text, int *value)
{
    char *end;
    long parsed;

    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno != 0 || text[0] == '\0' || *end != '\0' ||
            parsed < 0 || parsed > INT_MAX) {
        return false;
    }
    *value = (int)parsed;
    return true;
}

static bool
leme_command_parse_layout_kind(const char *text, enum leme_layout_kind *kind)
{
    if (strcmp(text, "dwindle") == 0) {
        *kind = LEME_LAYOUT_DWINDLE;
    } else if (strcmp(text, "master_stack") == 0) {
        *kind = LEME_LAYOUT_MASTER_STACK;
    } else if (strcmp(text, "accordion") == 0) {
        *kind = LEME_LAYOUT_ACCORDION;
    } else {
        return false;
    }
    return true;
}

static bool
leme_command_parse_direction(const char *text, enum leme_direction *direction)
{
    if (strcmp(text, "left") == 0) {
        *direction = LEME_DIRECTION_LEFT;
    } else if (strcmp(text, "right") == 0) {
        *direction = LEME_DIRECTION_RIGHT;
    } else if (strcmp(text, "up") == 0) {
        *direction = LEME_DIRECTION_UP;
    } else if (strcmp(text, "down") == 0) {
        *direction = LEME_DIRECTION_DOWN;
    } else {
        return false;
    }
    return true;
}

static bool
leme_command_parse_output_target(const char *text,
    struct leme_command *command)
{
    if (leme_command_parse_direction(text, &command->direction)) {
        command->has_direction = true;
        return true;
    }
    if (text[0] == '\0') {
        return false;
    }
    command->has_direction = false;
    command->text = strdup(text);
    return command->text != NULL;
}

bool
leme_command_parse(struct leme_command *command,
    char *const *params, size_t params_len, char **error)
{
    const char *name;
    size_t arguments;

    if (params_len == 0) {
        leme_command_set_error(error, "%s", "missing command");
        return false;
    }
    name = params[0];
    arguments = params_len - 1;
    if (strcmp(name, "focus_next_tag") == 0 && arguments == 0) {
        command->type = LEME_COMMAND_FOCUS_NEXT_TAG;
    } else if (strcmp(name, "focus_previous_tag") == 0 && arguments == 0) {
        command->type = LEME_COMMAND_FOCUS_PREVIOUS_TAG;
    } else if (strcmp(name, "focus_tag") == 0 && arguments == 1) {
        command->type = LEME_COMMAND_FOCUS_TAG;
        if (!leme_command_parse_u16(params[1], &command->tag_id)) {
            goto invalid;
        }
    } else if (strcmp(name, "focus") == 0 && arguments == 1) {
        command->type = LEME_COMMAND_FOCUS_DIRECTION;
        if (!leme_command_parse_direction(params[1], &command->direction)) {
            goto invalid;
        }
    } else if (strcmp(name, "focus_last_tag") == 0 && arguments == 0) {
        command->type = LEME_COMMAND_FOCUS_LAST_TAG;
    } else if (strcmp(name, "focus_previous_view") == 0 && arguments == 0) {
        command->type = LEME_COMMAND_FOCUS_PREVIOUS_VIEW;
    } else if (strcmp(name, "move") == 0 &&
            (arguments == 1 || arguments == 2)) {
        command->type = LEME_COMMAND_MOVE_DIRECTION;
        command->amount = 40;
        if (!leme_command_parse_direction(params[1], &command->direction) ||
                (arguments == 2 &&
                    (!leme_command_parse_nonnegative(
                        params[2], &command->amount) ||
                    command->amount == 0))) {
            goto invalid;
        }
    } else if (strcmp(name, "move_view_to_tag") == 0 &&
            (arguments == 1 || arguments == 2)) {
        command->type = LEME_COMMAND_MOVE_VIEW_TO_TAG;
        if (strcmp(params[1], "next") == 0) {
            command->has_direction = true;
            command->direction = LEME_DIRECTION_RIGHT;
        } else if (strcmp(params[1], "previous") == 0) {
            command->has_direction = true;
            command->direction = LEME_DIRECTION_LEFT;
        } else if (!leme_command_parse_u16(params[1], &command->tag_id)) {
            goto invalid;
        }
        if (arguments == 2 && strcmp(params[2], "follow") != 0) {
            goto invalid;
        }
        command->follow = arguments == 2;
    } else if (strcmp(name, "focus_output") == 0 && arguments == 1) {
        command->type = LEME_COMMAND_FOCUS_OUTPUT;
        if (!leme_command_parse_output_target(params[1], command)) {
            goto invalid;
        }
    } else if (strcmp(name, "move_view_to_output") == 0 &&
            (arguments == 1 || arguments == 2)) {
        command->type = LEME_COMMAND_MOVE_VIEW_TO_OUTPUT;
        if (!leme_command_parse_output_target(params[1], command) ||
                (arguments == 2 && strcmp(params[2], "follow") != 0)) {
            goto invalid;
        }
        command->follow = arguments == 2;
    } else if (strcmp(name, "set_layout") == 0 && arguments == 1) {
        command->type = LEME_COMMAND_SET_LAYOUT;
        if (!leme_command_parse_layout_kind(params[1], &command->layout)) {
            goto invalid;
        }
    } else if (strcmp(name, "switch_layout") == 0 && arguments == 0) {
        command->type = LEME_COMMAND_SWITCH_LAYOUT;
    } else if (strcmp(name, "remove_empty_tag") == 0 && arguments == 1) {
        command->type = LEME_COMMAND_REMOVE_EMPTY_TAG;
        if (!leme_command_parse_u16(params[1], &command->tag_id)) {
            goto invalid;
        }
    } else if (strcmp(name, "toggle_floating") == 0 && arguments == 0) {
        command->type = LEME_COMMAND_TOGGLE_FLOATING;
    } else if (strcmp(name, "toggle_fullscreen") == 0 && arguments == 0) {
        command->type = LEME_COMMAND_TOGGLE_FULLSCREEN;
    } else if (strcmp(name, "resize") == 0 && arguments == 2) {
        command->type = LEME_COMMAND_RESIZE;
        if (strcmp(params[1], "left") == 0) {
            command->edge = LEME_RESIZE_LEFT;
        } else if (strcmp(params[1], "right") == 0) {
            command->edge = LEME_RESIZE_RIGHT;
        } else if (strcmp(params[1], "up") == 0) {
            command->edge = LEME_RESIZE_UP;
        } else if (strcmp(params[1], "down") == 0) {
            command->edge = LEME_RESIZE_DOWN;
        } else {
            goto invalid;
        }
        if (!leme_command_parse_nonnegative(params[2], &command->amount) ||
                command->amount == 0) {
            goto invalid;
        }
    } else if (strcmp(name, "close_view") == 0 && arguments == 0) {
        command->type = LEME_COMMAND_CLOSE_VIEW;
    } else if (strcmp(name, "spawn") == 0 && arguments > 0) {
        command->type = LEME_COMMAND_SPAWN;
        command->argv = leme_config_copy_argv(params[1],
            &params[2], arguments - 1);
        if (command->argv == NULL) {
            goto allocation;
        }
    } else if (strcmp(name, "reload_config") == 0 && arguments == 0) {
        command->type = LEME_COMMAND_RELOAD_CONFIG;
    } else if (strcmp(name, "mode") == 0 && arguments == 1) {
        command->type = LEME_COMMAND_SET_MODE;
        command->text = strdup(params[1]);
        if (command->text == NULL) {
            goto allocation;
        }
    } else if (strcmp(name, "cycle_keyboard_layout") == 0 &&
            arguments == 0) {
        command->type = LEME_COMMAND_CYCLE_KEYBOARD_LAYOUT;
    } else if (strcmp(name, "switch_vt") == 0 && arguments == 1) {
        command->type = LEME_COMMAND_SWITCH_VT;
        if (!leme_command_parse_u16(params[1], &command->vt) ||
                command->vt == 0 || command->vt > 12) {
            goto invalid;
        }
    } else if (strcmp(name, "quit") == 0 && arguments == 0) {
        command->type = LEME_COMMAND_QUIT;
    } else if (strcmp(name, "scratchpad_send") == 0 && arguments == 0) {
        command->type = LEME_COMMAND_SCRATCHPAD_SEND;
    } else if (strcmp(name, "scratchpad_toggle") == 0 &&
            (arguments == 0 || arguments == 1)) {
        command->type = LEME_COMMAND_SCRATCHPAD_TOGGLE;
        if (arguments == 1) {
            if (params[1][0] == '\0') {
                goto invalid;
            }
            command->text = strdup(params[1]);
            if (command->text == NULL) {
                goto allocation;
            }
        }
    } else if (strcmp(name, "scratchpad_retrieve") == 0 && arguments == 0) {
        command->type = LEME_COMMAND_SCRATCHPAD_RETRIEVE;
    } else {
        goto invalid;
    }
    return true;

invalid:
    leme_command_set_error(error, "invalid command %s", name);
    return false;
allocation:
    leme_command_set_error(error, "%s", "out of memory");
    return false;
}

static bool
leme_command_require_view(struct leme_server *server, const char *name)
{
    if (server->focused_view != NULL) {
        return true;
    }
    wlr_log(WLR_ERROR, "leme: %s requires a focused view", name);
    return false;
}

static bool
leme_command_warp_cursor(const struct leme_server *server)
{
    return server->config == NULL ||
        server->config->output_policy.warp_cursor;
}

static struct leme_output *
leme_command_resolve_output(struct leme_server *server,
    const struct leme_command *command)
{
    if (command->has_direction) {
        return leme_output_adjacent(server,
            leme_output_focused(server), command->direction);
    }
    return leme_output_by_name(server, command->text);
}

static void
leme_command_refresh_tag(struct leme_server *server)
{
    leme_view_refresh_tag_focus(server);
}

static bool
leme_command_reload(struct leme_server *server)
{
    const char *path = leme_config_path();
    struct leme_config *next;
    char *error = NULL;

    if (path == NULL) {
        wlr_log(WLR_ERROR, "%s", "leme: cannot resolve configuration path");
        return false;
    }
    next = leme_config_load(path, &error);
    if (next == NULL || !leme_config_apply(server, next, &error)) {
        wlr_log(WLR_ERROR, "leme: reload failed: %s",
            error == NULL ? "unknown error" : error);
        leme_config_destroy(next);
        free(error);
        return false;
    }
    free(error);
    wlr_log(WLR_INFO, "leme: reloaded %s", path);
    if (server->config != NULL && server->config->diagnostics.count > 0) {
        size_t index;

        wlr_log(WLR_ERROR, "leme: %zu configuration problems in %s",
            server->config->diagnostics.count, path);
        for (index = 0; index < server->config->diagnostics.count; index++) {
            const struct leme_diagnostic *entry =
                &server->config->diagnostics.entries[index];

            wlr_log(WLR_ERROR, "leme: %s:%d: %s", path,
                entry->line, entry->message);
        }
        if (server->config->diagnostics.truncated) {
            wlr_log(WLR_ERROR, "%s",
                "leme: further configuration problems were not recorded");
        }
    }
    return true;
}

bool
leme_command_execute(struct leme_server *server,
    const struct leme_command *command)
{
    struct leme_tags *tags;
    struct leme_tag *tag;
    struct leme_view *view;

    if (server == NULL || command == NULL) {
        return false;
    }
    if ((command->type == LEME_COMMAND_SCRATCHPAD_SEND ||
            command->type == LEME_COMMAND_SCRATCHPAD_TOGGLE ||
            command->type == LEME_COMMAND_SCRATCHPAD_RETRIEVE) &&
            leme_session_locked(server)) {
        wlr_log(WLR_ERROR, "%s", "leme: scratchpad command refused while locked");
        return false;
    }
    tags = leme_focused_tags(server);
    switch (command->type) {
    case LEME_COMMAND_FOCUS_NEXT_TAG:
        leme_input_pointer_grab_cancel_tiled(server);
        leme_tags_step(tags, LEME_TAG_CHANGE_FORWARD);
        leme_command_refresh_tag(server);
        return true;
    case LEME_COMMAND_FOCUS_PREVIOUS_TAG:
        leme_input_pointer_grab_cancel_tiled(server);
        leme_tags_step(tags, LEME_TAG_CHANGE_BACKWARD);
        leme_command_refresh_tag(server);
        return true;
    case LEME_COMMAND_FOCUS_TAG:
        leme_input_pointer_grab_cancel_tiled(server);
        tag = leme_tags_focus_id(tags, command->tag_id);
        if (tag == NULL && !(tags->focused_id == command->tag_id &&
                tags->focused_is_candidate)) {
            wlr_log(WLR_ERROR, "leme: invalid tag id %u", command->tag_id);
            return false;
        }
        leme_command_refresh_tag(server);
        return true;
    case LEME_COMMAND_FOCUS_DIRECTION:
        leme_input_pointer_grab_cancel_tiled(server);
        if (!leme_view_focus_direction(server, command->direction)) {
            wlr_log(WLR_DEBUG, "%s", "leme: no directional focus candidate");
        }
        return true;
    case LEME_COMMAND_FOCUS_LAST_TAG:
        leme_input_pointer_grab_cancel_tiled(server);
        if (!leme_tags_focus_last(tags)) {
            wlr_log(WLR_ERROR, "%s", "leme: no previous tag");
            return false;
        }
        leme_command_refresh_tag(server);
        return true;
    case LEME_COMMAND_FOCUS_PREVIOUS_VIEW:
        leme_input_pointer_grab_cancel_tiled(server);
        if (!leme_view_focus_previous(server)) {
            wlr_log(WLR_DEBUG, "%s", "leme: no previous view");
        }
        return true;
    case LEME_COMMAND_MOVE_DIRECTION:
        if (!leme_command_require_view(server, "move")) {
            return false;
        }
        if (!leme_view_move_direction(
                server, command->direction, command->amount)) {
            wlr_log(WLR_ERROR, "%s", "leme: cannot move focused view");
            return false;
        }
        return true;
    case LEME_COMMAND_MOVE_VIEW_TO_TAG: {
        const enum leme_tag_change_direction tag_direction =
            command->has_direction &&
                command->direction == LEME_DIRECTION_LEFT ?
                LEME_TAG_CHANGE_BACKWARD : LEME_TAG_CHANGE_FORWARD;
        uint16_t target = command->tag_id;

        if (!leme_command_require_view(server, "move_view_to_tag")) {
            return false;
        }
        if (command->has_direction) {
            target = leme_tags_adjacent_id(tags, tag_direction);
            if (target == 0) {
                wlr_log(WLR_ERROR, "%s",
                    "leme: no adjacent tag to move the view to");
                return false;
            }
        }
        view = server->focused_view;
        if (leme_view_is_scratchpad(view)) {
            wlr_log(WLR_ERROR, "%s",
                "leme: cannot move a scratchpad member to a tag");
            return false;
        }
        leme_input_pointer_grab_cancel_tiled(server);
        if (!leme_tags_move_view(tags, view, target)) {
            wlr_log(WLR_ERROR, "leme: cannot move view to tag %u", target);
            return false;
        }
        if (command->follow) {
            struct leme_tag *followed = command->has_direction ?
                leme_tags_focus_id_direction(
                    tags, target, tag_direction) :
                leme_tags_focus_id(tags, target);

            if (followed == NULL) {
                wlr_log(WLR_ERROR,
                    "leme: cannot follow view to tag %u", target);
                return false;
            }
        }
        leme_command_refresh_tag(server);
        if (command->follow) {
            leme_view_focus(view);
        }
        return true;
    }
    case LEME_COMMAND_FOCUS_OUTPUT: {
        struct leme_output *target =
            leme_command_resolve_output(server, command);

        if (target == NULL || target == leme_output_focused(server)) {
            wlr_log(WLR_DEBUG, "%s", "leme: no output in that direction");
            return true;
        }
        leme_input_pointer_grab_cancel_tiled(server);
        leme_output_set_focused(server, target,
            leme_command_warp_cursor(server));
        return true;
    }
    case LEME_COMMAND_MOVE_VIEW_TO_OUTPUT: {
        struct leme_output *target;

        if (!leme_command_require_view(server, "move_view_to_output")) {
            return false;
        }
        target = leme_command_resolve_output(server, command);
        if (target == NULL || target == leme_output_focused(server)) {
            wlr_log(WLR_DEBUG, "%s", "leme: no output in that direction");
            return true;
        }
        view = server->focused_view;
        if (leme_view_is_scratchpad(view)) {
            wlr_log(WLR_ERROR, "%s",
                "leme: cannot move a scratchpad member to an output");
            return false;
        }
        leme_input_pointer_grab_cancel_tiled(server);
        if (!leme_view_move_to_output(view, target, command->follow)) {
            wlr_log(WLR_ERROR, "%s",
                "leme: cannot move the focused view to that output");
            return false;
        }
        return true;
    }
    case LEME_COMMAND_SET_LAYOUT:
        leme_input_pointer_grab_cancel_tiled(server);
        if (!leme_tags_set_layout(tags, command->layout)) {
            wlr_log(WLR_ERROR, "%s", "leme: cannot set the layout");
            return false;
        }
        leme_view_arrange(server);
        leme_tags_refresh_visibility(tags);
        return true;
    case LEME_COMMAND_SWITCH_LAYOUT:
        leme_input_pointer_grab_cancel_tiled(server);
        if (!leme_tags_cycle_layout(tags)) {
            wlr_log(WLR_ERROR, "%s", "leme: cannot switch the layout");
            return false;
        }
        leme_view_arrange(server);
        leme_tags_refresh_visibility(tags);
        return true;
    case LEME_COMMAND_REMOVE_EMPTY_TAG:
        leme_input_pointer_grab_cancel_tiled(server);
        if (!leme_tags_remove_empty(tags, command->tag_id)) {
            wlr_log(WLR_ERROR,
                "leme: tag %u is invalid, occupied, or the last tag",
                command->tag_id);
            return false;
        }
        leme_command_refresh_tag(server);
        return true;
    case LEME_COMMAND_TOGGLE_FLOATING:
        if (!leme_command_require_view(server, "toggle_floating")) {
            return false;
        }
        return leme_view_set_floating(server->focused_view,
            !server->focused_view->floating);
    case LEME_COMMAND_TOGGLE_FULLSCREEN:
        if (!leme_command_require_view(server, "toggle_fullscreen")) {
            return false;
        }
        return leme_view_set_fullscreen(server->focused_view,
            !server->focused_view->fullscreen);
    case LEME_COMMAND_RESIZE:
        if (!leme_command_require_view(server, "resize")) {
            return false;
        }
        if (!leme_view_resize(server->focused_view,
                command->edge, command->amount)) {
            wlr_log(WLR_ERROR, "%s", "leme: resize has no matching edge");
            return false;
        }
        return true;
    case LEME_COMMAND_CLOSE_VIEW:
        if (!leme_command_require_view(server, "close_view")) {
            return false;
        }
        leme_view_close(server->focused_view);
        return true;
    case LEME_COMMAND_SPAWN:
        return leme_process_spawn_detached(server, command->argv);
    case LEME_COMMAND_RELOAD_CONFIG:
        return leme_command_reload(server);
    case LEME_COMMAND_SET_MODE:
        if (!leme_input_set_mode(server, command->text)) {
            wlr_log(WLR_ERROR, "leme: unknown mode %s",
                command->text == NULL ? "(null)" : command->text);
            return false;
        }
        return true;
    case LEME_COMMAND_CYCLE_KEYBOARD_LAYOUT:
        return leme_input_cycle_keyboard_layout(server);
    case LEME_COMMAND_SWITCH_VT:
        if (server->session == NULL) {
            wlr_log(WLR_ERROR, "%s", "leme: VT switching is unavailable");
            return false;
        }
        leme_input_pointer_grab_finish(server);
        if (!wlr_session_change_vt(server->session, command->vt)) {
            wlr_log(WLR_ERROR, "leme: failed to switch to VT %u", command->vt);
            return false;
        }
        return true;
    case LEME_COMMAND_QUIT:
        wl_display_terminate(server->display);
        return true;
    case LEME_COMMAND_SCRATCHPAD_SEND:
        if (!leme_command_require_view(server, "scratchpad_send")) {
            return false;
        }
        return leme_scratchpad_send(server, server->focused_view);
    case LEME_COMMAND_SCRATCHPAD_TOGGLE:
        if (command->text != NULL) {
            return leme_scratchpad_toggle_named(server, command->text,
                leme_output_focused(server));
        }
        return leme_scratchpad_toggle_unnamed(server,
            leme_output_focused(server));
    case LEME_COMMAND_SCRATCHPAD_RETRIEVE:
        return leme_scratchpad_retrieve(server, tags);
    }
    wlr_log(WLR_ERROR, "%s", "leme: invalid command");
    return false;
}
