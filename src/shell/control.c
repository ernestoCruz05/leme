#include "shell/control.h"

#include "config/config.h"
#include "config/live.h"
#include "config/prepare.h"
#include "control/action.h"
#include "control/error.h"
#include "control/memory.h"
#include "core/command.h"
#include "core/server.h"
#include "desktop/target.h"
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

struct shell_prepared_item {
  uint64_t serial;
  uint64_t container_serial;
  uint64_t container_generation;
  uint64_t minimize_attempted_serial;
};

struct shell_prepared {
  enum leme_control_opcode opcode;
  struct leme_public_budget *account;
  size_t count;
  struct shell_prepared_item *items;
  bool target_bool;
  enum leme_resize_edge resize_edge;
  enum leme_layout_kind layout;
  enum leme_direction direction;
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

static enum leme_control_code
shell_prepare_fail(struct leme_server *server, struct shell_prepared *prep,
                   struct leme_control_error *error,
                   enum leme_control_code code, const char *message) {
  leme_shell_control_discard(server, (struct leme_control_prepared *)prep);
  return set_preflight_error(error, code, message);
}

enum leme_control_code leme_shell_control_prepare(
    struct leme_server *server, const struct leme_control_intent *intents,
    size_t count, struct leme_public_budget *account,
    struct leme_control_prepared **out, struct leme_control_error *error) {
  if (server == NULL || intents == NULL || count == 0 || out == NULL) {
    return LEME_CONTROL_INVALID_ARGUMENT;
  }

  const enum leme_control_opcode opcode = intents[0].opcode;
  if (opcode == LEME_CONTROL_OP_FOCUS || opcode == LEME_CONTROL_OP_RESIZE ||
      opcode == LEME_CONTROL_OP_CONTAINER_LAYOUT ||
      opcode == LEME_CONTROL_OP_CONTAINER_DETACH ||
      opcode == LEME_CONTROL_OP_CONTAINER_DISSOLVE ||
      opcode == LEME_CONTROL_OP_CONTAINER_JOIN) {
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

  if (count > SIZE_MAX / sizeof(struct shell_prepared_item)) {
    return set_preflight_error(error, LEME_CONTROL_RESOURCE_LIMIT,
                               "too many targets");
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
        return shell_prepare_fail(server, prep, error,
                                  LEME_CONTROL_OUT_OF_MEMORY, "out of memory");
      }
    }
  } else if (opcode == LEME_CONTROL_OP_MOVE_TO_OUTPUT) {
    prep->dest_output_serial = intents[0].destination.id.serial;
    prep->dest_tag_slot = intents[0].destination.tag_number;
  } else if (opcode == LEME_CONTROL_OP_CONTAINER_LAYOUT) {
    struct leme_public_text name = {0};
    bool valid = false;
    if (leme_public_as_text(intents[0].args, &name) == LEME_PUBLIC_OK) {
      for (enum leme_layout_kind kind = LEME_LAYOUT_DWINDLE;
           kind <= LEME_LAYOUT_MASTER_STACK; kind++) {
        const char *candidate = leme_layout_public_name(kind);
        if (name.length == strlen(candidate) &&
            memcmp(name.data, candidate, name.length) == 0) {
          prep->layout = kind;
          valid = true;
          break;
        }
      }
    }
    if (!valid) {
      return shell_prepare_fail(server, prep, error,
                                LEME_CONTROL_INVALID_ARGUMENT,
                                "invalid container layout");
    }
  } else if (opcode == LEME_CONTROL_OP_CONTAINER_JOIN) {
    static const char *const names[] = {"left", "right", "up", "down"};
    static const enum leme_direction directions[] = {
        LEME_DIRECTION_LEFT, LEME_DIRECTION_RIGHT, LEME_DIRECTION_UP,
        LEME_DIRECTION_DOWN};
    struct leme_public_text name = {0};
    bool valid = false;
    if (leme_public_as_text(intents[0].args, &name) == LEME_PUBLIC_OK) {
      for (size_t i = 0; i < LEME_ARRAY_LENGTH(names); i++) {
        if (name.length == strlen(names[i]) &&
            memcmp(name.data, names[i], name.length) == 0) {
          prep->direction = directions[i];
          valid = true;
          break;
        }
      }
    }
    if (!valid) {
      return shell_prepare_fail(server, prep, error,
                                LEME_CONTROL_INVALID_ARGUMENT,
                                "invalid container join direction");
    }
  } else if (opcode == LEME_CONTROL_OP_SET_MINIMIZED) {
    if (intents[0].args == NULL ||
        leme_public_as_bool(intents[0].args, &prep->target_bool) !=
            LEME_PUBLIC_OK) {
      return shell_prepare_fail(server, prep, error,
                                LEME_CONTROL_INVALID_ARGUMENT,
                                "minimized state must be boolean");
    }
  } else if (opcode == LEME_CONTROL_OP_SET_STICKY) {
    bool target_bool = false;
    if (intents[0].args != NULL) {
      leme_public_as_bool(intents[0].args, &target_bool);
    }
    prep->target_bool = target_bool;
  }

  for (size_t i = 0; i < count; ++i) {
    prep->items[i] =
        (struct shell_prepared_item){.serial = intents[i].target.id.serial};
    struct leme_view *view =
        leme_view_by_public_id(server, intents[i].target.id);
    if (view == NULL || !view->mapped || view->unmanaged) {
      return shell_prepare_fail(server, prep, error, LEME_CONTROL_NOT_FOUND,
                                "view not found or unmapped");
    }

    if (opcode == LEME_CONTROL_OP_CONTAINER_LAYOUT ||
        opcode == LEME_CONTROL_OP_CONTAINER_DETACH ||
        opcode == LEME_CONTROL_OP_CONTAINER_DISSOLVE) {
      if (!leme_container_action_allowed(view)) {
        return shell_prepare_fail(server, prep, error,
                                  LEME_CONTROL_ACTION_FAILED,
                                  "container action unavailable");
      }
      prep->items[i].container_serial = view->container->id.serial;
      prep->items[i].container_generation = view->container->generation;
    } else if (opcode == LEME_CONTROL_OP_CONTAINER_JOIN) {
      if (!leme_container_member_eligible(view) || view->container != NULL) {
        return shell_prepare_fail(server, prep, error,
                                  LEME_CONTROL_ACTION_FAILED,
                                  "container join unavailable");
      }
    } else if (opcode == LEME_CONTROL_OP_FOCUS) {
      if (leme_view_is_scratchpad(view) &&
          !leme_view_is_shown_scratchpad(view)) {
        return shell_prepare_fail(server, prep, error,
                                  LEME_CONTROL_ACTION_FAILED,
                                  "cannot focus hidden scratchpad");
      }
      if (!leme_ownership_focus_eligible(view) &&
          !leme_view_is_minimized(view)) {
        return shell_prepare_fail(server, prep, error,
                                  LEME_CONTROL_ACTION_FAILED,
                                  "view not eligible for focus");
      }
    } else if (opcode == LEME_CONTROL_OP_SET_MINIMIZED) {
      if (prep->target_bool && !leme_view_can_minimize(view)) {
        return shell_prepare_fail(server, prep, error,
                                  LEME_CONTROL_ACTION_FAILED,
                                  "view incompatible with minimization");
      }
    } else if (opcode == LEME_CONTROL_OP_SET_FLOATING) {
      if (leme_view_is_scratchpad(view) || leme_view_is_sticky(view) ||
          leme_view_geometry_owner(view) == LEME_VIEW_GEOMETRY_CONTAINER) {
        return shell_prepare_fail(server, prep, error,
                                  LEME_CONTROL_ACTION_FAILED,
                                  "view incompatible with floating");
      }
    } else if (opcode == LEME_CONTROL_OP_SET_FULLSCREEN) {
      if (leme_view_is_scratchpad(view)) {
        return shell_prepare_fail(server, prep, error,
                                  LEME_CONTROL_ACTION_FAILED,
                                  "scratchpad incompatible with fullscreen");
      }
    } else if (opcode == LEME_CONTROL_OP_RESIZE) {
      if (view->fullscreen || leme_view_is_placed(view) ||
          (view->container != NULL && !leme_container_action_allowed(view))) {
        return shell_prepare_fail(
            server, prep, error, LEME_CONTROL_ACTION_FAILED,
            "cannot resize fullscreen, maximized, snapped or unavailable view");
      }
    } else if (opcode == LEME_CONTROL_OP_MOVE_TO_TAG) {
      if (leme_view_is_scratchpad(view)) {
        return shell_prepare_fail(server, prep, error,
                                  LEME_CONTROL_ACTION_FAILED,
                                  "scratchpad cannot be moved to tag");
      }
      if (view->fullscreen) {
        return shell_prepare_fail(server, prep, error,
                                  LEME_CONTROL_ACTION_FAILED,
                                  "cannot move fullscreen view");
      }
    } else if (opcode == LEME_CONTROL_OP_MOVE_TO_OUTPUT) {
      if (leme_view_is_scratchpad(view)) {
        return shell_prepare_fail(server, prep, error,
                                  LEME_CONTROL_ACTION_FAILED,
                                  "scratchpad cannot be moved to output");
      }
      if (view->fullscreen) {
        return shell_prepare_fail(server, prep, error,
                                  LEME_CONTROL_ACTION_FAILED,
                                  "cannot move fullscreen view");
      }
    } else if (opcode == LEME_CONTROL_OP_SET_STICKY) {
      if (leme_view_is_scratchpad(view) || view->fullscreen) {
        return shell_prepare_fail(server, prep, error,
                                  LEME_CONTROL_ACTION_FAILED,
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
            return shell_prepare_fail(
                server, prep, error, LEME_CONTROL_ACTION_FAILED,
                "multiple views on same tag cannot be fullscreen");
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
      return shell_prepare_fail(server, prep, error,
                                LEME_CONTROL_INVALID_ARGUMENT,
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
      return shell_prepare_fail(server, prep, error,
                                LEME_CONTROL_INVALID_ARGUMENT,
                                "invalid resize direction");
    }

    double amt_num = 0.0;
    leme_public_as_number(amt_val, &amt_num);
    if (amt_num == 0.0 || amt_num < -INT_MAX || amt_num > INT_MAX) {
      return shell_prepare_fail(server, prep, error,
                                LEME_CONTROL_INVALID_ARGUMENT,
                                "resize amount must be a non-zero pixel count");
    }
    prep->resize_amount = (int)amt_num;
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
    if (leme_view_is_minimized(view) &&
        (leme_session_locked(server) ||
         leme_layer_keyboard_is_exclusive(server) ||
         leme_input_interacting(server) ||
         !leme_view_set_minimized(view, false))) {
      *outcome = LEME_CONTROL_FAILED;
      return LEME_CONTROL_ACTION_FAILED;
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
  case LEME_CONTROL_OP_SET_MINIMIZED: {
    if (leme_view_is_minimized(view) == prep->target_bool) {
      *outcome = LEME_CONTROL_NOOP;
      return LEME_CONTROL_OK;
    }
    uint64_t container_serial = 0;
    struct leme_desk_target unit = {0};
    if (prep->target_bool && view->container != NULL) {
      container_serial = view->container->id.serial;
    } else if (!prep->target_bool && leme_view_minimized_unit(view, &unit) &&
               unit.kind == LEME_DESK_TARGET_CONTAINER) {
      container_serial = unit.id.serial;
    }
    if (container_serial != 0) {
      for (size_t i = 0; i < prep->count; i++) {
        if (prep->items[i].minimize_attempted_serial == container_serial) {
          *outcome = LEME_CONTROL_FAILED;
          return LEME_CONTROL_ACTION_FAILED;
        }
      }
      prep->items[index].minimize_attempted_serial = container_serial;
    }
    if (!leme_view_set_minimized(view, prep->target_bool) ||
        leme_view_is_minimized(view) != prep->target_bool) {
      *outcome = LEME_CONTROL_FAILED;
      return LEME_CONTROL_ACTION_FAILED;
    }
    *outcome = LEME_CONTROL_APPLIED;
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
    if (view->fullscreen || leme_view_is_placed(view)) {
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
  case LEME_CONTROL_OP_CONTAINER_LAYOUT:
  case LEME_CONTROL_OP_CONTAINER_DETACH:
  case LEME_CONTROL_OP_CONTAINER_DISSOLVE: {
    if (!leme_container_action_allowed(view) ||
        view->container->id.serial != prep->items[index].container_serial ||
        view->container->generation !=
            prep->items[index].container_generation) {
      *outcome = LEME_CONTROL_FAILED;
      return LEME_CONTROL_ACTION_FAILED;
    }
    if (prep->opcode == LEME_CONTROL_OP_CONTAINER_LAYOUT &&
        view->container->layout.kind == prep->layout) {
      *outcome = LEME_CONTROL_NOOP;
      return LEME_CONTROL_OK;
    }
    const bool applied =
        prep->opcode == LEME_CONTROL_OP_CONTAINER_LAYOUT
            ? leme_container_set_layout(view->container, prep->layout)
        : prep->opcode == LEME_CONTROL_OP_CONTAINER_DETACH
            ? leme_container_detach_current(view)
            : leme_container_dissolve(view->container);
    *outcome = applied ? LEME_CONTROL_APPLIED : LEME_CONTROL_FAILED;
    return applied ? LEME_CONTROL_OK : LEME_CONTROL_ACTION_FAILED;
  }
  case LEME_CONTROL_OP_CONTAINER_JOIN: {
    const bool joined = leme_container_join(view, prep->direction);
    *outcome = joined ? LEME_CONTROL_APPLIED : LEME_CONTROL_FAILED;
    return joined ? LEME_CONTROL_OK : LEME_CONTROL_ACTION_FAILED;
  }
  case LEME_CONTROL_OP_CLOSE: {
    leme_view_protocol_close(view);
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
    bool ok = false;
    if (leme_view_is_sticky(view)) {
      ok = leme_sticky_move_to_tag(view, prep->dest_tag_slot, false);
    } else {
      ok = leme_tags_adopt_view(&dest_output->tags, view, prep->dest_tag_slot);
    }
    if (ok && reanchor && !placed &&
        leme_view_geometry_owner(view) == LEME_VIEW_GEOMETRY_FLOATING) {
      const struct leme_box box =
          leme_view_policy_clamp_box(translated, dest_output->usable_box);
      ok = leme_view_apply_interactive_box(
          view, box,
          box.width != previous_box.width || box.height != previous_box.height);
    } else if (!ok && placed) {
      view->placement.restore_box = previous_box;
    }
    if (ok) {
      leme_view_refresh_begin(server);
      leme_view_arrange(server);
      if (cur_output != NULL && cur_output != dest_output) {
        leme_tags_refresh_visibility(leme_output_tags(cur_output));
      }
      leme_tags_refresh_visibility(leme_output_tags(dest_output));
      leme_view_refresh_end(server);
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

#define COMMAND_WORD_MAX 4

struct command_prepared {
  enum leme_control_opcode opcode;
  struct leme_public_budget *account;
  struct leme_command cmd;
};

static void command_control_free_words(char **words, size_t count) {
  for (size_t index = 0; index < count; index++) {
    free(words[index]);
  }
}

static bool command_control_lowered(enum leme_command_type type) {
  switch (type) {
  case LEME_COMMAND_FOCUS_DIRECTION:
  case LEME_COMMAND_FOCUS_PREVIOUS_VIEW:
  case LEME_COMMAND_FOCUS_URGENT:
  case LEME_COMMAND_FOCUS_LAST_TAG:
  case LEME_COMMAND_FOCUS_OUTPUT:
  case LEME_COMMAND_MOVE_DIRECTION:
  case LEME_COMMAND_MOVE_VIEW_TO_OUTPUT:
  case LEME_COMMAND_SWITCH_LAYOUT:
  case LEME_COMMAND_REMOVE_EMPTY_TAG:
  case LEME_COMMAND_CYCLE_KEYBOARD_LAYOUT:
  case LEME_COMMAND_TOGGLE_SHORTCUTS_INHIBIT:
  case LEME_COMMAND_SCRATCHPAD_SEND:
  case LEME_COMMAND_SCRATCHPAD_TOGGLE:
  case LEME_COMMAND_SCRATCHPAD_RETRIEVE:
  case LEME_COMMAND_SNAP:
  case LEME_COMMAND_PLACE:
  case LEME_COMMAND_WINDOW_MENU:
  case LEME_COMMAND_SHOW_DESKTOP:
  case LEME_COMMAND_FOCUS_CYCLE:
  case LEME_COMMAND_FOCUS_DESKTOP:
  case LEME_COMMAND_DESKTOP_MENU:
  case LEME_COMMAND_GRID_OPEN:
  case LEME_COMMAND_GRID_TRASH:
  case LEME_COMMAND_GRID_RENAME:
  case LEME_COMMAND_GRID_NEW_FOLDER:
  case LEME_COMMAND_GRID_SELECT_ALL:
  case LEME_COMMAND_GRID_CLEAR:
  case LEME_COMMAND_GRID_SELECT:
    return true;
  default:
    return false;
  }
}

static enum leme_control_code
command_control_check(struct leme_server *server,
                      const struct leme_command *cmd,
                      struct leme_control_error *error) {
  const struct leme_view *view = server->focused_view;
  const bool needs_view = cmd->type == LEME_COMMAND_MOVE_DIRECTION ||
                          cmd->type == LEME_COMMAND_MOVE_VIEW_TO_OUTPUT ||
                          cmd->type == LEME_COMMAND_SCRATCHPAD_SEND ||
                          cmd->type == LEME_COMMAND_SNAP ||
                          cmd->type == LEME_COMMAND_PLACE ||
                          cmd->type == LEME_COMMAND_WINDOW_MENU;

  if (!command_control_lowered(cmd->type)) {
    return set_preflight_error(error, LEME_CONTROL_UNSUPPORTED,
                               "unknown command");
  }
  if (needs_view && (view == NULL || !view->mapped || view->unmanaged)) {
    return set_preflight_error(error, LEME_CONTROL_NOT_FOUND,
                               "no focused view");
  }
  if (cmd->type == LEME_COMMAND_MOVE_VIEW_TO_OUTPUT &&
      leme_view_is_scratchpad(view)) {
    return set_preflight_error(error, LEME_CONTROL_ACTION_FAILED,
                               "scratchpad cannot be moved to output");
  }
  if (cmd->type == LEME_COMMAND_SCRATCHPAD_SEND &&
      leme_view_is_scratchpad(view)) {
    return set_preflight_error(error, LEME_CONTROL_ACTION_FAILED,
                               "already a scratchpad");
  }
  if (cmd->type == LEME_COMMAND_SCRATCHPAD_TOGGLE && cmd->text != NULL &&
      (server->config == NULL ||
       leme_config_scratchpad(server->config, cmd->text) == NULL)) {
    return set_preflight_error(error, LEME_CONTROL_NOT_FOUND,
                               "scratchpad not configured");
  }
  return LEME_CONTROL_OK;
}

static enum leme_control_code command_control_prepare(
    struct leme_server *server, const struct leme_control_intent *intents,
    size_t count, struct leme_public_budget *account,
    struct leme_control_prepared **out, struct leme_control_error *error) {
  char *words[COMMAND_WORD_MAX] = {0};
  char *parse_error = NULL;
  size_t word_count;
  bool parsed;

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
  word_count = leme_public_length(argv);
  if (word_count == 0 || word_count > COMMAND_WORD_MAX) {
    return set_preflight_error(error, LEME_CONTROL_INVALID_ARGUMENT,
                               "missing command name");
  }
  for (size_t index = 0; index < word_count; index++) {
    const struct leme_public_value *word = leme_public_at(argv, index);
    struct leme_public_text text = {0};

    if (word == NULL || leme_public_kind(word) != LEME_PUBLIC_STRING) {
      command_control_free_words(words, word_count);
      return set_preflight_error(error, LEME_CONTROL_INVALID_ARGUMENT,
                                 "command args must be strings");
    }
    leme_public_as_text(word, &text);
    words[index] = strndup(text.data != NULL ? text.data : "", text.length);
    if (words[index] == NULL) {
      command_control_free_words(words, word_count);
      return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                 "out of memory");
    }
  }

  struct command_prepared *prep =
      leme_control_alloc(account, sizeof(struct command_prepared));
  if (prep == NULL) {
    command_control_free_words(words, word_count);
    return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                               "out of memory");
  }
  memset(prep, 0, sizeof(*prep));
  prep->opcode = LEME_CONTROL_OP_COMMAND;
  prep->account = account;

  parsed = leme_command_parse(&prep->cmd, words, word_count, &parse_error);
  command_control_free_words(words, word_count);
  if (!parsed) {
    const enum leme_control_code code = set_preflight_error(
        error, LEME_CONTROL_INVALID_ARGUMENT,
        parse_error != NULL ? parse_error : "invalid command");

    free(parse_error);
    leme_control_free(prep);
    return code;
  }
  const enum leme_control_code code =
      command_control_check(server, &prep->cmd, error);
  if (code != LEME_CONTROL_OK) {
    leme_command_finish(&prep->cmd);
    leme_control_free(prep);
    return code;
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
  const struct leme_box old_box =
      old_view != NULL ? old_view->box : (struct leme_box){0};
  const struct leme_output *old_view_output =
      old_view != NULL ? leme_view_output(old_view) : NULL;

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
      prep->cmd.type == LEME_COMMAND_FOCUS_PREVIOUS_VIEW ||
      prep->cmd.type == LEME_COMMAND_FOCUS_CYCLE ||
      prep->cmd.type == LEME_COMMAND_FOCUS_URGENT) {
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
  } else if (prep->cmd.type == LEME_COMMAND_MOVE_DIRECTION) {
    const bool moved =
        old_view != NULL &&
        (old_view->box.x != old_box.x || old_view->box.y != old_box.y ||
         leme_view_output(old_view) != old_view_output);
    *outcome = moved ? LEME_CONTROL_APPLIED : LEME_CONTROL_NOOP;
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
  leme_command_finish(&prep->cmd);
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
  case LEME_CONTROL_OP_SET_MINIMIZED:
  case LEME_CONTROL_OP_CONTAINER_DETACH:
  case LEME_CONTROL_OP_CONTAINER_DISSOLVE:
  case LEME_CONTROL_OP_CONTAINER_LAYOUT:
  case LEME_CONTROL_OP_CONTAINER_JOIN:
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
  case LEME_CONTROL_OP_SET_MINIMIZED:
  case LEME_CONTROL_OP_CONTAINER_DETACH:
  case LEME_CONTROL_OP_CONTAINER_DISSOLVE:
  case LEME_CONTROL_OP_CONTAINER_LAYOUT:
  case LEME_CONTROL_OP_CONTAINER_JOIN:
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
    wlr_log(WLR_INFO, "leme: reloaded %s",
            server->config != NULL && server->config->path != NULL
                ? server->config->path
                : "the configuration");
    leme_server_report_diagnostics(server);
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
  case LEME_CONTROL_OP_SET_MINIMIZED:
  case LEME_CONTROL_OP_CONTAINER_DETACH:
  case LEME_CONTROL_OP_CONTAINER_DISSOLVE:
  case LEME_CONTROL_OP_CONTAINER_LAYOUT:
  case LEME_CONTROL_OP_CONTAINER_JOIN:
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
