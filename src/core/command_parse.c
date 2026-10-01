#include "core/command.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct leme_command_usage {
  const char *name;
  const char *usage;
};

static const struct leme_command_usage leme_command_usages[] = {
    {"focus_next_tag", "focus_next_tag [occupied]"},
    {"focus_previous_tag", "focus_previous_tag [occupied]"},
    {"focus_tag", "focus_tag ID"},
    {"focus_last_tag", "focus_last_tag"},
    {"focus_previous_view", "focus_previous_view"},
    {"focus", "focus left|right|up|down"},
    {"move", "move left|right|up|down [PIXELS]"},
    {"move_view_to_tag", "move_view_to_tag next|previous|ID [follow]"},
    {"focus_output", "focus_output left|right|up|down|NAME"},
    {"move_view_to_output",
     "move_view_to_output left|right|up|down|NAME [follow]"},
    {"set_layout", "set_layout dwindle|master_stack|accordion"},
    {"switch_layout", "switch_layout"},
    {"remove_empty_tag", "remove_empty_tag ID"},
    {"toggle_floating", "toggle_floating"},
    {"toggle_sticky", "toggle_sticky"},
    {"toggle_fullscreen", "toggle_fullscreen"},
    {"scratchpad_send", "scratchpad_send"},
    {"scratchpad_toggle", "scratchpad_toggle [NAME]"},
    {"scratchpad_retrieve", "scratchpad_retrieve"},
    {"resize", "resize left|right|up|down PIXELS"},
    {"close_view", "close_view"},
    {"spawn", "spawn PROGRAM [ARG ...]"},
    {"reload_config", "reload_config"},
    {"mode", "mode NAME"},
    {"cycle_keyboard_layout", "cycle_keyboard_layout"},
    {"toggle_shortcuts_inhibit", "toggle_shortcuts_inhibit"},
    {"switch_vt", "switch_vt N"},
    {"quit", "quit"},
};

static const char *leme_command_usage(const char *name) {
  for (size_t index = 0;
       index < sizeof(leme_command_usages) / sizeof(leme_command_usages[0]);
       index++) {
    if (strcmp(leme_command_usages[index].name, name) == 0) {
      return leme_command_usages[index].usage;
    }
  }
  return NULL;
}

static void leme_command_set_error(char **error, const char *format, ...) {
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

static bool leme_command_parse_u16(const char *text, uint16_t *value) {
  char *end;
  unsigned long parsed;

  errno = 0;
  parsed = strtoul(text, &end, 10);
  if (errno != 0 || text[0] == '\0' || *end != '\0' || parsed == 0 ||
      parsed > UINT16_MAX) {
    return false;
  }
  *value = (uint16_t)parsed;
  return true;
}

static bool leme_command_parse_nonnegative(const char *text, int *value) {
  char *end;
  long parsed;

  errno = 0;
  parsed = strtol(text, &end, 10);
  if (errno != 0 || text[0] == '\0' || *end != '\0' || parsed < 0 ||
      parsed > INT_MAX) {
    return false;
  }
  *value = (int)parsed;
  return true;
}

static bool leme_command_parse_layout_kind(const char *text,
                                           enum leme_layout_kind *kind) {
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

static bool leme_command_parse_direction(const char *text,
                                         enum leme_direction *direction) {
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

static bool leme_command_parse_output_target(const char *text,
                                             struct leme_command *command) {
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

static char **command_copy_argv(const char *name, char *const *params,
                                size_t count) {
  char **argv = (char **)calloc(count + 2, sizeof(*argv));
  size_t index;

  if (argv == NULL) {
    return NULL;
  }
  argv[0] = strdup(name);
  if (argv[0] == NULL) {
    free((void *)argv);
    return NULL;
  }
  for (index = 0; index < count; index++) {
    argv[index + 1] = strdup(params[index]);
    if (argv[index + 1] == NULL) {
      while (index > 0) {
        free(argv[index--]);
      }
      free(argv[0]);
      free((void *)argv);
      return NULL;
    }
  }
  argv[count + 1] = NULL;
  return argv;
}

bool leme_command_parse(struct leme_command *command, char *const *params,
                        size_t params_len, char **error) {
  const char *name;
  const char *usage;
  size_t arguments;

  if (params_len == 0) {
    leme_command_set_error(error, "%s", "missing command");
    return false;
  }
  name = params[0];
  arguments = params_len - 1;
  if ((strcmp(name, "focus_next_tag") == 0 ||
       strcmp(name, "focus_previous_tag") == 0) &&
      arguments <= 1) {
    command->type = strcmp(name, "focus_next_tag") == 0
                        ? LEME_COMMAND_FOCUS_NEXT_TAG
                        : LEME_COMMAND_FOCUS_PREVIOUS_TAG;
    if (arguments == 1 && strcmp(params[1], "occupied") != 0) {
      goto invalid;
    }
    command->occupied = arguments == 1;
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
  } else if (strcmp(name, "move") == 0 && (arguments == 1 || arguments == 2)) {
    command->type = LEME_COMMAND_MOVE_DIRECTION;
    command->amount = 40;
    if (!leme_command_parse_direction(params[1], &command->direction) ||
        (arguments == 2 &&
         (!leme_command_parse_nonnegative(params[2], &command->amount) ||
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
  } else if (strcmp(name, "toggle_sticky") == 0 && arguments == 0) {
    command->type = LEME_COMMAND_TOGGLE_STICKY;
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
    command->argv = command_copy_argv(params[1], &params[2], arguments - 1);
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
  } else if (strcmp(name, "cycle_keyboard_layout") == 0 && arguments == 0) {
    command->type = LEME_COMMAND_CYCLE_KEYBOARD_LAYOUT;
  } else if (strcmp(name, "toggle_shortcuts_inhibit") == 0 && arguments == 0) {
    command->type = LEME_COMMAND_TOGGLE_SHORTCUTS_INHIBIT;
  } else if (strcmp(name, "switch_vt") == 0 && arguments == 1) {
    command->type = LEME_COMMAND_SWITCH_VT;
    if (!leme_command_parse_u16(params[1], &command->vt) || command->vt == 0 ||
        command->vt > 12) {
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
  leme_command_finish(command);
  usage = leme_command_usage(name);
  if (usage == NULL) {
    leme_command_set_error(error, "unknown command '%s'", name);
  } else {
    leme_command_set_error(error, "usage: %s", usage);
  }
  return false;
allocation:
  leme_command_finish(command);
  leme_command_set_error(error, "%s", "out of memory");
  return false;
}

void leme_command_finish(struct leme_command *command) {
  if (command == NULL) {
    return;
  }
  free(command->text);
  command->text = NULL;
  if (command->argv != NULL) {
    for (size_t index = 0; command->argv[index] != NULL; index++) {
      free(command->argv[index]);
    }
    free((void *)command->argv);
    command->argv = NULL;
  }
}
