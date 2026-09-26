#include "timao/cli-internal.h"
#include <string.h>

struct command_name {
  const char *name;
  size_t minimum, maximum;
};
static const struct command_name commands[] = {{"focus_next_tag", 0, 0},
                                               {"focus_previous_tag", 0, 0},
                                               {"focus_tag", 1, 1},
                                               {"focus", 1, 1},
                                               {"focus_last_tag", 0, 0},
                                               {"focus_previous_view", 0, 0},
                                               {"move", 1, 2},
                                               {"move_view_to_tag", 1, 2},
                                               {"focus_output", 1, 1},
                                               {"move_view_to_output", 1, 2},
                                               {"set_layout", 1, 1},
                                               {"switch_layout", 0, 0},
                                               {"remove_empty_tag", 1, 1},
                                               {"toggle_floating", 0, 0},
                                               {"toggle_sticky", 0, 0},
                                               {"toggle_fullscreen", 0, 0},
                                               {"resize", 2, 2},
                                               {"close_view", 0, 0},
                                               {"reload_config", 0, 0},
                                               {"mode", 1, 1},
                                               {"cycle_keyboard_layout", 0, 0},
                                               {"scratchpad_send", 0, 0},
                                               {"scratchpad_toggle", 0, 1},
                                               {"scratchpad_retrieve", 0, 0}};

static int usage(struct timao_diagnostic *error, const char *message) {
  timao_error(error, "usage_error", message);
  return 2;
}

int timao_cli_parse(int argc, char *const *argv,
                    struct timao_cli_arguments *out,
                    struct timao_diagnostic *error) {
  timao_diagnostic_destroy(error);
  if (out == NULL)
    return usage(error, "missing CLI argument output");
  *out = (struct timao_cli_arguments){.format = TIMAO_OUTPUT_JSON};
  if (argc < 1 || argv == NULL)
    return usage(error, "missing command; use --help");
  const size_t count = (size_t)argc;
  for (size_t i = 0; i < count; ++i)
    if (argv[i] == NULL)
      return usage(error, "invalid argv boundary");
  bool format = false, special = false;
  size_t index = 1;
  while (index < count && argv[index][0] == '-') {
    const char *option = argv[index++];
    if (strcmp(option, "--") == 0)
      break;
    if (strcmp(option, "--socket") == 0) {
      if (out->socket != NULL || index == count || argv[index][0] == '\0')
        return usage(error, "--socket requires one nonempty literal path");
      out->socket = argv[index++];
      if (strnlen(out->socket,
                  sizeof(((struct timao_endpoint *)0)->address.sun_path)) >=
          sizeof(((struct timao_endpoint *)0)->address.sun_path))
        return usage(error, "socket path is too long");
      continue;
    }
    if (strcmp(option, "--help") == 0 || strcmp(option, "--version") == 0) {
      const enum timao_cli_kind kind =
          strcmp(option, "--help") == 0 ? TIMAO_CLI_HELP : TIMAO_CLI_VERSION;
      if (special && out->kind != kind)
        return usage(error, "conflicting help/version options");
      special = true;
      out->kind = kind;
      continue;
    }
    enum timao_output_mode mode = TIMAO_OUTPUT_JSON;
    if (strcmp(option, "--raw") == 0)
      mode = TIMAO_OUTPUT_RAW;
    else if (strcmp(option, "--human") == 0)
      mode = TIMAO_OUTPUT_HUMAN;
    else if (strcmp(option, "--json") != 0)
      return usage(error, "unknown global option; use --help");
    if (format && mode != out->format)
      return usage(error, "conflicting output formats");
    format = true;
    out->format = mode;
  }
  if (special)
    return index == count
               ? 0
               : usage(error, "help/version take no command arguments");
  if (index == count)
    return usage(error, "missing command; use --help");
  const char *name = argv[index++];
  if (strcmp(name, "get") == 0 || strcmp(name, "sub") == 0)
    return usage(error,
                 "get/sub were removed; use eval with query/watch; see --help");
  if (strcmp(name, "eval") == 0) {
    if (count - index != 1)
      return usage(error, "usage: timao eval EXPR");
    out->kind = TIMAO_CLI_EVAL;
    out->source = argv[index];
    return 0;
  }
  if (strcmp(name, "run") == 0) {
    if (index == count)
      return usage(error, "usage: timao run FILE [ARG...]");
    out->kind = TIMAO_CLI_RUN;
    out->source = argv[index++];
    out->arguments = argv + index;
    out->count = count - index;
    return 0;
  }
  if (strcmp(name, "repl") == 0) {
    out->kind = TIMAO_CLI_REPL;
    return index == count ? 0 : usage(error, "usage: timao repl");
  }
  for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
    if (strcmp(name, commands[i].name) != 0)
      continue;
    if (count - index < commands[i].minimum ||
        count - index > commands[i].maximum)
      return usage(error, "incorrect command argument count; use --help");
    if (out->format == TIMAO_OUTPUT_RAW)
      return usage(error, "raw cannot represent a command action result; use "
                          "--json or --human");
    out->kind = TIMAO_CLI_COMMAND;
    out->command = commands[i].name;
    out->arguments = argv + index;
    out->count = count - index;
    return 0;
  }
  return usage(error, "unknown or unavailable command; use --help; use launch "
                      "for external programs");
}
