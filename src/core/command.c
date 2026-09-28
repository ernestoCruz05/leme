#include "core/command.h"
#include "core/process.h"

#include "config/config.h"
#include "config/prepare.h"
#include "input/input.h"
#include "core/server.h"
#include "output/output.h"
#include "workspace/tag.h"
#include "shell/scratchpad.h"
#include "shell/sticky.h"
#include "shell/view.h"
#include "shell/xwayland.h"
#include "protocols/input.h"
#include "protocols/session.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/backend/session.h>
#include <wlr/util/log.h>

#include "config/internal.h"

static bool leme_command_require_view(struct leme_server *server,
                                      const char *name) {
  if (server->focused_view != NULL) {
    return true;
  }
  wlr_log(WLR_ERROR, "leme: %s requires a focused view", name);
  return false;
}

bool leme_command_warp_cursor(const struct leme_server *server) {
  return server->config == NULL || server->config->output_policy.warp_cursor;
}

static struct leme_output *
leme_command_resolve_output(struct leme_server *server,
                            const struct leme_command *command) {
  if (command->has_direction) {
    return leme_output_adjacent(server, leme_output_focused(server),
                                command->direction);
  }
  return leme_output_by_name(server, command->text);
}

static void leme_command_refresh_tag(struct leme_server *server) {
  leme_view_refresh_tag_focus(server);
}

static bool leme_command_reload(struct leme_server *server) {
  const char *path = leme_config_path();
  struct leme_config *next;
  char *error = NULL;

  if (path == NULL) {
    wlr_log(WLR_ERROR, "%s", "leme: cannot resolve configuration path");
    return false;
  }
  next = leme_config_load(path, &error);
  if (next == NULL) {
    wlr_log(WLR_ERROR, "leme: reload failed: %s",
            error == NULL ? "unknown error" : error);
    free(error);
    return false;
  }
  free(error);

  struct leme_config_reload *plan = NULL;
  struct leme_control_error err = {0};
  enum leme_control_code code =
      leme_config_reload_prepare(server, next, NULL, &plan, &err);
  if (code != LEME_CONTROL_OK) {
    wlr_log(WLR_ERROR, "leme: reload failed: %s", err.message);
    leme_config_destroy(next);
    return false;
  }
  leme_config_reload_commit(server, plan);
  leme_config_reload_discard(&plan);
  wlr_log(WLR_INFO, "leme: reloaded %s", path);
  if (server->config != NULL && server->config->diagnostics.count > 0) {
    size_t index;

    wlr_log(WLR_ERROR, "leme: %zu configuration problems in %s",
            server->config->diagnostics.count, path);
    for (index = 0; index < server->config->diagnostics.count; index++) {
      const struct leme_diagnostic *entry =
          &server->config->diagnostics.entries[index];

      wlr_log(WLR_ERROR, "leme: %s:%d: %s", path, entry->line, entry->message);
    }
    if (server->config->diagnostics.truncated) {
      wlr_log(WLR_ERROR, "%s",
              "leme: further configuration problems were not recorded");
    }
  }
  return true;
}

static bool leme_command_uses_tags(enum leme_command_type type) {
  switch (type) {
  case LEME_COMMAND_FOCUS_NEXT_TAG:
  case LEME_COMMAND_FOCUS_PREVIOUS_TAG:
  case LEME_COMMAND_FOCUS_TAG:
  case LEME_COMMAND_FOCUS_LAST_TAG:
  case LEME_COMMAND_MOVE_VIEW_TO_TAG:
  case LEME_COMMAND_SET_LAYOUT:
  case LEME_COMMAND_SWITCH_LAYOUT:
  case LEME_COMMAND_REMOVE_EMPTY_TAG:
    return true;
  default:
    return false;
  }
}

bool leme_command_execute(struct leme_server *server,
                          const struct leme_command *command) {
  struct leme_tags *tags;
  struct leme_tag *tag;
  struct leme_view *view;

  if (server == NULL || command == NULL) {
    return false;
  }
  if ((command->type == LEME_COMMAND_SCRATCHPAD_SEND ||
       command->type == LEME_COMMAND_SCRATCHPAD_TOGGLE ||
       command->type == LEME_COMMAND_SCRATCHPAD_RETRIEVE ||
       command->type == LEME_COMMAND_TOGGLE_STICKY) &&
      leme_session_locked(server)) {
    wlr_log(WLR_ERROR, "%s", "leme: scratchpad command refused while locked");
    return false;
  }
  tags = leme_focused_tags(server);
  if (tags == NULL && leme_command_uses_tags(command->type)) {
    wlr_log(WLR_ERROR, "%s",
            "leme: tag command refused without an enabled output");
    return false;
  }
  view = server->focused_view;
  if (leme_sticky_is_dependent(view)) {
    switch (command->type) {
    case LEME_COMMAND_MOVE_VIEW_TO_TAG:
    case LEME_COMMAND_MOVE_VIEW_TO_OUTPUT:
    case LEME_COMMAND_TOGGLE_FLOATING:
    case LEME_COMMAND_TOGGLE_STICKY:
    case LEME_COMMAND_TOGGLE_FULLSCREEN:
      view = leme_sticky_group_root(view);
      break;
    default:
      break;
    }
  }
  switch (command->type) {
  case LEME_COMMAND_FOCUS_NEXT_TAG:
  case LEME_COMMAND_FOCUS_PREVIOUS_TAG: {
    const enum leme_tag_change_direction direction =
        command->type == LEME_COMMAND_FOCUS_NEXT_TAG ? LEME_TAG_CHANGE_FORWARD
                                                     : LEME_TAG_CHANGE_BACKWARD;

    leme_input_pointer_grab_cancel_tiled(server);
    if (!command->occupied) {
      leme_tags_step(tags, direction);
    } else if (leme_tags_step_occupied(tags, direction) == NULL) {
      wlr_log(WLR_DEBUG, "%s", "leme: no other occupied tag");
    }
    leme_command_refresh_tag(server);
    return true;
  }
  case LEME_COMMAND_FOCUS_TAG:
    leme_input_pointer_grab_cancel_tiled(server);
    tag = leme_tags_focus_id(tags, command->tag_id);
    if (tag == NULL &&
        !(tags->focused_id == command->tag_id && tags->focused_is_candidate)) {
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
    if (!leme_view_move_direction(server, command->direction,
                                  command->amount)) {
      wlr_log(WLR_ERROR, "%s", "leme: cannot move focused view");
      return false;
    }
    return true;
  case LEME_COMMAND_MOVE_VIEW_TO_TAG: {
    const enum leme_tag_change_direction tag_direction =
        command->has_direction && command->direction == LEME_DIRECTION_LEFT
            ? LEME_TAG_CHANGE_BACKWARD
            : LEME_TAG_CHANGE_FORWARD;
    uint16_t target = command->tag_id;

    if (!leme_command_require_view(server, "move_view_to_tag")) {
      return false;
    }
    if (command->has_direction) {
      target = leme_tags_adjacent_id(tags, tag_direction);
      if (target == 0) {
        wlr_log(WLR_ERROR, "%s", "leme: no adjacent tag to move the view to");
        return false;
      }
    }
    if (leme_view_is_scratchpad(view)) {
      wlr_log(WLR_ERROR, "%s",
              "leme: cannot move a scratchpad member to a tag");
      return false;
    }
    leme_input_pointer_grab_cancel_tiled(server);
    if (leme_view_is_sticky(view)) {
      if (!leme_sticky_move_to_tag(view, target, command->follow)) {
        wlr_log(WLR_ERROR, "leme: cannot move sticky group to tag %u", target);
        return false;
      }
      return true;
    }
    if (!leme_tags_move_view(tags, view, target)) {
      wlr_log(WLR_ERROR, "leme: cannot move view to tag %u", target);
      return false;
    }
    if (command->follow) {
      struct leme_tag *followed =
          command->has_direction
              ? leme_tags_focus_id_direction(tags, target, tag_direction)
              : leme_tags_focus_id(tags, target);

      if (followed == NULL) {
        wlr_log(WLR_ERROR, "leme: cannot follow view to tag %u", target);
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
    struct leme_output *target = leme_command_resolve_output(server, command);

    if (target == NULL || target == leme_output_focused(server)) {
      wlr_log(WLR_DEBUG, "%s", "leme: no output in that direction");
      return true;
    }
    leme_input_pointer_grab_cancel_tiled(server);
    leme_output_set_focused(server, target, leme_command_warp_cursor(server));
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
    if (leme_view_is_scratchpad(view)) {
      wlr_log(WLR_ERROR, "%s",
              "leme: cannot move a scratchpad member to an output");
      return false;
    }
    leme_input_pointer_grab_cancel_tiled(server);
    if (leme_view_is_sticky(view)
            ? !leme_sticky_move_to_output(view, target, command->follow)
            : !leme_view_move_to_output(view, target, command->follow)) {
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
      wlr_log(WLR_ERROR, "leme: tag %u is invalid, occupied, or the last tag",
              command->tag_id);
      return false;
    }
    leme_command_refresh_tag(server);
    return true;
  case LEME_COMMAND_TOGGLE_FLOATING:
    if (!leme_command_require_view(server, "toggle_floating")) {
      return false;
    }
    return leme_view_set_floating(view, !view->floating);
  case LEME_COMMAND_TOGGLE_STICKY:
    if (!leme_command_require_view(server, "toggle_sticky")) {
      return false;
    }
    return leme_sticky_toggle(view);
  case LEME_COMMAND_TOGGLE_FULLSCREEN:
    if (!leme_command_require_view(server, "toggle_fullscreen")) {
      return false;
    }
    return leme_view_set_fullscreen(view, !view->fullscreen);
  case LEME_COMMAND_RESIZE:
    if (!leme_command_require_view(server, "resize")) {
      return false;
    }
    if (!leme_view_resize(server->focused_view, command->edge,
                          command->amount)) {
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
  case LEME_COMMAND_TOGGLE_SHORTCUTS_INHIBIT:
    return leme_input_protocols_toggle_shortcuts_inhibit(server);
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
    return leme_scratchpad_toggle_unnamed(server, leme_output_focused(server));
  case LEME_COMMAND_SCRATCHPAD_RETRIEVE:
    return leme_scratchpad_retrieve(server, tags);
  }
  wlr_log(WLR_ERROR, "%s", "leme: invalid command");
  return false;
}
