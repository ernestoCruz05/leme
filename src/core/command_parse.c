#include "core/command.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const leme_command_placements[] = {
    [LEME_VIEW_PLACEMENT_NORMAL] = "normal",
    [LEME_VIEW_PLACEMENT_MAXIMIZED] = "maximized",
    [LEME_VIEW_PLACEMENT_LEFT_HALF] = "left",
    [LEME_VIEW_PLACEMENT_RIGHT_HALF] = "right",
    [LEME_VIEW_PLACEMENT_TOP_LEFT] = "top_left",
    [LEME_VIEW_PLACEMENT_TOP_RIGHT] = "top_right",
    [LEME_VIEW_PLACEMENT_BOTTOM_LEFT] = "bottom_left",
    [LEME_VIEW_PLACEMENT_BOTTOM_RIGHT] = "bottom_right",
};

const char *leme_command_placement_name(enum leme_view_placement placement) {
  return placement <= LEME_VIEW_PLACEMENT_BOTTOM_RIGHT
             ? leme_command_placements[placement]
             : NULL;
}

enum leme_command_status {
  LEME_COMMAND_STATUS_OK,
  LEME_COMMAND_STATUS_INVALID,
  LEME_COMMAND_STATUS_MEMORY,
};

static const char *const leme_command_directions[] = {"left", "right", "up",
                                                      "down"};
static const char *const leme_command_layouts[] = {"dwindle", "master_stack",
                                                   "desktop"};
static const char *const leme_command_container_layouts[] = {"dwindle",
                                                             "master_stack"};
static const char *const leme_command_cycles[] = {"next", "previous"};
static const char *const leme_command_switches[] = {"on", "off", "toggle"};

_Static_assert(LEME_DIRECTION_LEFT == 0 && LEME_DIRECTION_DOWN == 3 &&
                   LEME_RESIZE_LEFT == 0 && LEME_RESIZE_DOWN == 3 &&
                   LEME_LAYOUT_DWINDLE == 0 && LEME_LAYOUT_DESKTOP == 2 &&
                   LEME_OUTPUT_SWITCH_ON == 0 &&
                   LEME_OUTPUT_SWITCH_TOGGLE == 2 &&
                   LEME_VIEW_PLACEMENT_NORMAL == 0,
               "command words follow the enum order");
_Static_assert(sizeof(enum leme_direction) == sizeof(unsigned) &&
                   sizeof(enum leme_resize_edge) == sizeof(unsigned) &&
                   sizeof(enum leme_layout_kind) == sizeof(unsigned) &&
                   sizeof(enum leme_view_placement) == sizeof(unsigned) &&
                   sizeof(enum leme_output_switch) == sizeof(unsigned),
               "command choices are stored as unsigned");

#define FIELD(member)                                                          \
  .offset = offsetof(struct leme_command, member),                             \
  .size = sizeof(((struct leme_command *)0)->member)
#define ARG_CHOICE(list, member)                                               \
  {.kind = LEME_COMMAND_ARG_CHOICE,                                            \
   .words = (list),                                                            \
   .word_count = LEME_ARRAY_LENGTH(list),                                      \
   FIELD(member)},
#define ARG_DIRECTION ARG_CHOICE(leme_command_directions, direction)
#define ARG_KEYWORD(word, member)                                              \
  {.kind = LEME_COMMAND_ARG_KEYWORD,                                           \
   .optional = true,                                                           \
   .words = (const char *const[]){word},                                       \
   .word_count = 1,                                                            \
   FIELD(member)},
#define ARG_TAG {.kind = LEME_COMMAND_ARG_TAG},
#define ARG_TAG_OR_STEP {.kind = LEME_COMMAND_ARG_TAG_OR_STEP},
#define ARG_OUTPUT {.kind = LEME_COMMAND_ARG_OUTPUT},
#define ARG_PIXELS(value)                                                      \
  {.kind = LEME_COMMAND_ARG_PIXELS, .optional = true, .fallback = (value)},
#define ARG_DELTA {.kind = LEME_COMMAND_ARG_DELTA},
#define ARG_VT {.kind = LEME_COMMAND_ARG_VT},
#define ARG_TEXT {.kind = LEME_COMMAND_ARG_TEXT},
#define ARG_NAME(may_omit)                                                     \
  {.kind = LEME_COMMAND_ARG_NAME, .optional = (may_omit)},
#define ARG_ARGV {.kind = LEME_COMMAND_ARG_ARGV},

#define LEME_COMMAND(id, label, help, bits, list)                              \
  static const struct leme_command_arg leme_command_args_##id[] = {            \
      list{.kind = LEME_COMMAND_ARG_END}};
#include "core/commands.def"
#undef LEME_COMMAND

#define NEEDS_TAGS LEME_COMMAND_FLAG_NEEDS_TAGS
#define NEEDS_VIEW LEME_COMMAND_FLAG_NEEDS_VIEW
#define CANCELS_GRAB LEME_COMMAND_FLAG_CANCELS_GRAB
#define STICKY_ROOT LEME_COMMAND_FLAG_STICKY_ROOT
#define LOCKED_ALLOWED LEME_COMMAND_FLAG_LOCKED_ALLOWED
#define LOCKED_REFUSED LEME_COMMAND_FLAG_LOCKED_REFUSED
#define LOCAL_ONLY LEME_COMMAND_FLAG_LOCAL_ONLY
#define GESTURE_ONLY LEME_COMMAND_FLAG_GESTURE_ONLY
#define GRID_ONLY LEME_COMMAND_FLAG_GRID_ONLY
#define RESERVED LEME_COMMAND_FLAG_RESERVED

static const struct leme_command_spec leme_command_specs[] = {
#define LEME_COMMAND(id, label, help, bits, list)                              \
  [LEME_COMMAND_##id] = {.name = (label),                                      \
                         .usage = (help),                                      \
                         .args = leme_command_args_##id,                       \
                         .flags = (bits)},
#include "core/commands.def"
#undef LEME_COMMAND
};

const struct leme_command_spec *leme_command_spec(enum leme_command_type type) {
  return &leme_command_specs[type];
}

static const struct leme_command_spec *leme_command_find(const char *name) {
  for (size_t index = 0; index < LEME_COMMAND_COUNT; index++) {
    if (strcmp(leme_command_specs[index].name, name) == 0) {
      return &leme_command_specs[index];
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

static bool leme_command_parse_int(const char *text, long minimum, int *value) {
  char *end;
  long parsed;

  errno = 0;
  parsed = strtol(text, &end, 10);
  if (errno != 0 || text[0] == '\0' || *end != '\0' || parsed < minimum ||
      parsed > INT_MAX) {
    return false;
  }
  *value = (int)parsed;
  return true;
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

static size_t leme_command_word_index(const struct leme_command_arg *arg,
                                      const char *text) {
  size_t index = 0;

  while (index < arg->word_count && strcmp(arg->words[index], text) != 0) {
    index++;
  }
  return index;
}

static enum leme_command_status
leme_command_parse_arg(struct leme_command *command,
                       const struct leme_command_arg *arg, const char *word) {
  void *field = (char *)command + arg->offset;
  size_t index;

  switch (arg->kind) {
  case LEME_COMMAND_ARG_CHOICE:
    index = leme_command_word_index(arg, word);
    if (index == arg->word_count) {
      return LEME_COMMAND_STATUS_INVALID;
    }
    if (arg->size == sizeof(bool)) {
      *(bool *)field = index != 0;
    } else {
      const unsigned value = (unsigned)index;
      memcpy(field, &value, sizeof(value));
    }
    return LEME_COMMAND_STATUS_OK;
  case LEME_COMMAND_ARG_KEYWORD:
    if (strcmp(arg->words[0], word) != 0) {
      return LEME_COMMAND_STATUS_INVALID;
    }
    *(bool *)field = true;
    return LEME_COMMAND_STATUS_OK;
  case LEME_COMMAND_ARG_TAG:
    return leme_command_parse_u16(word, &command->tag_id)
               ? LEME_COMMAND_STATUS_OK
               : LEME_COMMAND_STATUS_INVALID;
  case LEME_COMMAND_ARG_TAG_OR_STEP:
    if (strcmp(word, "next") == 0 || strcmp(word, "previous") == 0) {
      command->has_direction = true;
      command->direction =
          word[0] == 'n' ? LEME_DIRECTION_RIGHT : LEME_DIRECTION_LEFT;
      return LEME_COMMAND_STATUS_OK;
    }
    return leme_command_parse_u16(word, &command->tag_id)
               ? LEME_COMMAND_STATUS_OK
               : LEME_COMMAND_STATUS_INVALID;
  case LEME_COMMAND_ARG_OUTPUT:
    for (index = 0; index < LEME_ARRAY_LENGTH(leme_command_directions);
         index++) {
      if (strcmp(word, leme_command_directions[index]) == 0) {
        command->has_direction = true;
        command->direction = (enum leme_direction)index;
        return LEME_COMMAND_STATUS_OK;
      }
    }
    command->has_direction = false;
    break;
  case LEME_COMMAND_ARG_PIXELS:
    return leme_command_parse_int(word, 0, &command->amount) &&
                   command->amount != 0
               ? LEME_COMMAND_STATUS_OK
               : LEME_COMMAND_STATUS_INVALID;
  case LEME_COMMAND_ARG_DELTA:
    return leme_command_parse_int(word, -INT_MAX, &command->amount) &&
                   command->amount != 0
               ? LEME_COMMAND_STATUS_OK
               : LEME_COMMAND_STATUS_INVALID;
  case LEME_COMMAND_ARG_VT:
    return leme_command_parse_u16(word, &command->vt) && command->vt <= 12
               ? LEME_COMMAND_STATUS_OK
               : LEME_COMMAND_STATUS_INVALID;
  case LEME_COMMAND_ARG_TEXT:
    command->text = strdup(word);
    return command->text != NULL ? LEME_COMMAND_STATUS_OK
                                 : LEME_COMMAND_STATUS_MEMORY;
  case LEME_COMMAND_ARG_NAME:
    break;
  case LEME_COMMAND_ARG_ARGV:
  case LEME_COMMAND_ARG_END:
    return LEME_COMMAND_STATUS_INVALID;
  }
  if (word[0] == '\0') {
    return LEME_COMMAND_STATUS_INVALID;
  }
  command->text = strdup(word);
  return command->text != NULL ? LEME_COMMAND_STATUS_OK
                               : LEME_COMMAND_STATUS_MEMORY;
}

bool leme_command_parse(struct leme_command *command, char *const *params,
                        size_t params_len, char **error) {
  const struct leme_command_spec *spec;
  enum leme_command_status status = LEME_COMMAND_STATUS_OK;
  size_t used = 1;

  if (params_len == 0) {
    leme_command_set_error(error, "%s", "missing command");
    return false;
  }
  spec = leme_command_find(params[0]);
  if (spec == NULL) {
    leme_command_set_error(error, "unknown command '%s'", params[0]);
    return false;
  }
  command->type = (enum leme_command_type)(spec - leme_command_specs);
  for (const struct leme_command_arg *arg = spec->args;
       status == LEME_COMMAND_STATUS_OK && arg->kind != LEME_COMMAND_ARG_END;
       arg++) {
    if (arg->kind == LEME_COMMAND_ARG_ARGV) {
      if (used == params_len) {
        status = LEME_COMMAND_STATUS_INVALID;
        break;
      }
      command->argv = command_copy_argv(params[used], &params[used + 1],
                                        params_len - used - 1);
      status = command->argv != NULL ? LEME_COMMAND_STATUS_OK
                                     : LEME_COMMAND_STATUS_MEMORY;
      used = params_len;
    } else if (used < params_len) {
      status = leme_command_parse_arg(command, arg, params[used++]);
    } else if (!arg->optional) {
      status = LEME_COMMAND_STATUS_INVALID;
    } else if (arg->kind == LEME_COMMAND_ARG_PIXELS) {
      command->amount = arg->fallback;
    }
  }
  if (status == LEME_COMMAND_STATUS_OK && used == params_len) {
    return true;
  }
  leme_command_finish(command);
  if (status == LEME_COMMAND_STATUS_MEMORY) {
    leme_command_set_error(error, "%s", "out of memory");
  } else if (spec->usage == NULL) {
    leme_command_set_error(error, "unknown command '%s'", params[0]);
  } else if (spec->usage[0] == '\0') {
    leme_command_set_error(error, "usage: %s", params[0]);
  } else {
    leme_command_set_error(error, "usage: %s %s", params[0], spec->usage);
  }
  return false;
}

bool leme_command_words(const struct leme_command *command,
                        struct leme_command_word *words, size_t *count) {
  const struct leme_command_spec *spec = leme_command_spec(command->type);

  *count = 0;
  for (const struct leme_command_arg *arg = spec->args;
       arg->kind != LEME_COMMAND_ARG_END; arg++) {
    const void *field = (const char *)command + arg->offset;
    struct leme_command_word *word = &words[*count];
    unsigned choice = 0;

    *word = (struct leme_command_word){0};
    switch (arg->kind) {
    case LEME_COMMAND_ARG_CHOICE:
      if (arg->size == sizeof(bool)) {
        choice = *(const bool *)field ? 1 : 0;
      } else {
        memcpy(&choice, field, sizeof(choice));
      }
      if (choice >= arg->word_count) {
        return false;
      }
      word->text = arg->words[choice];
      break;
    case LEME_COMMAND_ARG_KEYWORD:
      if (!*(const bool *)field) {
        continue;
      }
      word->text = arg->words[0];
      break;
    case LEME_COMMAND_ARG_TAG_OR_STEP:
      if (command->has_direction) {
        if (command->direction != LEME_DIRECTION_LEFT &&
            command->direction != LEME_DIRECTION_RIGHT) {
          return false;
        }
        word->text =
            command->direction == LEME_DIRECTION_RIGHT ? "next" : "previous";
        break;
      }
      if (command->tag_id == 0) {
        return false;
      }
      word->is_number = true;
      word->number = command->tag_id;
      break;
    case LEME_COMMAND_ARG_TAG:
      if (command->tag_id == 0) {
        return false;
      }
      word->is_number = true;
      word->number = command->tag_id;
      break;
    case LEME_COMMAND_ARG_OUTPUT:
      if (command->has_direction) {
        if ((unsigned)command->direction >=
            LEME_ARRAY_LENGTH(leme_command_directions)) {
          return false;
        }
        word->text = leme_command_directions[command->direction];
        break;
      }
      if (command->text == NULL || command->text[0] == '\0') {
        return false;
      }
      word->text = command->text;
      break;
    case LEME_COMMAND_ARG_PIXELS:
    case LEME_COMMAND_ARG_DELTA:
      if (command->amount == 0 ||
          (arg->kind == LEME_COMMAND_ARG_PIXELS && command->amount < 0)) {
        return false;
      }
      word->is_number = true;
      word->number = command->amount;
      break;
    case LEME_COMMAND_ARG_VT:
      if (command->vt == 0 || command->vt > 12) {
        return false;
      }
      word->is_number = true;
      word->number = command->vt;
      break;
    case LEME_COMMAND_ARG_NAME:
      if (arg->optional && command->text == NULL) {
        continue;
      }
      if (command->text == NULL || command->text[0] == '\0') {
        return false;
      }
      word->text = command->text;
      break;
    case LEME_COMMAND_ARG_TEXT:
      if (command->text == NULL || command->text[0] == '\0') {
        return false;
      }
      word->text = command->text;
      break;
    case LEME_COMMAND_ARG_ARGV:
      if (command->argv == NULL || command->argv[0] == NULL) {
        return false;
      }
      continue;
    case LEME_COMMAND_ARG_END:
      continue;
    }
    (*count)++;
  }
  return true;
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
