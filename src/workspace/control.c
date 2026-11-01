#include "workspace/control.h"

#include "control/action.h"
#include "control/error.h"
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

static bool parse_layout(const struct leme_control_call *call,
                         enum leme_layout_kind *out) {
  struct leme_public_text text = {0};
  if (call->count == 0 ||
      leme_public_as_text(call->args[0], &text) != LEME_PUBLIC_OK)
    return false;
  static const struct {
    const char *name;
    enum leme_layout_kind kind;
  } layouts[] = {{"dwindle", LEME_LAYOUT_DWINDLE},
                 {"master_stack", LEME_LAYOUT_MASTER_STACK},
                 {"desktop", LEME_LAYOUT_DESKTOP}};
  for (size_t i = 0; i < sizeof(layouts) / sizeof(layouts[0]); i++) {
    if (text.length == strlen(layouts[i].name) &&
        memcmp(text.data, layouts[i].name, text.length) == 0) {
      *out = layouts[i].kind;
      return true;
    }
  }
  return false;
}

static void focus_output(struct leme_server *server,
                         struct leme_output *output) {
  bool warp =
      server->config == NULL || server->config->output_policy.warp_cursor;
  leme_output_set_focused(server, output, warp);
}

enum leme_control_code
leme_workspace_control_run(struct leme_server *server,
                           const struct leme_control_call *call, bool *applied,
                           struct leme_control_error *error) {
  *applied = false;
  struct leme_output *output =
      leme_output_by_public_id(server, call->target->id);
  if (output == NULL)
    return leme_control_refuse(error, LEME_CONTROL_NOT_FOUND,
                               "output not found");
  if (call->opcode == LEME_CONTROL_OP_FOCUS_OUTPUT) {
    if (server->focused_output == output)
      return LEME_CONTROL_OK;
    focus_output(server, output);
    *applied = true;
    return LEME_CONTROL_OK;
  }
  const uint16_t number = call->target->tag_number;
  if (number == 0 || number > output->tags.max_tags)
    return leme_control_refuse(error, LEME_CONTROL_INVALID_ARGUMENT,
                               "invalid tag number");
  if (call->opcode == LEME_CONTROL_OP_FOCUS_TAG) {
    if (server->focused_output == output && output->tags.focused_id == number)
      return LEME_CONTROL_OK;
    if (server->focused_output != output)
      focus_output(server, output);
    leme_tags_focus_id(&output->tags, number);
    leme_view_refresh_tag_focus(server);
    *applied = true;
    return LEME_CONTROL_OK;
  }
  enum leme_layout_kind layout = LEME_LAYOUT_DWINDLE;
  if (!parse_layout(call, &layout))
    return leme_control_refuse(error, LEME_CONTROL_INVALID_ARGUMENT,
                               "invalid layout name");
  if (output->tags.table[number] == NULL) {
    struct leme_tag_materialize *plan = NULL;
    if (!leme_tags_prepare_materialize(&output->tags, number, &plan))
      return leme_control_refuse(error, LEME_CONTROL_OUT_OF_MEMORY,
                                 "out of memory");
    leme_tags_commit_materialize(&plan);
  }
  struct leme_tag *tag = output->tags.table[number];
  if (tag != NULL && tag->layout.kind == layout)
    return LEME_CONTROL_OK;
  if (tag == NULL || !leme_tag_set_layout(tag, layout))
    return leme_control_fail(error, LEME_CONTROL_ACTION_FAILED,
                             "set layout failed");
  leme_view_refresh_begin(server);
  leme_view_arrange(server);
  leme_tags_refresh_visibility(&output->tags);
  leme_view_refresh_end(server);
  *applied = true;
  return LEME_CONTROL_OK;
}
