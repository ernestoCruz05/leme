#include "timao/cli.h"
#include "timao/cli-internal.h"
#include "control/memory.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char help[] =
    "Usage: timao [--json|--raw|--human] [--socket PATH] COMMAND\n"
    "  eval EXPR            evaluate one expression and display its result\n"
    "  run FILE [ARG...]    execute a whole file; '-' reads source from stdin\n"
    "  repl                 interactive interpreter; no startup files\n"
    "  --help, --version    no connection required\n"
    "Global options precede COMMAND. JSON is the default, including on "
    "terminals.\n"
    "Simple commands preserve argv boundaries and use Leme's command adapter:\n"
    "  focus_next_tag [occupied]; focus_previous_tag [occupied]\n"
    "  focus_last_tag; focus_previous_view\n"
    "  focus_tag TAG; focus DIRECTION; move DIRECTION [AMOUNT]\n"
    "  move_view_to_tag TAG|next|previous [follow]\n"
    "  focus_output NAME|DIRECTION; move_view_to_output NAME|DIRECTION "
    "[follow]\n"
    "  set_layout LAYOUT; switch_layout; remove_empty_tag TAG\n"
    "  toggle_floating; toggle_sticky; toggle_fullscreen; resize DIRECTION "
    "AMOUNT\n"
    "  close_view; reload_config; mode NAME; cycle_keyboard_layout\n"
    "  toggle_shortcuts_inhibit\n"
    "  scratchpad_send; scratchpad_toggle [NAME]; scratchpad_retrieve\n"
    "A configured named scratchpad may launch its configured program.\n"
    "Read state without writing expressions:\n"
    "  get ROOT [FIELD...]    one value, e.g. get session focused_output name\n"
    "  watch ROOT [FIELD...]  stream changes, e.g. watch tags\n"
    "  ROOT: views, tags, outputs, inputs, session, config, runtime, status\n"
    "Use (launch (list PROGRAM ARG...)) for detached programs, never spawn.\n";

static bool write_buffer(struct timao_runtime *runtime,
                         struct timao_output_writer *writer, int fd,
                         const struct timao_output_buffer *buffer,
                         bool reporting) {
  uint64_t deadline = UINT64_MAX;
  if (reporting) {
    uint64_t now = 0;
    if (timao_runtime_now(&now) != LEME_PUBLIC_OK)
      return false;
    deadline = now > UINT64_MAX - 250 ? UINT64_MAX : now + 250;
  }
  size_t offset = 0;
  for (;;) {
    const enum timao_output_progress progress =
        timao_output_write(writer, buffer, &offset,
                           !reporting && timao_runtime_cancelled(runtime));
    if (progress == TIMAO_OUTPUT_DONE)
      return true;
    if (progress == TIMAO_OUTPUT_ERROR || progress == TIMAO_OUTPUT_CANCELLED) {
      if (!reporting && progress == TIMAO_OUTPUT_ERROR)
        runtime->io_error = true;
      return false;
    }
    if (timao_runtime_poll_fd(runtime, deadline,
                              &(const struct timao_runtime_poll_input){
                                  .fd = fd, .events = POLLOUT}) !=
        TIMAO_RUNTIME_PROGRESS)
      return false;
  }
}

static int access_source(const struct timao_cli_arguments *options,
                         char *buffer, size_t capacity) {
  size_t used = 0;
  int written = snprintf(buffer, capacity, "(%s ",
                         options->kind == TIMAO_CLI_GET ? "query" : "watch");
  if (written < 0 || (size_t)written >= capacity)
    return -1;
  used = (size_t)written;
  for (size_t i = 0; i < options->count; ++i) {
    written = snprintf(buffer + used, capacity - used, "(get ");
    if (written < 0 || (size_t)written >= capacity - used)
      return -1;
    used += (size_t)written;
  }
  written = snprintf(buffer + used, capacity - used, "(%s)", options->command);
  if (written < 0 || (size_t)written >= capacity - used)
    return -1;
  used += (size_t)written;
  for (size_t i = 0; i < options->count; ++i) {
    written = snprintf(buffer + used, capacity - used, " \"%s\")",
                       options->arguments[i]);
    if (written < 0 || (size_t)written >= capacity - used)
      return -1;
    used += (size_t)written;
  }
  written = snprintf(buffer + used, capacity - used, ")");
  if (written < 0 || (size_t)written >= capacity - used)
    return -1;
  return (int)(used + (size_t)written);
}

static int arguments(struct timao_runtime *runtime,
                     const struct timao_cli_arguments *options,
                     struct leme_public_builder **owner,
                     struct leme_public_value **out,
                     struct timao_diagnostic *error) {
  enum leme_public_status status =
      leme_public_builder_create_budget(runtime->account, 33554432, owner);
  if (status == LEME_PUBLIC_OK)
    status = leme_public_array(*owner, options->count, out);
  for (size_t i = 0; status == LEME_PUBLIC_OK && i < options->count; ++i) {
    struct leme_public_value *value = NULL;
    const size_t length = strnlen(options->arguments[i], 33554433);
    if (length > 33554432)
      status = LEME_PUBLIC_LIMIT;
    else
      status = leme_public_string(
          *owner, (struct leme_public_text){options->arguments[i], length},
          false, &value);
    if (status == LEME_PUBLIC_OK)
      status = leme_public_array_set(*owner, *out, i, value);
  }
  if (status == LEME_PUBLIC_OK) {
    const struct leme_public_value *roots[] = {*out};
    status = leme_public_builder_seal(*owner, roots, 1);
  }
  if (status == LEME_PUBLIC_OK)
    return 0;
  timao_error(error,
              status == LEME_PUBLIC_OOM       ? "out_of_memory"
              : status == LEME_PUBLIC_INVALID ? "invalid_argument"
                                              : "resource_limit",
              "unable to copy script arguments");
  return status == LEME_PUBLIC_INVALID ? 2 : 1;
}

bool timao_cli_report(struct timao_runtime *runtime,
                      struct timao_output_writer *writer,
                      const struct timao_diagnostic *error, char *buffer,
                      size_t capacity) {
  struct timao_output_buffer output = {0};
  if (!timao_cli_diagnostic(error, runtime->output_mode, buffer, capacity,
                            &output)) {
    struct timao_diagnostic fallback = {0};
    struct leme_public_text code = {0};
    const struct leme_public_value *remote_code =
        leme_public_get(error->payload, LEME_PUBLIC_TEXT("code"));
    const bool unknown =
        leme_public_as_text(remote_code, &code) == LEME_PUBLIC_OK &&
        ((code.length == 15 && memcmp(code.data, "outcome_unknown", 15) == 0) ||
         (code.length == 22 &&
          memcmp(code.data, "launch_outcome_unknown", 22) == 0));
    timao_error(
        &fallback, unknown ? "outcome_unknown" : error->code,
        "diagnostic details unavailable; earlier effects are not rolled back");
    if (!timao_cli_diagnostic(&fallback, runtime->output_mode, buffer, capacity,
                              &output)) {
      timao_diagnostic_destroy(&fallback);
      return false;
    }
    timao_diagnostic_destroy(&fallback);
  }
  return write_buffer(runtime, writer, runtime->error_fd, &output, true);
}

int timao_cli_main(int argc, char **argv) {
  struct timao_cli_arguments options = {0};
  struct timao_diagnostic error = {0};
  int status = timao_cli_parse(argc, argv, &options, &error);
  struct timao_runtime *runtime = NULL;
  const int input_fd = fcntl(0, F_GETFD) >= 0 ? 0 : -1;
  const int output_fd = fcntl(1, F_GETFD) >= 0 ? 1 : -1;
  const int error_fd = fcntl(2, F_GETFD) >= 0 ? 2 : -1;
  const struct timao_runtime_options configuration = {
      .endpoint = {.explicit_path = options.socket,
                   .environment_path = getenv("LEME_SOCKET"),
                   .runtime_dir = getenv("XDG_RUNTIME_DIR"),
                   .display = getenv("WAYLAND_DISPLAY")},
      .output_mode = options.format,
      .output_fd = output_fd,
      .error_fd = error_fd,
      .signals = true,
      .display_results = true};
  if (timao_runtime_create(&configuration, &runtime) != LEME_PUBLIC_OK) {
    timao_diagnostic_destroy(&error);
    return 1;
  }
  struct timao_output_writer *diagnostics = NULL, *information = NULL;
  struct leme_public_builder *argv_owner = NULL;
  struct leme_public_value *values = NULL;
  struct timao_cli_source source = {0};
  char emergency[4096] = {0};
  char *reserved = NULL;
  if (timao_output_writer_create(runtime->account, runtime->error_fd,
                                 &diagnostics) != LEME_PUBLIC_OK) {
    status = 1;
    goto done;
  }
  if (status != 0)
    goto done;
  if (options.kind == TIMAO_CLI_HELP || options.kind == TIMAO_CLI_VERSION) {
    static const char version[] = "timao " LEME_VERSION "\n";
    const struct timao_output_buffer output = {
        .data = options.kind == TIMAO_CLI_HELP ? help : version,
        .length = options.kind == TIMAO_CLI_HELP ? sizeof(help) - 1
                                                 : sizeof(version) - 1};
    if (timao_output_writer_create(runtime->account, runtime->output_fd,
                                   &information) != LEME_PUBLIC_OK ||
        !write_buffer(runtime, information, runtime->output_fd, &output,
                      false)) {
      timao_error(&error, "io_error", "unable to write requested information");
      status = timao_runtime_exit_status(runtime, 1);
    }
    goto done;
  }
  reserved = leme_control_alloc(runtime->account, 8388608);
  if (reserved == NULL) {
    timao_error(&error, errno == ENOSPC ? "resource_limit" : "out_of_memory",
                "unable to reserve diagnostic output");
    status = 1;
    goto done;
  }
  if (options.kind == TIMAO_CLI_REPL) {
    status = timao_cli_repl(
        runtime, input_fd,
        &(const struct timao_cli_reporter){
            .writer = diagnostics, .buffer = reserved, .capacity = 8388608},
        &error);
    goto done;
  }
  status = arguments(runtime, &options, &argv_owner, &values, &error);
  if (status != 0)
    goto done;
  char command[128] = {0};
  char access[TIMAO_CLI_FIELDS_MAX * (TIMAO_CLI_FIELD_MAX + 12) + 64] = {0};
  struct timao_input input = {.mode = TIMAO_EXPRESSION,
                              .name = LEME_PUBLIC_TEXT("<eval>")};
  if (options.kind == TIMAO_CLI_COMMAND) {
    const int length = snprintf(command, sizeof(command),
                                "(act (command \"%s\" args))", options.command);
    if (length < 0 || (size_t)length >= sizeof(command)) {
      timao_error(&error, "resource_limit",
                  "command adapter source exceeds limit");
      status = 1;
      goto done;
    }
    input.bytes = (struct leme_public_text){command, (size_t)length};
    input.name = LEME_PUBLIC_TEXT("<command>");
  } else if (options.kind == TIMAO_CLI_GET || options.kind == TIMAO_CLI_WATCH) {
    const int length = access_source(&options, access, sizeof(access));
    if (length < 0) {
      timao_error(&error, "resource_limit", "get/watch source exceeds limit");
      status = 1;
      goto done;
    }
    input.bytes = (struct leme_public_text){access, (size_t)length};
    input.name = LEME_PUBLIC_TEXT("<get>");
  } else if (options.kind == TIMAO_CLI_EVAL) {
    input.bytes = (struct leme_public_text){options.source,
                                            strnlen(options.source, 1048577)};
  } else {
    input.mode = TIMAO_FILE;
    input.name = (struct leme_public_text){options.source,
                                           strnlen(options.source, 1048577)};
    status = timao_cli_source_load(runtime, options.source, input_fd, &source,
                                   &error);
    if (status != 0) {
      timao_diagnostic_input(&error, &input);
      goto done;
    }
    input.bytes = (struct leme_public_text){source.data, source.length};
  }
  status = timao_runtime_execute(runtime, &input, values);
done:
  if (timao_runtime_shutdown(runtime) < 0 && errno != EBUSY && status == 0) {
    status = 1;
    timao_error(&error, "io_error", "runtime shutdown failed");
  }
  status = timao_runtime_exit_status(runtime, status);
  if (status != 0 && diagnostics != NULL) {
    const struct timao_diagnostic *diagnostic =
        error.code[0] != '\0' ? &error : timao_runtime_diagnostic(runtime);
    if (diagnostic->code[0] == '\0') {
      timao_error(&error, "resource_limit", "CLI operation failed");
      diagnostic = &error;
    }
    if (!timao_cli_report(runtime, diagnostics, diagnostic,
                          reserved != NULL ? reserved : emergency,
                          reserved != NULL ? 8388608 : sizeof(emergency)) &&
        status != 130 && status != 143)
      status = 1;
  }
  timao_cli_source_destroy(&source);
  leme_public_builder_destroy(argv_owner);
  leme_control_free(reserved);
  timao_diagnostic_destroy(&error);
  if (timao_output_writer_destroy(information) < 0 && status == 0)
    status = 1;
  if (timao_output_writer_destroy(diagnostics) < 0 && status == 0)
    status = 1;
  status = timao_runtime_exit_status(runtime, status);
  if (timao_runtime_destroy(runtime) < 0) {
    const int failure = errno;
    if (status == 0)
      status = 1;
    if (failure == EBUSY)
      _Exit(status);
  }
  return status;
}
