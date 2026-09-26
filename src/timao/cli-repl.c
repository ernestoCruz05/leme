#include "timao/cli-internal.h"
#include "timao/repl.h"
#include <poll.h>
#include <signal.h>

struct editor_context {
  struct timao_repl_editor *editor;
  bool editing;
};

static enum timao_status notify_output(void *opaque, bool before,
                                       struct timao_diagnostic *error) {
  struct editor_context *context = opaque;
  if (!context->editing)
    return TIMAO_OK;
  return before ? timao_repl_editor_hide(context->editor, error)
                : timao_repl_editor_redraw(context->editor, error);
}

static void acknowledge_interrupt(struct timao_runtime *runtime) {
  timao_runtime_signal_clear_interrupt(runtime->signals);
  runtime->caught_signal = 0;
  runtime->cancelled = false;
}

static int contained(struct timao_runtime *runtime,
                     struct timao_repl_editor *editor,
                     const struct timao_cli_reporter *reporter,
                     struct timao_diagnostic *error) {
  struct timao_diagnostic *diagnostic = &runtime->diagnostic;
  if (runtime->io_error || runtime->output_incomplete ||
      runtime->caught_signal == SIGTERM)
    return timao_runtime_exit_status(runtime, 1);
  const bool interrupted = runtime->caught_signal == SIGINT;
  if (interrupted)
    acknowledge_interrupt(runtime);
  if (timao_repl_editor_hide(editor, NULL) != TIMAO_OK)
    return timao_runtime_exit_status(runtime, 1);
  if (!timao_cli_report(runtime, reporter->writer, diagnostic, reporter->buffer,
                        reporter->capacity))
    return timao_runtime_exit_status(runtime, 1);
  timao_diagnostic_destroy(diagnostic);
  if (interrupted && timao_repl_editor_interrupt(editor, error) != TIMAO_OK)
    return timao_runtime_exit_status(runtime, 1);
  return timao_repl_editor_redraw(editor, error) == TIMAO_OK
             ? 0
             : timao_runtime_exit_status(runtime, 1);
}

int timao_cli_repl(struct timao_runtime *runtime, int input_fd,
                   const struct timao_cli_reporter *reporter,
                   struct timao_diagnostic *error) {
  struct timao_repl_editor *editor = NULL;
  if (timao_repl_editor_create(runtime, input_fd, &editor, error) != TIMAO_OK)
    return timao_runtime_exit_status(runtime, 1);
  runtime->repl = true;
  struct editor_context context = {.editor = editor, .editing = true};
  runtime->output_context = &context;
  runtime->output_notify = notify_output;
  int status = 0;
  for (;;) {
    if (timao_runtime_cancelled(runtime)) {
      if (runtime->io_error || runtime->caught_signal == SIGTERM) {
        status = timao_runtime_exit_status(runtime, 1);
        break;
      }
      acknowledge_interrupt(runtime);
      if (timao_repl_editor_interrupt(editor, error) != TIMAO_OK) {
        status = timao_runtime_exit_status(runtime, 1);
        break;
      }
    }
    if (timao_runtime_dispatch(runtime) != 0) {
      status = contained(runtime, editor, reporter, error);
      if (status != 0)
        break;
    }
    struct leme_public_text form = {0};
    const enum timao_repl_progress progress =
        timao_repl_editor_step(editor, &form, error);
    if (progress == TIMAO_REPL_END)
      break;
    if (progress == TIMAO_REPL_ERROR) {
      if (runtime->caught_signal == SIGINT && !runtime->io_error) {
        timao_diagnostic_destroy(error);
        continue;
      }
      status = timao_runtime_exit_status(runtime, 1);
      break;
    }
    if (progress == TIMAO_REPL_FORM) {
      timao_diagnostic_destroy(error);
      const struct timao_input input = {.bytes = form,
                                        .name = LEME_PUBLIC_TEXT("<repl>"),
                                        .mode = TIMAO_REPL};
      context.editing = false;
      status = timao_runtime_execute(runtime, &input, NULL);
      context.editing = true;
      timao_repl_editor_consume(editor);
      if (status != 0)
        status = contained(runtime, editor, reporter, error);
      if (status != 0)
        break;
      if (timao_repl_editor_redraw(editor, error) != TIMAO_OK) {
        status = timao_runtime_exit_status(runtime, 1);
        break;
      }
      continue;
    }
    const enum timao_runtime_wait waited =
        timao_runtime_poll_fd(runtime, UINT64_MAX,
                              &(const struct timao_runtime_poll_input){
                                  .fd = input_fd, .events = POLLIN});
    if (waited == TIMAO_RUNTIME_IO_ERROR) {
      timao_error(error, "io_error", "REPL input wait failed");
      status = 1;
      break;
    }
  }
  runtime->output_notify = NULL;
  runtime->output_context = NULL;
  if (timao_repl_editor_destroy(editor) < 0 && status == 0) {
    timao_error(error, "io_error", "unable to restore terminal state");
    status = 1;
  }
  return status;
}
