#include "shell/control.h"

#include "config/config.h"
#include "config/live.h"
#include "config/prepare.h"
#include "control/action.h"
#include "control/error.h"
#include "core/command.h"
#include "core/server.h"
#include "input/control.h"
#include "input/input.h"
#include "shell/layer.h"
#include "output/control.h"
#include "output/output.h"
#include "protocols/session.h"
#include "public/value.h"
#include "shell/ownership.h"
#include "shell/container.h"
#include "shell/policy.h"
#include "shell/scratchpad.h"
#include "shell/sticky.h"
#include "shell/view.h"
#include "workspace/control.h"
#include "workspace/layout.h"
#include "workspace/public.h"
#include "workspace/tag.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/util/log.h>

#define COMMAND_ARGS_MAX 16u

static bool text_is(struct leme_public_text text, const char *name) {
  return text.length == strlen(name) &&
         memcmp(text.data, name, text.length) == 0;
}

static enum leme_control_code flag_arg(const struct leme_control_call *call,
                                       bool *out,
                                       struct leme_control_error *error,
                                       const char *message) {
  if (call->count == 0 ||
      leme_public_as_bool(call->args[0], out) != LEME_PUBLIC_OK)
    return leme_control_refuse(error, LEME_CONTROL_INVALID_ARGUMENT, message);
  return LEME_CONTROL_OK;
}

static enum leme_control_code focus(struct leme_server *server,
                                    struct leme_view *view, bool *applied,
                                    struct leme_control_error *error) {
  if (leme_view_is_scratchpad(view) && !leme_view_is_shown_scratchpad(view))
    return leme_control_refuse(error, LEME_CONTROL_ACTION_FAILED,
                               "cannot focus hidden scratchpad");
  if (!leme_ownership_focus_eligible(view) && !leme_view_is_minimized(view))
    return leme_control_refuse(error, LEME_CONTROL_ACTION_FAILED,
                               "view not eligible for focus");
  if (server->focused_view == view)
    return LEME_CONTROL_OK;
  if (leme_view_is_minimized(view) &&
      (leme_session_locked(server) ||
       leme_layer_keyboard_is_exclusive(server) ||
       leme_input_interacting(server) || !leme_view_set_minimized(view, false)))
    return leme_control_fail(error, LEME_CONTROL_ACTION_FAILED,
                             "view cannot be restored");
  struct leme_output *output = leme_view_output(view);
  if (output != NULL && leme_output_focused(server) != output) {
    bool warp =
        server->config == NULL || server->config->output_policy.warp_cursor;
    leme_output_set_focused(server, output, warp);
  }
  struct leme_tag *tag = leme_ownership_tag(view);
  if (tag != NULL && tag->owner != NULL &&
      (tag->owner->focused_is_candidate || tag->owner->focused_id != tag->id)) {
    leme_tags_focus_id(tag->owner, tag->id);
  }
  leme_view_focus(view);
  if (server->focused_view != view)
    return leme_control_fail(error, LEME_CONTROL_ACTION_FAILED,
                             "view did not take focus");
  *applied = true;
  return LEME_CONTROL_OK;
}

static enum leme_control_code resize(struct leme_view *view,
                                     const struct leme_control_call *call,
                                     bool *applied,
                                     struct leme_control_error *error) {
  if (view->fullscreen || leme_view_is_placed(view) ||
      (view->container != NULL && !leme_container_action_allowed(view)))
    return leme_control_refuse(
        error, LEME_CONTROL_ACTION_FAILED,
        "cannot resize fullscreen, maximized, snapped or unavailable view");
  struct leme_public_text direction = {0};
  double amount = 0.0;
  if (call->count < 2 ||
      leme_public_as_text(call->args[0], &direction) != LEME_PUBLIC_OK ||
      leme_public_as_number(call->args[1], &amount) != LEME_PUBLIC_OK)
    return leme_control_refuse(error, LEME_CONTROL_INVALID_ARGUMENT,
                               "missing resize arguments");
  enum leme_resize_edge edge = LEME_RESIZE_LEFT;
  if (text_is(direction, "left"))
    edge = LEME_RESIZE_LEFT;
  else if (text_is(direction, "right"))
    edge = LEME_RESIZE_RIGHT;
  else if (text_is(direction, "up"))
    edge = LEME_RESIZE_UP;
  else if (text_is(direction, "down"))
    edge = LEME_RESIZE_DOWN;
  else
    return leme_control_refuse(error, LEME_CONTROL_INVALID_ARGUMENT,
                               "invalid resize direction");
  if (amount == 0.0 || amount < -INT_MAX || amount > INT_MAX)
    return leme_control_refuse(error, LEME_CONTROL_INVALID_ARGUMENT,
                               "resize amount must be a non-zero pixel count");
  if (!leme_view_resize(view, edge, (int)amount))
    return leme_control_fail(error, LEME_CONTROL_ACTION_FAILED,
                             "resize failed");
  *applied = true;
  return LEME_CONTROL_OK;
}

static enum leme_control_code container(struct leme_view *view,
                                        const struct leme_control_call *call,
                                        bool *applied,
                                        struct leme_control_error *error) {
  if (!leme_container_action_allowed(view))
    return leme_control_refuse(error, LEME_CONTROL_ACTION_FAILED,
                               "container action unavailable");
  bool done = false;
  if (call->opcode == LEME_CONTROL_OP_CONTAINER_LAYOUT) {
    struct leme_public_text name = {0};
    enum leme_layout_kind layout = LEME_LAYOUT_DWINDLE;
    bool valid = call->count > 0 &&
                 leme_public_as_text(call->args[0], &name) == LEME_PUBLIC_OK;
    for (enum leme_layout_kind kind = LEME_LAYOUT_DWINDLE;
         valid && kind <= LEME_LAYOUT_MASTER_STACK; kind++) {
      if (text_is(name, leme_layout_public_name(kind))) {
        layout = kind;
        break;
      }
      if (kind == LEME_LAYOUT_MASTER_STACK)
        valid = false;
    }
    if (!valid)
      return leme_control_refuse(error, LEME_CONTROL_INVALID_ARGUMENT,
                                 "invalid container layout");
    if (view->container->layout.kind == layout)
      return LEME_CONTROL_OK;
    done = leme_container_set_layout(view->container, layout);
  } else if (call->opcode == LEME_CONTROL_OP_CONTAINER_DETACH) {
    done = leme_container_detach_current(view);
  } else {
    done = leme_container_dissolve(view->container);
  }
  if (!done)
    return leme_control_fail(error, LEME_CONTROL_ACTION_FAILED,
                             "container action failed");
  *applied = true;
  return LEME_CONTROL_OK;
}

static enum leme_control_code join(struct leme_view *view,
                                   const struct leme_control_call *call,
                                   bool *applied,
                                   struct leme_control_error *error) {
  static const char *const names[] = {"left", "right", "up", "down"};
  static const enum leme_direction directions[] = {
      LEME_DIRECTION_LEFT, LEME_DIRECTION_RIGHT, LEME_DIRECTION_UP,
      LEME_DIRECTION_DOWN};
  if (!leme_container_member_eligible(view) || view->container != NULL)
    return leme_control_refuse(error, LEME_CONTROL_ACTION_FAILED,
                               "container join unavailable");
  struct leme_public_text name = {0};
  if (call->count > 0 &&
      leme_public_as_text(call->args[0], &name) == LEME_PUBLIC_OK) {
    for (size_t i = 0; i < LEME_ARRAY_LENGTH(names); i++) {
      if (!text_is(name, names[i]))
        continue;
      if (!leme_container_join(view, directions[i]))
        return leme_control_fail(error, LEME_CONTROL_ACTION_FAILED,
                                 "container join failed");
      *applied = true;
      return LEME_CONTROL_OK;
    }
  }
  return leme_control_refuse(error, LEME_CONTROL_INVALID_ARGUMENT,
                             "invalid container join direction");
}

static enum leme_control_code move_to_tag(struct leme_server *server,
                                          struct leme_view *view,
                                          const struct leme_control_call *call,
                                          bool *applied,
                                          struct leme_control_error *error) {
  if (leme_view_is_scratchpad(view))
    return leme_control_refuse(error, LEME_CONTROL_ACTION_FAILED,
                               "scratchpad cannot be moved to tag");
  if (view->fullscreen)
    return leme_control_refuse(error, LEME_CONTROL_ACTION_FAILED,
                               "cannot move fullscreen view");
  struct leme_output *dest_output =
      leme_output_by_public_id(server, call->destination->id);
  if (dest_output == NULL)
    return leme_control_refuse(error, LEME_CONTROL_NOT_FOUND,
                               "destination output not found");
  const uint16_t slot = call->destination->tag_number;
  if (slot == 0 || slot > dest_output->tags.max_tags)
    return leme_control_refuse(error, LEME_CONTROL_INVALID_ARGUMENT,
                               "invalid destination tag");
  struct leme_tag *cur_tag = leme_ownership_tag(view);
  struct leme_output *cur_output = leme_view_output(view);
  if (cur_tag != NULL && cur_tag->id == slot && cur_output == dest_output)
    return LEME_CONTROL_OK;
  if (dest_output->tags.table[slot] == NULL) {
    struct leme_tag_materialize *plan = NULL;
    if (!leme_tags_prepare_materialize(&dest_output->tags, slot, &plan))
      return leme_control_refuse(error, LEME_CONTROL_OUT_OF_MEMORY,
                                 "out of memory");
    leme_tags_commit_materialize(&plan);
  }
  const bool reanchor =
      cur_output != NULL && cur_output != dest_output &&
      leme_view_geometry_owner(view) == LEME_VIEW_GEOMETRY_FLOATING &&
      !view->fullscreen && !leme_view_is_sticky(view);
  const bool placed = leme_view_is_placed(view);
  const struct leme_box previous_box =
      placed ? view->placement.restore_box : view->box;
  struct leme_box translated = previous_box;
  if (reanchor) {
    const int64_t x = (int64_t)translated.x - cur_output->full_box.x +
                      dest_output->full_box.x;
    const int64_t y = (int64_t)translated.y - cur_output->full_box.y +
                      dest_output->full_box.y;
    translated.x = x < INT_MIN ? INT_MIN : (x > INT_MAX ? INT_MAX : (int)x);
    translated.y = y < INT_MIN ? INT_MIN : (y > INT_MAX ? INT_MAX : (int)y);
    if (placed) {
      view->placement.restore_box = translated;
    }
  }
  bool ok = leme_view_is_sticky(view)
                ? leme_sticky_move_to_tag(view, slot, false)
                : leme_tags_adopt_view(&dest_output->tags, view, slot);
  if (ok && reanchor && !placed &&
      leme_view_geometry_owner(view) == LEME_VIEW_GEOMETRY_FLOATING) {
    const struct leme_box box =
        leme_view_policy_clamp_box(translated, dest_output->usable_box);
    ok = leme_view_apply_interactive_box(view, box,
                                         box.width != previous_box.width ||
                                             box.height != previous_box.height);
  } else if (!ok && placed) {
    view->placement.restore_box = previous_box;
  }
  if (!ok)
    return leme_control_fail(error, LEME_CONTROL_ACTION_FAILED,
                             "move to tag failed");
  leme_view_refresh_begin(server);
  leme_view_arrange(server);
  if (cur_output != NULL && cur_output != dest_output) {
    leme_tags_refresh_visibility(leme_output_tags(cur_output));
  }
  leme_tags_refresh_visibility(leme_output_tags(dest_output));
  leme_view_refresh_end(server);
  *applied = true;
  return LEME_CONTROL_OK;
}

static enum leme_control_code
move_to_output(struct leme_server *server, struct leme_view *view,
               const struct leme_control_call *call, bool *applied,
               struct leme_control_error *error) {
  if (leme_view_is_scratchpad(view))
    return leme_control_refuse(error, LEME_CONTROL_ACTION_FAILED,
                               "scratchpad cannot be moved to output");
  if (view->fullscreen)
    return leme_control_refuse(error, LEME_CONTROL_ACTION_FAILED,
                               "cannot move fullscreen view");
  struct leme_output *dest_output =
      leme_output_by_public_id(server, call->destination->id);
  if (dest_output == NULL)
    return leme_control_refuse(error, LEME_CONTROL_NOT_FOUND,
                               "destination output not found");
  if (leme_view_output(view) == dest_output)
    return LEME_CONTROL_OK;
  const bool ok = leme_view_is_sticky(view)
                      ? leme_sticky_move_to_output(view, dest_output, false)
                      : leme_view_move_to_output(view, dest_output, false);
  if (!ok)
    return leme_control_fail(error, LEME_CONTROL_ACTION_FAILED,
                             "move to output failed");
  *applied = true;
  return LEME_CONTROL_OK;
}

enum leme_control_code
leme_shell_control_run(struct leme_server *server,
                       const struct leme_control_call *call, bool *applied,
                       struct leme_control_error *error) {
  *applied = false;
  struct leme_view *view = leme_view_by_public_id(server, call->target->id);
  if (view == NULL || !view->mapped || view->unmanaged)
    return leme_control_refuse(error, LEME_CONTROL_NOT_FOUND,
                               "view not found or unmapped");
  bool flag = false;
  enum leme_control_code code = LEME_CONTROL_OK;
  switch (call->opcode) {
  case LEME_CONTROL_OP_FOCUS:
    return focus(server, view, applied, error);
  case LEME_CONTROL_OP_RESIZE:
    return resize(view, call, applied, error);
  case LEME_CONTROL_OP_CONTAINER_LAYOUT:
  case LEME_CONTROL_OP_CONTAINER_DETACH:
  case LEME_CONTROL_OP_CONTAINER_DISSOLVE:
    return container(view, call, applied, error);
  case LEME_CONTROL_OP_CONTAINER_JOIN:
    return join(view, call, applied, error);
  case LEME_CONTROL_OP_MOVE_TO_TAG:
    return move_to_tag(server, view, call, applied, error);
  case LEME_CONTROL_OP_MOVE_TO_OUTPUT:
    return move_to_output(server, view, call, applied, error);
  case LEME_CONTROL_OP_CLOSE:
    leme_view_protocol_close(view);
    *applied = true;
    return LEME_CONTROL_OK;
  case LEME_CONTROL_OP_SET_MINIMIZED:
    code = flag_arg(call, &flag, error, "minimized state must be boolean");
    if (code != LEME_CONTROL_OK)
      return code;
    if (flag && !leme_view_can_minimize(view))
      return leme_control_refuse(error, LEME_CONTROL_ACTION_FAILED,
                                 "view incompatible with minimization");
    if (leme_view_is_minimized(view) == flag)
      return LEME_CONTROL_OK;
    if (!leme_view_set_minimized(view, flag) ||
        leme_view_is_minimized(view) != flag)
      return leme_control_fail(error, LEME_CONTROL_ACTION_FAILED,
                               "minimize failed");
    break;
  case LEME_CONTROL_OP_SET_FLOATING:
    code = flag_arg(call, &flag, error, "floating state must be boolean");
    if (code != LEME_CONTROL_OK)
      return code;
    if (leme_view_is_scratchpad(view) || leme_view_is_sticky(view) ||
        leme_view_geometry_owner(view) == LEME_VIEW_GEOMETRY_CONTAINER)
      return leme_control_refuse(error, LEME_CONTROL_ACTION_FAILED,
                                 "view incompatible with floating");
    if (view->floating == flag)
      return LEME_CONTROL_OK;
    leme_view_set_floating(view, flag);
    break;
  case LEME_CONTROL_OP_SET_FULLSCREEN:
    code = flag_arg(call, &flag, error, "fullscreen state must be boolean");
    if (code != LEME_CONTROL_OK)
      return code;
    if (leme_view_is_scratchpad(view))
      return leme_control_refuse(error, LEME_CONTROL_ACTION_FAILED,
                                 "scratchpad incompatible with fullscreen");
    if (view->fullscreen == flag)
      return LEME_CONTROL_OK;
    leme_view_set_fullscreen(view, flag);
    break;
  case LEME_CONTROL_OP_SET_STICKY:
    code = flag_arg(call, &flag, error, "sticky state must be boolean");
    if (code != LEME_CONTROL_OK)
      return code;
    if (leme_view_is_scratchpad(view) || view->fullscreen)
      return leme_control_refuse(error, LEME_CONTROL_ACTION_FAILED,
                                 "view incompatible with sticky");
    if (leme_view_is_sticky(view) == flag)
      return LEME_CONTROL_OK;
    if (!leme_sticky_toggle(view))
      return leme_control_fail(error, LEME_CONTROL_ACTION_FAILED,
                               "sticky toggle failed");
    break;
  default:
    return leme_control_refuse(error, LEME_CONTROL_UNSUPPORTED,
                               "unsupported view action");
  }
  *applied = true;
  return LEME_CONTROL_OK;
}

static enum leme_control_code reload(struct leme_server *server, bool *applied,
                                     struct leme_control_error *error) {
  const char *path = leme_config_path();
  if (path == NULL)
    return leme_control_refuse(error, LEME_CONTROL_NOT_FOUND,
                               "cannot resolve configuration path");
  char *load_error = NULL;
  struct leme_config *next = leme_config_load(path, &load_error);
  if (next == NULL) {
    const enum leme_control_code code =
        load_error != NULL && strstr(load_error, "not found") != NULL
            ? LEME_CONTROL_NOT_FOUND
            : LEME_CONTROL_INVALID_ARGUMENT;
    (void)leme_control_refuse(
        error, code,
        load_error != NULL ? load_error : "failed to load configuration");
    free(load_error);
    return code;
  }
  free(load_error);
  struct leme_config_reload *plan = NULL;
  const enum leme_control_code code =
      leme_config_reload_prepare(server, next, &plan, error);
  if (code != LEME_CONTROL_OK) {
    leme_config_destroy(next);
    return code;
  }
  leme_config_reload_commit(server, plan);
  leme_config_reload_discard(&plan);
  wlr_log(WLR_INFO, "leme: reloaded %s",
          server->config != NULL && server->config->path != NULL
              ? server->config->path
              : "the configuration");
  leme_server_report_diagnostics(server);
  *applied = true;
  return LEME_CONTROL_OK;
}

static enum leme_control_code command(struct leme_server *server,
                                      const struct leme_control_call *call,
                                      bool *applied,
                                      struct leme_control_error *error) {
  struct leme_public_text name = {0};
  if (call->count != 2 ||
      leme_public_as_text(call->args[0], &name) != LEME_PUBLIC_OK)
    return leme_control_refuse(error, LEME_CONTROL_TYPE_ERROR,
                               "command name must be string");
  if (name.length == 0)
    return leme_control_refuse(error, LEME_CONTROL_INVALID_ARGUMENT,
                               "empty command name");
  const struct leme_public_value *argv = call->args[1];
  if (leme_public_kind(argv) != LEME_PUBLIC_ARRAY)
    return leme_control_refuse(error, LEME_CONTROL_TYPE_ERROR,
                               "command argv must be array");
  const size_t argc = leme_public_length(argv);
  if (argc >= COMMAND_ARGS_MAX)
    return leme_control_refuse(error, LEME_CONTROL_INVALID_ARGUMENT,
                               "too many command arguments");
  char *words[COMMAND_ARGS_MAX] = {0};
  size_t count = 0;
  enum leme_control_code code = LEME_CONTROL_OK;
  for (size_t i = 0; i <= argc; ++i) {
    struct leme_public_text word = name;
    if (i > 0 && leme_public_as_text(leme_public_at(argv, i - 1), &word) !=
                     LEME_PUBLIC_OK) {
      code = leme_control_refuse(error, LEME_CONTROL_TYPE_ERROR,
                                 "argv element must be string");
      goto cleanup;
    }
    if (word.length > 0 && memchr(word.data, '\0', word.length) != NULL) {
      code = leme_control_refuse(error, LEME_CONTROL_INVALID_ARGUMENT,
                                 "command contains embedded NUL");
      goto cleanup;
    }
    words[count] = strndup(word.length > 0 ? word.data : "", word.length);
    if (words[count] == NULL) {
      code = leme_control_refuse(error, LEME_CONTROL_OUT_OF_MEMORY,
                                 "out of memory");
      goto cleanup;
    }
    ++count;
  }
  struct leme_command cmd = {0};
  char *parse_error = NULL;
  if (!leme_command_parse(&cmd, words, count, &parse_error)) {
    code = leme_control_refuse(error, LEME_CONTROL_INVALID_ARGUMENT,
                               parse_error != NULL ? parse_error
                                                   : "invalid command");
    free(parse_error);
    goto cleanup;
  }
  free(parse_error);
  if (cmd.type == LEME_COMMAND_SPAWN || cmd.type == LEME_COMMAND_QUIT ||
      cmd.type == LEME_COMMAND_SWITCH_VT)
    code =
        leme_control_refuse(error, LEME_CONTROL_UNSUPPORTED, "command refused");
  else if (server->config != NULL &&
           !leme_config_validate_command(server->config, &cmd))
    code = leme_control_refuse(error, LEME_CONTROL_INVALID_ARGUMENT,
                               "command not valid for this configuration");
  else if (cmd.type == LEME_COMMAND_RELOAD_CONFIG)
    code = reload(server, applied, error);
  else
    *applied = leme_command_execute(server, &cmd);
  leme_command_finish(&cmd);
cleanup:
  for (size_t i = 0; i < count; ++i)
    free(words[i]);
  return code;
}

static enum leme_control_code
server_control_run(void *context, const struct leme_control_call *call,
                   bool *applied, struct leme_control_error *error) {
  struct leme_server *server = context;
  *applied = false;
  switch (call->opcode) {
  case LEME_CONTROL_OP_FOCUS:
  case LEME_CONTROL_OP_SET_FLOATING:
  case LEME_CONTROL_OP_SET_FULLSCREEN:
  case LEME_CONTROL_OP_RESIZE:
  case LEME_CONTROL_OP_CLOSE:
  case LEME_CONTROL_OP_MOVE_TO_TAG:
  case LEME_CONTROL_OP_MOVE_TO_OUTPUT:
  case LEME_CONTROL_OP_SET_STICKY:
  case LEME_CONTROL_OP_SET_MINIMIZED:
  case LEME_CONTROL_OP_CONTAINER_DETACH:
  case LEME_CONTROL_OP_CONTAINER_DISSOLVE:
  case LEME_CONTROL_OP_CONTAINER_LAYOUT:
  case LEME_CONTROL_OP_CONTAINER_JOIN:
    return leme_shell_control_run(server, call, applied, error);
  case LEME_CONTROL_OP_FOCUS_TAG:
  case LEME_CONTROL_OP_FOCUS_OUTPUT:
  case LEME_CONTROL_OP_SET_LAYOUT:
    return leme_workspace_control_run(server, call, applied, error);
  case LEME_CONTROL_OP_CONFIGURE_OUTPUT:
  case LEME_CONTROL_OP_SET_OUTPUT_POWER:
    return leme_output_control_run(server, call, applied, error);
  case LEME_CONTROL_OP_SET_INPUT:
  case LEME_CONTROL_OP_SET_MODE:
  case LEME_CONTROL_OP_SET_KEYBOARD_LAYOUT:
    return leme_input_control_run(server, call, applied, error);
  case LEME_CONTROL_OP_SET_CONFIG:
    return leme_config_live_run(server, call, applied, error);
  case LEME_CONTROL_OP_COMMAND:
    return command(server, call, applied, error);
  case LEME_CONTROL_OP_RELOAD_CONFIG:
    return reload(server, applied, error);
  default:
    return leme_control_refuse(error, LEME_CONTROL_UNSUPPORTED,
                               "unsupported action opcode");
  }
}

struct leme_control_domain
leme_server_control_domain(struct leme_server *server) {
  return (struct leme_control_domain){.context = server,
                                      .run = server_control_run};
}
