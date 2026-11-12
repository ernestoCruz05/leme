#ifndef LEME_COMMAND_H
#define LEME_COMMAND_H

#include "shell/placement.h"
#include "workspace/layout.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum leme_command_type {
#define LEME_COMMAND(id, ...) LEME_COMMAND_##id,
#include "core/commands.def"
#undef LEME_COMMAND
};

enum {
  LEME_COMMAND_COUNT = 0
#define LEME_COMMAND(...) +1
#include "core/commands.def"
#undef LEME_COMMAND
};

enum leme_command_flag {
  LEME_COMMAND_FLAG_NEEDS_TAGS = 1 << 0,
  LEME_COMMAND_FLAG_NEEDS_VIEW = 1 << 1,
  LEME_COMMAND_FLAG_CANCELS_GRAB = 1 << 2,
  LEME_COMMAND_FLAG_STICKY_ROOT = 1 << 3,
  LEME_COMMAND_FLAG_LOCKED_ALLOWED = 1 << 4,
  LEME_COMMAND_FLAG_LOCKED_REFUSED = 1 << 5,
  LEME_COMMAND_FLAG_LOCAL_ONLY = 1 << 6,
  LEME_COMMAND_FLAG_GESTURE_ONLY = 1 << 7,
  LEME_COMMAND_FLAG_GRID_ONLY = 1 << 8,
  LEME_COMMAND_FLAG_RESERVED = 1 << 9,
};

enum leme_command_arg_kind {
  LEME_COMMAND_ARG_END,
  LEME_COMMAND_ARG_CHOICE,
  LEME_COMMAND_ARG_KEYWORD,
  LEME_COMMAND_ARG_TAG,
  LEME_COMMAND_ARG_TAG_OR_STEP,
  LEME_COMMAND_ARG_OUTPUT,
  LEME_COMMAND_ARG_PIXELS,
  LEME_COMMAND_ARG_DELTA,
  LEME_COMMAND_ARG_VT,
  LEME_COMMAND_ARG_TEXT,
  LEME_COMMAND_ARG_NAME,
  LEME_COMMAND_ARG_ARGV,
};

struct leme_command_arg {
  enum leme_command_arg_kind kind;
  bool optional;
  const char *const *words;
  size_t word_count;
  size_t offset;
  size_t size;
  int fallback;
};

#define LEME_COMMAND_WORDS_MAX 4

struct leme_command_word {
  const char *text;
  int64_t number;
  bool is_number;
};

struct leme_command_spec {
  const char *name;
  const char *usage;
  const struct leme_command_arg *args;
  uint32_t flags;
};

enum leme_output_switch {
  LEME_OUTPUT_SWITCH_ON,
  LEME_OUTPUT_SWITCH_OFF,
  LEME_OUTPUT_SWITCH_TOGGLE,
};

struct leme_command {
  enum leme_command_type type;
  uint16_t tag_id;
  uint16_t vt;
  enum leme_direction direction;
  enum leme_resize_edge edge;
  enum leme_layout_kind layout;
  enum leme_view_placement placement;
  enum leme_output_switch output_switch;
  int amount;
  bool follow;
  bool backward;
  bool extend;
  bool occupied;
  bool has_direction;
  char *text;
  char **argv;
};

struct leme_server;
bool leme_command_parse(struct leme_command *command, char *const *params,
                        size_t params_len, char **error);
void leme_command_finish(struct leme_command *command);
const char *leme_command_placement_name(enum leme_view_placement placement);
const struct leme_command_spec *leme_command_spec(enum leme_command_type type);
bool leme_command_words(const struct leme_command *command,
                        struct leme_command_word *words, size_t *count);
bool leme_command_execute(struct leme_server *server,
                          const struct leme_command *command);

#endif
