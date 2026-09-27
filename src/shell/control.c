#include "shell/control.h"

#include "config/config.h"
#include "config/live.h"
#include "config/prepare.h"
#include "control/action.h"
#include "control/error.h"
#include "control/memory.h"
#include "core/command.h"
#include "core/server.h"
#include "input/control.h"
#include "output/control.h"
#include "output/output.h"
#include "protocols/session.h"
#include "public/value.h"
#include "shell/ownership.h"
#include "shell/scratchpad.h"
#include "shell/sticky.h"
#include "shell/view.h"
#include "workspace/control.h"
#include "workspace/layout.h"
#include "workspace/tag.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct shell_prepared_item {
  uint64_t serial;
};

struct shell_prepared {
  enum leme_control_opcode opcode;
  struct leme_public_budget *account;
  size_t count;
  struct shell_prepared_item *items;
  bool target_bool;
  enum leme_resize_edge resize_edge;
  int resize_amount;
  uint64_t dest_output_serial;
  uint16_t dest_tag_slot;
  struct leme_tag_materialize *mat_slot;
};

static enum leme_control_code
set_preflight_error(struct leme_control_error *error,
                    enum leme_control_code code, const char *msg) {
  if (error != NULL) {
    error->code = code;
    error->phase = LEME_CONTROL_PREFLIGHT;
    (void)snprintf(error->message, sizeof(error->message), "%.*s",
                   (int)sizeof(error->message) - 1, msg);
    (void)snprintf(error->expr_path, sizeof(error->expr_path), "/expr");
    error->effects_applied = false;
  }
  return code;
}

enum leme_control_code leme_shell_control_prepare(
    struct leme_server *server, const struct leme_control_intent *intents,
    size_t count, struct leme_public_budget *account,
    struct leme_control_prepared **out, struct leme_control_error *error) {
  if (server == NULL || intents == NULL || count == 0 || out == NULL) {
    return LEME_CONTROL_INVALID_ARGUMENT;
  }

  const enum leme_control_opcode opcode = intents[0].opcode;
  if (opcode == LEME_CONTROL_OP_FOCUS || opcode == LEME_CONTROL_OP_RESIZE) {
    if (count != 1) {
      return set_preflight_error(error, LEME_CONTROL_CARDINALITY,
                                 "single target required");
    }
  }

  if (opcode == LEME_CONTROL_OP_MOVE_TO_TAG) {
    if (!intents[0].has_destination ||
        intents[0].destination.kind != LEME_PUBLIC_TAG) {
      return set_preflight_error(error, LEME_CONTROL_INVALID_ARGUMENT,
                                 "missing destination tag");
    }
    struct leme_output *dest_output =
        leme_output_by_public_id(server, intents[0].destination.id);
    if (dest_output == NULL) {
      return set_preflight_error(error, LEME_CONTROL_NOT_FOUND,
                                 "destination output not found");
    }
    const uint16_t dest_slot = intents[0].destination.tag_number;
    if (dest_slot == 0 || dest_slot > dest_output->tags.max_tags) {
      return set_preflight_error(error, LEME_CONTROL_INVALID_ARGUMENT,
                                 "invalid destination tag");
    }
  } else if (opcode == LEME_CONTROL_OP_MOVE_TO_OUTPUT) {
    if (!intents[0].has_destination ||
        intents[0].destination.kind != LEME_PUBLIC_OUTPUT) {
      return set_preflight_error(error, LEME_CONTROL_INVALID_ARGUMENT,
                                 "missing destination output");
    }
    struct leme_output *dest_output =
        leme_output_by_public_id(server, intents[0].destination.id);
    if (dest_output == NULL) {
      return set_preflight_error(error, LEME_CONTROL_NOT_FOUND,
                                 "destination output not found");
    }
  }

  struct shell_prepared *prep =
      leme_control_alloc(account, sizeof(struct shell_prepared));
  if (prep == NULL) {
    return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                               "out of memory");
  }
  memset(prep, 0, sizeof(*prep));
  prep->opcode = opcode;
  prep->account = account;
  prep->count = count;
  prep->items =
      leme_control_alloc(account, count * sizeof(struct shell_prepared_item));
  if (prep->items == NULL) {
    leme_control_free(prep);
    return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                               "out of memory");
  }

  if (opcode == LEME_CONTROL_OP_MOVE_TO_TAG) {
    prep->dest_output_serial = intents[0].destination.id.serial;
    prep->dest_tag_slot = intents[0].destination.tag_number;
    struct leme_output *dest_output =
        leme_output_by_public_id(server, intents[0].destination.id);
    if (dest_output != NULL &&
        dest_output->tags.table[prep->dest_tag_slot] == NULL) {
      if (!leme_tags_prepare_materialize(
              &dest_output->tags, prep->dest_tag_slot, &prep->mat_slot)) {
        leme_shell_control_discard(server,
                                   (struct leme_control_prepared *)prep);
        return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                   "out of memory");
      }
    }
  } else if (opcode == LEME_CONTROL_OP_MOVE_TO_OUTPUT) {
    prep->dest_output_serial = intents[0].destination.id.serial;
    prep->dest_tag_slot = intents[0].destination.tag_number;
  } else if (opcode == LEME_CONTROL_OP_SET_STICKY) {
    bool target_bool = false;
    if (intents[0].args != NULL) {
      leme_public_as_bool(intents[0].args, &target_bool);
    }
    prep->target_bool = target_bool;
  }

  for (size_t i = 0; i < count; ++i) {
    prep->items[i].serial = intents[i].target.id.serial;
    struct leme_view *view =
        leme_view_by_public_id(server, intents[i].target.id);
    if (view == NULL || !view->mapped || view->unmanaged) {
      leme_shell_control_discard(server, (struct leme_control_prepared *)prep);
      return set_preflight_error(error, LEME_CONTROL_NOT_FOUND,
                                 "view not found or unmapped");
    }

    if (opcode == LEME_CONTROL_OP_FOCUS) {
      if (leme_view_is_scratchpad(view) &&
          !leme_view_is_shown_scratchpad(view)) {
        leme_shell_control_discard(server,
                                   (struct leme_control_prepared *)prep);
        return set_preflight_error(error, LEME_CONTROL_ACTION_FAILED,
                                   "cannot focus hidden scratchpad");
      }
      if (!leme_ownership_focus_eligible(view)) {
        leme_shell_control_discard(server,
                                   (struct leme_control_prepared *)prep);
        return set_preflight_error(error, LEME_CONTROL_ACTION_FAILED,
                                   "view not eligible for focus");
      }
    } else if (opcode == LEME_CONTROL_OP_SET_FLOATING) {
      if (leme_view_is_scratchpad(view) || leme_view_is_sticky(view)) {
        leme_shell_control_discard(server,
                                   (struct leme_control_prepared *)prep);
        return set_preflight_error(error, LEME_CONTROL_ACTION_FAILED,
                                   "view incompatible with floating");
      }
    } else if (opcode == LEME_CONTROL_OP_SET_FULLSCREEN) {
      if (leme_view_is_scratchpad(view)) {
        leme_shell_control_discard(server,
                                   (struct leme_control_prepared *)prep);
        return set_preflight_error(error, LEME_CONTROL_ACTION_FAILED,
                                   "scratchpad incompatible with fullscreen");
      }
    } else if (opcode == LEME_CONTROL_OP_RESIZE) {
      if (view->fullscreen) {
        leme_shell_control_discard(server,
                                   (struct leme_control_prepared *)prep);
        return set_preflight_error(error, LEME_CONTROL_ACTION_FAILED,
                                   "cannot resize fullscreen view");
      }
    } else if (opcode == LEME_CONTROL_OP_MOVE_TO_TAG) {
      if (leme_view_is_scratchpad(view)) {
        leme_shell_control_discard(server,
                                   (struct leme_control_prepared *)prep);
        return set_preflight_error(error, LEME_CONTROL_ACTION_FAILED,
                                   "scratchpad cannot be moved to tag");
      }
      if (view->fullscreen) {
        leme_shell_control_discard(server,
                                   (struct leme_control_prepared *)prep);
        return set_preflight_error(error, LEME_CONTROL_ACTION_FAILED,
                                   "cannot move fullscreen view");
      }
    } else if (opcode == LEME_CONTROL_OP_MOVE_TO_OUTPUT) {
      if (leme_view_is_scratchpad(view)) {
        leme_shell_control_discard(server,
                                   (struct leme_control_prepared *)prep);
        return set_preflight_error(error, LEME_CONTROL_ACTION_FAILED,
                                   "scratchpad cannot be moved to output");
      }
      if (view->fullscreen) {
        leme_shell_control_discard(server,
                                   (struct leme_control_prepared *)prep);
        return set_preflight_error(error, LEME_CONTROL_ACTION_FAILED,
                                   "cannot move fullscreen view");
      }
    } else if (opcode == LEME_CONTROL_OP_SET_STICKY) {
      if (leme_view_is_scratchpad(view) || view->fullscreen) {
        leme_shell_control_discard(server,
                                   (struct leme_control_prepared *)prep);
        return set_preflight_error(error, LEME_CONTROL_ACTION_FAILED,
                                   "view incompatible with sticky");
      }
    }
  }

  if (opcode == LEME_CONTROL_OP_SET_FLOATING ||
      opcode == LEME_CONTROL_OP_SET_FULLSCREEN) {
    bool target_bool = false;
    if (intents[0].args != NULL) {
      leme_public_as_bool(intents[0].args, &target_bool);
    }
    prep->target_bool = target_bool;

    if (opcode == LEME_CONTROL_OP_SET_FULLSCREEN && target_bool) {
      for (size_t i = 0; i < count; ++i) {
        struct leme_view *vi = leme_view_by_public_id(
            server, (struct leme_public_id){prep->items[i].serial});
        struct leme_tag *tag_i = leme_ownership_tag(vi);
        for (size_t j = i + 1; j < count; ++j) {
          struct leme_view *vj = leme_view_by_public_id(
              server, (struct leme_public_id){prep->items[j].serial});
          struct leme_tag *tag_j = leme_ownership_tag(vj);
          if (tag_i != NULL && tag_i == tag_j) {
            leme_shell_control_discard(server,
                                       (struct leme_control_prepared *)prep);
            return set_preflight_error(
                error, LEME_CONTROL_ACTION_FAILED,
                "multiple views on same tag cannot be fullscreen");
          }
        }
        if (tag_i != NULL) {
          struct leme_view *existing = NULL;
          wl_list_for_each(existing, &tag_i->views, tag_link) {
            if (existing->mapped && existing->fullscreen && existing != vi) {
              leme_shell_control_discard(server,
                                         (struct leme_control_prepared *)prep);
              return set_preflight_error(error, LEME_CONTROL_ACTION_FAILED,
                                         "tag already has a fullscreen view");
            }
          }
        }
      }
    }
  } else if (opcode == LEME_CONTROL_OP_RESIZE) {
    const struct leme_public_value *dir_val =
        leme_public_at(intents[0].args, 0);
    const struct leme_public_value *amt_val =
        leme_public_at(intents[0].args, 1);
    if (dir_val == NULL || amt_val == NULL) {
      leme_shell_control_discard(server, (struct leme_control_prepared *)prep);
      return set_preflight_error(error, LEME_CONTROL_INVALID_ARGUMENT,
                                 "missing resize arguments");
    }
    struct leme_public_text dir_text = {0};
    leme_public_as_text(dir_val, &dir_text);
    if (dir_text.length == 4 && memcmp(dir_text.data, "left", 4) == 0) {
      prep->resize_edge = LEME_RESIZE_LEFT;
    } else if (dir_text.length == 5 && memcmp(dir_text.data, "right", 5) == 0) {
      prep->resize_edge = LEME_RESIZE_RIGHT;
    } else if (dir_text.length == 2 && memcmp(dir_text.data, "up", 2) == 0) {
      prep->resize_edge = LEME_RESIZE_UP;
    } else if (dir_text.length == 4 && memcmp(dir_text.data, "down", 4) == 0) {
      prep->resize_edge = LEME_RESIZE_DOWN;
    } else {
      leme_shell_control_discard(server, (struct leme_control_prepared *)prep);
      return set_preflight_error(error, LEME_CONTROL_INVALID_ARGUMENT,
                                 "invalid resize direction");
    }

    double amt_num = 0.0;
    leme_public_as_number(amt_val, &amt_num);
    int amount = (int)amt_num;
    if (amount <= 0) {
      leme_shell_control_discard(server, (struct leme_control_prepared *)prep);
      return set_preflight_error(error, LEME_CONTROL_INVALID_ARGUMENT,
                                 "resize amount must be positive");
    }
    prep->resize_amount = amount;
  }

  *out = (struct leme_control_prepared *)prep;
  return LEME_CONTROL_OK;
}

enum leme_control_code
leme_shell_control_execute_one(struct leme_server *server,
                               struct leme_control_prepared *prepared,
                               size_t index, enum leme_control_outcome *outcome,
                               struct leme_control_error *error) {
  if (server == NULL || prepared == NULL || outcome == NULL) {
    return LEME_CONTROL_INVALID_ARGUMENT;
  }

  struct shell_prepared *prep = (struct shell_prepared *)prepared;
  if (index >= prep->count) {
    *outcome = LEME_CONTROL_FAILED;
    return LEME_CONTROL_INVALID_ARGUMENT;
  }

  struct leme_view *view = leme_view_by_public_id(
      server, (struct leme_public_id){prep->items[index].serial});
  if (view == NULL || !view->mapped || view->unmanaged) {
    *outcome = LEME_CONTROL_FAILED;
    if (error != NULL) {
      error->code = LEME_CONTROL_ACTION_FAILED;
      error->phase = LEME_CONTROL_EXECUTE;
      (void)snprintf(error->message, sizeof(error->message),
                     "view no longer available");
      (void)snprintf(error->expr_path, sizeof(error->expr_path), "/expr");
      error->effects_applied = false;
    }
    return LEME_CONTROL_ACTION_FAILED;
  }

  switch (prep->opcode) {
  case LEME_CONTROL_OP_FOCUS: {
    if (server->focused_view == view) {
      *outcome = LEME_CONTROL_NOOP;
      return LEME_CONTROL_OK;
    }
    struct leme_output *output = leme_view_output(view);
    if (output != NULL && leme_output_focused(server) != output) {
      bool warp =
          server->config == NULL || server->config->output_policy.warp_cursor;
      leme_output_set_focused(server, output, warp);
    }
    struct leme_tag *tag = leme_ownership_tag(view);
    if (tag != NULL && tag->owner != NULL &&
        (tag->owner->focused_is_candidate ||
         tag->owner->focused_id != tag->id)) {
      leme_tags_focus_id(tag->owner, tag->id);
    }
    leme_view_focus(view);
    if (server->focused_view == view) {
      *outcome = LEME_CONTROL_APPLIED;
    } else {
      *outcome = LEME_CONTROL_FAILED;
    }
    return LEME_CONTROL_OK;
  }
  case LEME_CONTROL_OP_SET_FLOATING: {
    if (view->floating == prep->target_bool) {
      *outcome = LEME_CONTROL_NOOP;
      return LEME_CONTROL_OK;
    }
    leme_view_set_floating(view, prep->target_bool);
    *outcome = LEME_CONTROL_APPLIED;
    return LEME_CONTROL_OK;
  }
  case LEME_CONTROL_OP_SET_FULLSCREEN: {
    if (view->fullscreen == prep->target_bool) {
      *outcome = LEME_CONTROL_NOOP;
      return LEME_CONTROL_OK;
    }
    leme_view_set_fullscreen(view, prep->target_bool);
    *outcome = LEME_CONTROL_APPLIED;
    return LEME_CONTROL_OK;
  }
  case LEME_CONTROL_OP_RESIZE: {
    if (view->fullscreen) {
      *outcome = LEME_CONTROL_FAILED;
      return LEME_CONTROL_ACTION_FAILED;
    }
    bool ok = leme_view_resize(view, prep->resize_edge, prep->resize_amount);
    if (ok) {
      *outcome = LEME_CONTROL_APPLIED;
    } else {
      *outcome = LEME_CONTROL_FAILED;
    }
    return LEME_CONTROL_OK;
  }
  case LEME_CONTROL_OP_CLOSE: {
    leme_view_close(view);
    *outcome = LEME_CONTROL_ACCEPTED;
    return LEME_CONTROL_OK;
  }
  case LEME_CONTROL_OP_MOVE_TO_TAG: {
    struct leme_output *dest_output = leme_output_by_public_id(
        server, (struct leme_public_id){prep->dest_output_serial});
    if (dest_output == NULL) {
      *outcome = LEME_CONTROL_FAILED;
      return LEME_CONTROL_ACTION_FAILED;
    }
    if (prep->mat_slot != NULL) {
      leme_tags_commit_materialize(&prep->mat_slot);
    }
    struct leme_tag *cur_tag = leme_ownership_tag(view);
    struct leme_output *cur_output = leme_view_output(view);
    if (cur_tag != NULL && cur_tag->id == prep->dest_tag_slot &&
        cur_output == dest_output) {
      *outcome = LEME_CONTROL_NOOP;
      return LEME_CONTROL_OK;
    }
    bool ok = false;
    if (leme_view_is_sticky(view)) {
      ok = leme_sticky_move_to_tag(view, prep->dest_tag_slot, false);
    } else {
      ok = leme_tags_adopt_view(&dest_output->tags, view, prep->dest_tag_slot);
    }
    if (ok) {
      if (cur_output != NULL && cur_output != dest_output) {
        leme_tags_refresh_visibility(leme_output_tags(cur_output));
      }
      leme_tags_refresh_visibility(leme_output_tags(dest_output));
      leme_view_arrange(server);
      *outcome = LEME_CONTROL_APPLIED;
    } else {
      *outcome = LEME_CONTROL_FAILED;
    }
    return LEME_CONTROL_OK;
  }
  case LEME_CONTROL_OP_MOVE_TO_OUTPUT: {
    struct leme_output *dest_output = leme_output_by_public_id(
        server, (struct leme_public_id){prep->dest_output_serial});
    if (dest_output == NULL) {
      *outcome = LEME_CONTROL_FAILED;
      return LEME_CONTROL_ACTION_FAILED;
    }
    struct leme_output *cur_output = leme_view_output(view);
    if (cur_output == dest_output) {
      *outcome = LEME_CONTROL_NOOP;
      return LEME_CONTROL_OK;
    }
    bool ok = false;
    if (leme_view_is_sticky(view)) {
      ok = leme_sticky_move_to_output(view, dest_output, false);
    } else {
      ok = leme_view_move_to_output(view, dest_output, false);
    }
    if (ok) {
      *outcome = LEME_CONTROL_APPLIED;
    } else {
      *outcome = LEME_CONTROL_FAILED;
    }
    return LEME_CONTROL_OK;
  }
  case LEME_CONTROL_OP_SET_STICKY: {
    if (leme_view_is_sticky(view) == prep->target_bool) {
      *outcome = LEME_CONTROL_NOOP;
      return LEME_CONTROL_OK;
    }
    bool ok = leme_sticky_toggle(view);
    if (ok) {
      *outcome = LEME_CONTROL_APPLIED;
    } else {
      *outcome = LEME_CONTROL_FAILED;
    }
    return LEME_CONTROL_OK;
  }
  default:
    *outcome = LEME_CONTROL_FAILED;
    return LEME_CONTROL_UNSUPPORTED;
  }
}

void leme_shell_control_discard(struct leme_server *server,
                                struct leme_control_prepared *prepared) {
  (void)server;
  if (prepared == NULL) {
    return;
  }
  struct shell_prepared *prep = (struct shell_prepared *)prepared;
  if (prep->mat_slot != NULL) {
    leme_tags_discard_materialize(&prep->mat_slot);
  }
  if (prep->items != NULL) {
    leme_control_free(prep->items);
  }
  leme_control_free(prep);
}

#define COMPOSITE_PREPARED_MAGIC LEME_CONTROL_OPERATOR_COUNT

struct composite_item {
  enum leme_control_opcode opcode;
  struct leme_control_prepared *prepared;
};

struct composite_prepared {
  enum leme_control_opcode magic;
  struct leme_public_budget *account;
  size_t count;
  struct composite_item *items;
};

struct command_prepared {
  enum leme_control_opcode opcode;
  struct leme_public_budget *account;
  struct leme_command cmd;
};

static enum leme_control_code command_control_prepare(
    struct leme_server *server, const struct leme_control_intent *intents,
    size_t count, struct leme_public_budget *account,
    struct leme_control_prepared **out, struct leme_control_error *error) {
  if (server == NULL || intents == NULL || count == 0 || out == NULL) {
    return LEME_CONTROL_INVALID_ARGUMENT;
  }
  if (count != 1) {
    return set_preflight_error(error, LEME_CONTROL_CARDINALITY,
                               "single command required");
  }

  const struct leme_public_value *argv = intents[0].args;
  if (argv == NULL || leme_public_kind(argv) != LEME_PUBLIC_ARRAY) {
    return set_preflight_error(error, LEME_CONTROL_INVALID_ARGUMENT,
                               "missing command args");
  }

  const struct leme_public_value *name_val = leme_public_at(argv, 0);
  if (name_val == NULL || leme_public_kind(name_val) != LEME_PUBLIC_STRING) {
    return set_preflight_error(error, LEME_CONTROL_INVALID_ARGUMENT,
                               "missing command name");
  }
  struct leme_public_text name_t = {0};
  leme_public_as_text(name_val, &name_t);

  struct command_prepared *prep =
      leme_control_alloc(account, sizeof(struct command_prepared));
  if (prep == NULL) {
    return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                               "out of memory");
  }
  memset(prep, 0, sizeof(*prep));
  prep->opcode = LEME_CONTROL_OP_COMMAND;
  prep->account = account;

  if (name_t.length == 5 && memcmp(name_t.data, "focus", 5) == 0) {
    prep->cmd.type = LEME_COMMAND_FOCUS_DIRECTION;
    const struct leme_public_value *d_val = leme_public_at(argv, 1);
    struct leme_public_text d_t = {0};
    if (d_val != NULL) {
      leme_public_as_text(d_val, &d_t);
    }
    if (d_t.length == 4 && memcmp(d_t.data, "left", 4) == 0) {
      prep->cmd.direction = LEME_DIRECTION_LEFT;
    } else if (d_t.length == 5 && memcmp(d_t.data, "right", 5) == 0) {
      prep->cmd.direction = LEME_DIRECTION_RIGHT;
    } else if (d_t.length == 2 && memcmp(d_t.data, "up", 2) == 0) {
      prep->cmd.direction = LEME_DIRECTION_UP;
    } else if (d_t.length == 4 && memcmp(d_t.data, "down", 4) == 0) {
      prep->cmd.direction = LEME_DIRECTION_DOWN;
    }
  } else if (name_t.length == 19 &&
             memcmp(name_t.data, "focus_previous_view", 19) == 0) {
    prep->cmd.type = LEME_COMMAND_FOCUS_PREVIOUS_VIEW;
  } else if (name_t.length == 14 &&
             memcmp(name_t.data, "focus_last_tag", 14) == 0) {
    prep->cmd.type = LEME_COMMAND_FOCUS_LAST_TAG;
  } else if (name_t.length == 12 &&
             memcmp(name_t.data, "focus_output", 12) == 0) {
    prep->cmd.type = LEME_COMMAND_FOCUS_OUTPUT;
    prep->cmd.has_direction = true;
    const struct leme_public_value *d_val = leme_public_at(argv, 1);
    struct leme_public_text d_t = {0};
    if (d_val != NULL) {
      leme_public_as_text(d_val, &d_t);
    }
    if (d_t.length == 4 && memcmp(d_t.data, "left", 4) == 0) {
      prep->cmd.direction = LEME_DIRECTION_LEFT;
    } else if (d_t.length == 5 && memcmp(d_t.data, "right", 5) == 0) {
      prep->cmd.direction = LEME_DIRECTION_RIGHT;
    } else if (d_t.length == 2 && memcmp(d_t.data, "up", 2) == 0) {
      prep->cmd.direction = LEME_DIRECTION_UP;
    } else if (d_t.length == 4 && memcmp(d_t.data, "down", 4) == 0) {
      prep->cmd.direction = LEME_DIRECTION_DOWN;
    }
  } else if (name_t.length == 4 && memcmp(name_t.data, "move", 4) == 0) {
    if (server->focused_view == NULL || !server->focused_view->mapped ||
        server->focused_view->unmanaged) {
      leme_control_free(prep);
      return set_preflight_error(error, LEME_CONTROL_NOT_FOUND,
                                 "no focused view");
    }
    prep->cmd.type = LEME_COMMAND_MOVE_DIRECTION;
    const struct leme_public_value *d_val = leme_public_at(argv, 1);
    struct leme_public_text d_t = {0};
    if (d_val != NULL) {
      leme_public_as_text(d_val, &d_t);
    }
    if (d_t.length == 4 && memcmp(d_t.data, "left", 4) == 0) {
      prep->cmd.direction = LEME_DIRECTION_LEFT;
    } else if (d_t.length == 5 && memcmp(d_t.data, "right", 5) == 0) {
      prep->cmd.direction = LEME_DIRECTION_RIGHT;
    } else if (d_t.length == 2 && memcmp(d_t.data, "up", 2) == 0) {
      prep->cmd.direction = LEME_DIRECTION_UP;
    } else if (d_t.length == 4 && memcmp(d_t.data, "down", 4) == 0) {
      prep->cmd.direction = LEME_DIRECTION_DOWN;
    }
    const struct leme_public_value *a_val = leme_public_at(argv, 2);
    int64_t amt = 0;
    if (a_val != NULL) {
      leme_public_as_integer(a_val, &amt);
    }
    prep->cmd.amount = (int)amt;
  } else if (name_t.length == 19 &&
             memcmp(name_t.data, "move_view_to_output", 19) == 0) {
    if (server->focused_view == NULL || !server->focused_view->mapped ||
        server->focused_view->unmanaged) {
      leme_control_free(prep);
      return set_preflight_error(error, LEME_CONTROL_NOT_FOUND,
                                 "no focused view");
    }
    if (leme_view_is_scratchpad(server->focused_view)) {
      leme_control_free(prep);
      return set_preflight_error(error, LEME_CONTROL_ACTION_FAILED,
                                 "scratchpad cannot be moved to output");
    }
    prep->cmd.type = LEME_COMMAND_MOVE_VIEW_TO_OUTPUT;
    prep->cmd.has_direction = true;
    const struct leme_public_value *d_val = leme_public_at(argv, 1);
    struct leme_public_text d_t = {0};
    if (d_val != NULL) {
      leme_public_as_text(d_val, &d_t);
    }
    if (d_t.length == 4 && memcmp(d_t.data, "left", 4) == 0) {
      prep->cmd.direction = LEME_DIRECTION_LEFT;
    } else if (d_t.length == 5 && memcmp(d_t.data, "right", 5) == 0) {
      prep->cmd.direction = LEME_DIRECTION_RIGHT;
    } else if (d_t.length == 2 && memcmp(d_t.data, "up", 2) == 0) {
      prep->cmd.direction = LEME_DIRECTION_UP;
    } else if (d_t.length == 4 && memcmp(d_t.data, "down", 4) == 0) {
      prep->cmd.direction = LEME_DIRECTION_DOWN;
    }
    const struct leme_public_value *f_val = leme_public_at(argv, 2);
    int64_t f_int = 0;
    if (f_val != NULL) {
      leme_public_as_integer(f_val, &f_int);
    }
    prep->cmd.follow = (f_int != 0);
  } else if (name_t.length == 13 &&
             memcmp(name_t.data, "switch_layout", 13) == 0) {
    prep->cmd.type = LEME_COMMAND_SWITCH_LAYOUT;
  } else if (name_t.length == 16 &&
             memcmp(name_t.data, "remove_empty_tag", 16) == 0) {
    prep->cmd.type = LEME_COMMAND_REMOVE_EMPTY_TAG;
    const struct leme_public_value *t_val = leme_public_at(argv, 1);
    int64_t tid = 0;
    if (t_val != NULL) {
      leme_public_as_integer(t_val, &tid);
    }
    prep->cmd.tag_id = (uint16_t)tid;
  } else if (name_t.length == 21 &&
             memcmp(name_t.data, "cycle_keyboard_layout", 21) == 0) {
    prep->cmd.type = LEME_COMMAND_CYCLE_KEYBOARD_LAYOUT;
  } else if (name_t.length == 24 &&
             memcmp(name_t.data, "toggle_shortcuts_inhibit", 24) == 0) {
    prep->cmd.type = LEME_COMMAND_TOGGLE_SHORTCUTS_INHIBIT;
  } else if (name_t.length == 15 &&
             memcmp(name_t.data, "scratchpad_send", 15) == 0) {
    if (server->focused_view == NULL || !server->focused_view->mapped ||
        server->focused_view->unmanaged) {
      leme_control_free(prep);
      return set_preflight_error(error, LEME_CONTROL_NOT_FOUND,
                                 "no focused view");
    }
    if (leme_view_is_scratchpad(server->focused_view)) {
      leme_control_free(prep);
      return set_preflight_error(error, LEME_CONTROL_ACTION_FAILED,
                                 "already a scratchpad");
    }
    prep->cmd.type = LEME_COMMAND_SCRATCHPAD_SEND;
  } else if (name_t.length == 17 &&
             memcmp(name_t.data, "scratchpad_toggle", 17) == 0) {
    prep->cmd.type = LEME_COMMAND_SCRATCHPAD_TOGGLE;
    const struct leme_public_value *nm_val = leme_public_at(argv, 1);
    struct leme_public_text nm_t = {0};
    if (nm_val != NULL) {
      leme_public_as_text(nm_val, &nm_t);
    }
    if (nm_t.length > 0 && nm_t.data != NULL) {
      char *sp_name = strndup(nm_t.data, nm_t.length);
      if (sp_name == NULL) {
        leme_control_free(prep);
        return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                   "out of memory");
      }
      if (server->config == NULL ||
          leme_config_scratchpad(server->config, sp_name) == NULL) {
        free(sp_name);
        leme_control_free(prep);
        return set_preflight_error(error, LEME_CONTROL_NOT_FOUND,
                                   "scratchpad not configured");
      }
      prep->cmd.text = sp_name;
    }
  } else if (name_t.length == 19 &&
             memcmp(name_t.data, "scratchpad_retrieve", 19) == 0) {
    prep->cmd.type = LEME_COMMAND_SCRATCHPAD_RETRIEVE;
  } else {
    leme_control_free(prep);
    return set_preflight_error(error, LEME_CONTROL_UNSUPPORTED,
                               "unknown command");
  }

  *out = (struct leme_control_prepared *)prep;
  return LEME_CONTROL_OK;
}

static enum leme_control_code
command_control_execute_one(struct leme_server *server,
                            struct leme_control_prepared *prepared,
                            size_t index, enum leme_control_outcome *outcome,
                            struct leme_control_error *error) {
  (void)index;
  (void)error;
  if (server == NULL || prepared == NULL || outcome == NULL) {
    return LEME_CONTROL_INVALID_ARGUMENT;
  }
  struct command_prepared *prep = (struct command_prepared *)prepared;

  struct leme_view *old_view = server->focused_view;
  struct leme_output *old_output = leme_output_focused(server);

  bool ok = leme_command_execute(server, &prep->cmd);
  if (!ok) {
    if (prep->cmd.type == LEME_COMMAND_FOCUS_LAST_TAG ||
        prep->cmd.type == LEME_COMMAND_REMOVE_EMPTY_TAG) {
      *outcome = LEME_CONTROL_NOOP;
      return LEME_CONTROL_OK;
    }
    *outcome = LEME_CONTROL_FAILED;
    return LEME_CONTROL_OK;
  }

  if (prep->cmd.type == LEME_COMMAND_FOCUS_DIRECTION ||
      prep->cmd.type == LEME_COMMAND_FOCUS_PREVIOUS_VIEW) {
    if (server->focused_view != old_view) {
      *outcome = LEME_CONTROL_APPLIED;
    } else {
      *outcome = LEME_CONTROL_NOOP;
    }
  } else if (prep->cmd.type == LEME_COMMAND_FOCUS_OUTPUT &&
             prep->cmd.has_direction) {
    if (leme_output_focused(server) != old_output) {
      *outcome = LEME_CONTROL_APPLIED;
    } else {
      *outcome = LEME_CONTROL_NOOP;
    }
  } else {
    *outcome = LEME_CONTROL_APPLIED;
  }
  return LEME_CONTROL_OK;
}

static void command_control_discard(struct leme_server *server,
                                    struct leme_control_prepared *prepared) {
  (void)server;
  if (prepared == NULL) {
    return;
  }
  struct command_prepared *prep = (struct command_prepared *)prepared;
  if (prep->cmd.text != NULL) {
    free(prep->cmd.text);
  }
  leme_control_free(prep);
}

static void server_control_discard(void *context,
                                   struct leme_control_prepared *prepared);

static enum leme_control_code
server_control_prepare(void *context, const struct leme_control_intent *intents,
                       size_t count, struct leme_public_budget *account,
                       struct leme_control_prepared **out,
                       struct leme_control_error *error) {
  struct leme_server *server = context;
  if (count == 0 || intents == NULL) {
    *out = NULL;
    return LEME_CONTROL_OK;
  }

  if (leme_session_locked(server)) {
    return set_preflight_error(error, LEME_CONTROL_SESSION_LOCKED,
                               "session is locked");
  }

  bool is_composite = false;
  for (size_t i = 1; i < count; ++i) {
    if (intents[i].opcode != intents[0].opcode) {
      is_composite = true;
      break;
    }
  }

  if (is_composite) {
    struct composite_prepared *comp =
        leme_control_alloc(account, sizeof(struct composite_prepared));
    if (comp == NULL) {
      return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                 "out of memory");
    }
    memset(comp, 0, sizeof(*comp));
    comp->magic = COMPOSITE_PREPARED_MAGIC;
    comp->account = account;
    comp->count = count;
    comp->items =
        leme_control_alloc(account, count * sizeof(struct composite_item));
    if (comp->items == NULL) {
      leme_control_free(comp);
      return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                 "out of memory");
    }
    memset(comp->items, 0, count * sizeof(struct composite_item));

    for (size_t i = 0; i < count; ++i) {
      comp->items[i].opcode = intents[i].opcode;
      enum leme_control_code c = server_control_prepare(
          server, &intents[i], 1, account, &comp->items[i].prepared, error);
      if (c != LEME_CONTROL_OK) {
        for (size_t j = 0; j < i; ++j) {
          if (comp->items[j].prepared != NULL) {
            server_control_discard(server, comp->items[j].prepared);
          }
        }
        leme_control_free(comp->items);
        leme_control_free(comp);
        return c;
      }
    }
    *out = (struct leme_control_prepared *)comp;
    return LEME_CONTROL_OK;
  }

  switch (intents[0].opcode) {
  case LEME_CONTROL_OP_FOCUS:
  case LEME_CONTROL_OP_SET_FLOATING:
  case LEME_CONTROL_OP_SET_FULLSCREEN:
  case LEME_CONTROL_OP_RESIZE:
  case LEME_CONTROL_OP_CLOSE:
  case LEME_CONTROL_OP_MOVE_TO_TAG:
  case LEME_CONTROL_OP_MOVE_TO_OUTPUT:
  case LEME_CONTROL_OP_SET_STICKY:
    return leme_shell_control_prepare(server, intents, count, account, out,
                                      error);
  case LEME_CONTROL_OP_FOCUS_TAG:
  case LEME_CONTROL_OP_FOCUS_OUTPUT:
  case LEME_CONTROL_OP_SET_LAYOUT:
    return leme_workspace_control_prepare(server, intents, count, account, out,
                                          error);
  case LEME_CONTROL_OP_CONFIGURE_OUTPUT:
  case LEME_CONTROL_OP_SET_OUTPUT_POWER:
    return leme_output_control_prepare(server, intents, count, account, out,
                                       error);
  case LEME_CONTROL_OP_SET_INPUT:
  case LEME_CONTROL_OP_SET_MODE:
  case LEME_CONTROL_OP_SET_KEYBOARD_LAYOUT:
    return leme_input_control_prepare(server, intents, count, account, out,
                                      error);
  case LEME_CONTROL_OP_SET_CONFIG:
    return leme_config_live_prepare(server, intents, count, account, out,
                                    error);
  case LEME_CONTROL_OP_COMMAND:
    return command_control_prepare(server, intents, count, account, out, error);
  case LEME_CONTROL_OP_RELOAD_CONFIG: {
    const char *path = leme_config_path();
    if (path == NULL) {
      return set_preflight_error(error, LEME_CONTROL_NOT_FOUND,
                                 "cannot resolve configuration path");
    }
    char *cfg_err = NULL;
    struct leme_config *next = leme_config_load(path, &cfg_err);
    if (next == NULL) {
      char msg[256];
      (void)snprintf(msg, sizeof(msg), "%s",
                     cfg_err != NULL ? cfg_err
                                     : "failed to load configuration");
      enum leme_control_code err_code =
          (cfg_err != NULL && strstr(cfg_err, "not found") != NULL)
              ? LEME_CONTROL_NOT_FOUND
              : LEME_CONTROL_INVALID_ARGUMENT;
      free(cfg_err);
      return set_preflight_error(error, err_code, msg);
    }
    free(cfg_err);
    struct leme_config_reload *plan = NULL;
    enum leme_control_code code =
        leme_config_reload_prepare(server, next, account, &plan, error);
    if (code != LEME_CONTROL_OK) {
      leme_config_destroy(next);
      return code;
    }
    *out = (struct leme_control_prepared *)plan;
    return LEME_CONTROL_OK;
  }
  default:
    if (error != NULL) {
      error->code = LEME_CONTROL_UNSUPPORTED;
      error->phase = LEME_CONTROL_PREFLIGHT;
      (void)snprintf(error->message, sizeof(error->message),
                     "unsupported action opcode");
      (void)snprintf(error->expr_path, sizeof(error->expr_path), "/expr");
      error->effects_applied = false;
    }
    return LEME_CONTROL_UNSUPPORTED;
  }
}

static enum leme_control_code server_control_execute_one(
    void *context, struct leme_control_prepared *prepared, size_t index,
    enum leme_control_outcome *outcome, struct leme_control_error *error) {
  struct leme_server *server = context;
  if (prepared == NULL) {
    *outcome = LEME_CONTROL_NOOP;
    return LEME_CONTROL_OK;
  }
  const enum leme_control_opcode *op =
      (const enum leme_control_opcode *)prepared;
  if (*op == COMPOSITE_PREPARED_MAGIC) {
    struct composite_prepared *comp = (struct composite_prepared *)prepared;
    if (index >= comp->count) {
      *outcome = LEME_CONTROL_FAILED;
      return LEME_CONTROL_INVALID_ARGUMENT;
    }
    return server_control_execute_one(server, comp->items[index].prepared, 0,
                                      outcome, error);
  }

  switch (*op) {
  case LEME_CONTROL_OP_FOCUS:
  case LEME_CONTROL_OP_SET_FLOATING:
  case LEME_CONTROL_OP_SET_FULLSCREEN:
  case LEME_CONTROL_OP_RESIZE:
  case LEME_CONTROL_OP_CLOSE:
  case LEME_CONTROL_OP_MOVE_TO_TAG:
  case LEME_CONTROL_OP_MOVE_TO_OUTPUT:
  case LEME_CONTROL_OP_SET_STICKY:
    return leme_shell_control_execute_one(server, prepared, index, outcome,
                                          error);
  case LEME_CONTROL_OP_FOCUS_TAG:
  case LEME_CONTROL_OP_FOCUS_OUTPUT:
  case LEME_CONTROL_OP_SET_LAYOUT:
    return leme_workspace_control_execute_one(server, prepared, index, outcome,
                                              error);
  case LEME_CONTROL_OP_CONFIGURE_OUTPUT:
  case LEME_CONTROL_OP_SET_OUTPUT_POWER:
    return leme_output_control_execute_one(server, prepared, index, outcome,
                                           error);
  case LEME_CONTROL_OP_SET_INPUT:
  case LEME_CONTROL_OP_SET_MODE:
  case LEME_CONTROL_OP_SET_KEYBOARD_LAYOUT:
    return leme_input_control_execute_one(server, prepared, index, outcome,
                                          error);
  case LEME_CONTROL_OP_SET_CONFIG:
    return leme_config_live_execute_one(server, prepared, index, outcome,
                                        error);
  case LEME_CONTROL_OP_COMMAND:
    return command_control_execute_one(server, prepared, index, outcome, error);
  case LEME_CONTROL_OP_RELOAD_CONFIG: {
    struct leme_config_reload *plan = (struct leme_config_reload *)prepared;
    leme_config_reload_commit(server, plan);
    *outcome = LEME_CONTROL_APPLIED;
    return LEME_CONTROL_OK;
  }
  default:
    return LEME_CONTROL_UNSUPPORTED;
  }
}

static void server_control_discard(void *context,
                                   struct leme_control_prepared *prepared) {
  struct leme_server *server = context;
  if (prepared == NULL) {
    return;
  }
  const enum leme_control_opcode *op =
      (const enum leme_control_opcode *)prepared;
  if (*op == COMPOSITE_PREPARED_MAGIC) {
    struct composite_prepared *comp = (struct composite_prepared *)prepared;
    for (size_t i = 0; i < comp->count; ++i) {
      if (comp->items[i].prepared != NULL) {
        server_control_discard(server, comp->items[i].prepared);
      }
    }
    if (comp->items != NULL) {
      leme_control_free(comp->items);
    }
    leme_control_free(comp);
    return;
  }

  switch (*op) {
  case LEME_CONTROL_OP_FOCUS:
  case LEME_CONTROL_OP_SET_FLOATING:
  case LEME_CONTROL_OP_SET_FULLSCREEN:
  case LEME_CONTROL_OP_RESIZE:
  case LEME_CONTROL_OP_CLOSE:
  case LEME_CONTROL_OP_MOVE_TO_TAG:
  case LEME_CONTROL_OP_MOVE_TO_OUTPUT:
  case LEME_CONTROL_OP_SET_STICKY:
    leme_shell_control_discard(server, prepared);
    break;
  case LEME_CONTROL_OP_FOCUS_TAG:
  case LEME_CONTROL_OP_FOCUS_OUTPUT:
  case LEME_CONTROL_OP_SET_LAYOUT:
    leme_workspace_control_discard(server, prepared);
    break;
  case LEME_CONTROL_OP_CONFIGURE_OUTPUT:
  case LEME_CONTROL_OP_SET_OUTPUT_POWER:
    leme_output_control_discard(server, prepared);
    break;
  case LEME_CONTROL_OP_SET_INPUT:
  case LEME_CONTROL_OP_SET_MODE:
  case LEME_CONTROL_OP_SET_KEYBOARD_LAYOUT:
    leme_input_control_discard(server, prepared);
    break;
  case LEME_CONTROL_OP_SET_CONFIG:
    leme_config_live_discard(server, prepared);
    break;
  case LEME_CONTROL_OP_COMMAND:
    command_control_discard(server, prepared);
    break;
  case LEME_CONTROL_OP_RELOAD_CONFIG: {
    struct leme_config_reload *plan = (struct leme_config_reload *)prepared;
    leme_config_reload_discard(&plan);
    break;
  }
  default:
    break;
  }
}

struct leme_control_domain
leme_server_control_domain(struct leme_server *server) {
  return (struct leme_control_domain){
      .context = server,
      .prepare = server_control_prepare,
      .execute_one = server_control_execute_one,
      .discard = server_control_discard,
  };
}
