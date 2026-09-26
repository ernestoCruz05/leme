#include "workspace/control.h"

#include "control/action.h"
#include "control/error.h"
#include "control/memory.h"
#include "core/server.h"
#include "output/output.h"
#include "public/value.h"
#include "shell/view.h"
#include "workspace/layout.h"
#include "workspace/tag.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct workspace_prepared_item {
  uint64_t output_serial;
  uint16_t tag_number;
  struct leme_tag_materialize *mat_slot;
};

struct workspace_prepared {
  enum leme_control_opcode opcode;
  struct leme_public_budget *account;
  size_t count;
  struct workspace_prepared_item *items;
  enum leme_layout_kind layout_kind;
};

static enum leme_control_code set_preflight_error(
    struct leme_control_error *error, enum leme_control_code code,
    const char *msg) {
  if (error != NULL) {
    error->code = code;
    error->phase = LEME_CONTROL_PREFLIGHT;
    (void)snprintf(error->message, sizeof(error->message), "%s", msg);
    (void)snprintf(error->expr_path, sizeof(error->expr_path), "/expr");
    error->effects_applied = false;
  }
  return code;
}

enum leme_control_code leme_workspace_control_prepare(
    struct leme_server *server,
    const struct leme_control_intent *intents, size_t count,
    struct leme_public_budget *account,
    struct leme_control_prepared **out, struct leme_control_error *error) {
  if (server == NULL || intents == NULL || count == 0 || out == NULL) {
    return LEME_CONTROL_INVALID_ARGUMENT;
  }

  const enum leme_control_opcode opcode = intents[0].opcode;
  if (opcode == LEME_CONTROL_OP_FOCUS_TAG ||
      opcode == LEME_CONTROL_OP_FOCUS_OUTPUT) {
    if (count != 1) {
      return set_preflight_error(error, LEME_CONTROL_CARDINALITY,
                                 "single target required");
    }
  }

  enum leme_layout_kind parsed_layout = LEME_LAYOUT_DWINDLE;
  if (opcode == LEME_CONTROL_OP_SET_LAYOUT) {
    if (intents[0].args == NULL ||
        leme_public_kind(intents[0].args) != LEME_PUBLIC_STRING) {
      return set_preflight_error(error, LEME_CONTROL_INVALID_ARGUMENT,
                                 "invalid layout argument");
    }
    struct leme_public_text layout_text = {0};
    leme_public_as_text(intents[0].args, &layout_text);
    if (layout_text.length == 7 &&
        memcmp(layout_text.data, "dwindle", 7) == 0) {
      parsed_layout = LEME_LAYOUT_DWINDLE;
    } else if (layout_text.length == 12 &&
               memcmp(layout_text.data, "master_stack", 12) == 0) {
      parsed_layout = LEME_LAYOUT_MASTER_STACK;
    } else if (layout_text.length == 9 &&
               memcmp(layout_text.data, "accordion", 9) == 0) {
      parsed_layout = LEME_LAYOUT_ACCORDION;
    } else {
      return set_preflight_error(error, LEME_CONTROL_INVALID_ARGUMENT,
                                 "invalid layout name");
    }
  }

  struct workspace_prepared *prep =
      leme_control_alloc(account, sizeof(struct workspace_prepared));
  if (prep == NULL) {
    return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                               "out of memory");
  }
  prep->opcode = opcode;
  prep->account = account;
  prep->count = count;
  prep->layout_kind = parsed_layout;
  prep->items = leme_control_alloc(
      account, count * sizeof(struct workspace_prepared_item));
  if (prep->items == NULL) {
    leme_control_free(prep);
    return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                               "out of memory");
  }
  memset(prep->items, 0, count * sizeof(struct workspace_prepared_item));

  for (size_t i = 0; i < count; ++i) {
    struct leme_output *output =
        leme_output_by_public_id(server, intents[i].target.id);
    if (output == NULL) {
      leme_workspace_control_discard(server,
                                     (struct leme_control_prepared *)prep);
      return set_preflight_error(error, LEME_CONTROL_NOT_FOUND,
                                 "output not found");
    }
    prep->items[i].output_serial = intents[i].target.id.serial;
    prep->items[i].tag_number = intents[i].target.tag_number;
    prep->items[i].mat_slot = NULL;

    if (opcode == LEME_CONTROL_OP_FOCUS_TAG ||
        opcode == LEME_CONTROL_OP_SET_LAYOUT) {
      const uint16_t tag_number = intents[i].target.tag_number;
      if (tag_number == 0 || tag_number > output->tags.max_tags) {
        leme_workspace_control_discard(server,
                                       (struct leme_control_prepared *)prep);
        return set_preflight_error(error, LEME_CONTROL_INVALID_ARGUMENT,
                                   "invalid tag number");
      }
      if (opcode == LEME_CONTROL_OP_SET_LAYOUT &&
          output->tags.table[tag_number] == NULL) {
        bool already_prepared = false;
        for (size_t prev = 0; prev < i; ++prev) {
          if (prep->items[prev].output_serial == output->public_id.serial &&
              prep->items[prev].tag_number == tag_number) {
            already_prepared = true;
            break;
          }
        }
        if (!already_prepared) {
          if (!leme_tags_prepare_materialize(&output->tags, tag_number,
                                             &prep->items[i].mat_slot)) {
            leme_workspace_control_discard(
                server, (struct leme_control_prepared *)prep);
            return set_preflight_error(error, LEME_CONTROL_OUT_OF_MEMORY,
                                       "out of memory");
          }
        }
      }
    }
  }

  *out = (struct leme_control_prepared *)prep;
  return LEME_CONTROL_OK;
}

enum leme_control_code leme_workspace_control_execute_one(
    struct leme_server *server,
    struct leme_control_prepared *prepared, size_t index,
    enum leme_control_outcome *outcome, struct leme_control_error *error) {
  if (server == NULL || prepared == NULL || outcome == NULL) {
    return LEME_CONTROL_INVALID_ARGUMENT;
  }

  struct workspace_prepared *prep = (struct workspace_prepared *)prepared;
  if (index >= prep->count) {
    *outcome = LEME_CONTROL_FAILED;
    return LEME_CONTROL_INVALID_ARGUMENT;
  }

  struct workspace_prepared_item *item = &prep->items[index];
  struct leme_output *output = leme_output_by_public_id(
      server, (struct leme_public_id){item->output_serial});
  if (output == NULL) {
    *outcome = LEME_CONTROL_FAILED;
    if (error != NULL) {
      error->code = LEME_CONTROL_ACTION_FAILED;
      error->phase = LEME_CONTROL_EXECUTE;
      (void)snprintf(error->message, sizeof(error->message),
                     "output no longer available");
      (void)snprintf(error->expr_path, sizeof(error->expr_path), "/expr");
      error->effects_applied = false;
    }
    return LEME_CONTROL_ACTION_FAILED;
  }

  if (prep->opcode == LEME_CONTROL_OP_FOCUS_TAG) {
    if (server->focused_output == output &&
        output->tags.focused_id == item->tag_number) {
      *outcome = LEME_CONTROL_NOOP;
      return LEME_CONTROL_OK;
    }
    if (server->focused_output != output) {
      bool warp =
          server->config == NULL || server->config->output_policy.warp_cursor;
      leme_output_set_focused(server, output, warp);
    }
    leme_tags_focus_id(&output->tags, item->tag_number);
    leme_view_refresh_tag_focus(server);
    *outcome = LEME_CONTROL_APPLIED;
    return LEME_CONTROL_OK;
  } else if (prep->opcode == LEME_CONTROL_OP_FOCUS_OUTPUT) {
    if (server->focused_output == output) {
      *outcome = LEME_CONTROL_NOOP;
      return LEME_CONTROL_OK;
    }
    bool warp =
        server->config == NULL || server->config->output_policy.warp_cursor;
    leme_output_set_focused(server, output, warp);
    *outcome = LEME_CONTROL_APPLIED;
    return LEME_CONTROL_OK;
  } else if (prep->opcode == LEME_CONTROL_OP_SET_LAYOUT) {
    if (item->mat_slot != NULL) {
      leme_tags_commit_materialize(&item->mat_slot);
    }
    struct leme_tag *tag = output->tags.table[item->tag_number];
    if (tag == NULL) {
      *outcome = LEME_CONTROL_FAILED;
      return LEME_CONTROL_ACTION_FAILED;
    }
    if (tag->layout.kind == prep->layout_kind) {
      *outcome = LEME_CONTROL_NOOP;
      return LEME_CONTROL_OK;
    }
    if (!leme_tag_set_layout(tag, prep->layout_kind)) {
      *outcome = LEME_CONTROL_FAILED;
      return LEME_CONTROL_ACTION_FAILED;
    }
    leme_view_arrange(server);
    leme_tags_refresh_visibility(&output->tags);
    *outcome = LEME_CONTROL_APPLIED;
    return LEME_CONTROL_OK;
  }

  *outcome = LEME_CONTROL_FAILED;
  return LEME_CONTROL_UNSUPPORTED;
}

void leme_workspace_control_discard(
    struct leme_server *server,
    struct leme_control_prepared *prepared) {
  (void)server;
  if (prepared == NULL) {
    return;
  }
  struct workspace_prepared *prep = (struct workspace_prepared *)prepared;
  if (prep->items != NULL) {
    for (size_t i = 0; i < prep->count; ++i) {
      if (prep->items[i].mat_slot != NULL) {
        leme_tags_discard_materialize(&prep->items[i].mat_slot);
      }
    }
    leme_control_free(prep->items);
  }
  leme_control_free(prep);
}
