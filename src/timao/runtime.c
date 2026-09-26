#include "timao/runtime-internal.h"
#include "control/memory.h"
#include "timao/launch.h"
#include "timao/value.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <time.h>

static enum leme_public_status allocation_status(void) {
  return errno == ENOSPC || errno == EOVERFLOW ? LEME_PUBLIC_LIMIT
                                               : LEME_PUBLIC_OOM;
}

enum leme_public_status
timao_runtime_create(const struct timao_runtime_options *options,
                     struct timao_runtime **out) {
  if (out == NULL)
    return LEME_PUBLIC_INVALID;
  *out = NULL;
  const struct timao_runtime_options defaults = {
      .output_fd = 1, .error_fd = 2, .signals = true};
  const struct timao_runtime_options *chosen =
      options != NULL ? options : &defaults;
  const size_t total =
      chosen->total_bytes != 0 ? chosen->total_bytes : 67108864;
  if (total > 67108864 || chosen->output_mode < TIMAO_OUTPUT_JSON ||
      chosen->output_mode > TIMAO_OUTPUT_HUMAN)
    return LEME_PUBLIC_INVALID;
  struct timao_limits limits = {0};
  struct timao_diagnostic error = {0};
  const enum timao_status validated =
      timao_limits_resolve(&chosen->language_limits, &limits, &error);
  timao_diagnostic_destroy(&error);
  if (validated != TIMAO_OK)
    return LEME_PUBLIC_INVALID;
  struct leme_public_budget *account = NULL;
  enum leme_public_status status =
      chosen->account != NULL
          ? leme_public_budget_child(chosen->account, total, &account)
          : leme_public_budget_create(total, NULL, &account);
  if (status != LEME_PUBLIC_OK)
    return status;
  struct timao_runtime *runtime = leme_control_alloc(account, sizeof(*runtime));
  if (runtime == NULL) {
    status = allocation_status();
    leme_public_budget_unref(account);
    return status;
  }
  *runtime = (struct timao_runtime){.account = account,
                                    .limits = limits,
                                    .output_mode = chosen->output_mode,
                                    .display_results = chosen->display_results,
                                    .output_fd = chosen->output_fd,
                                    .error_fd = chosen->error_fd,
                                    .writing_fd = -1};
  runtime->endpoint_status =
      timao_endpoint_resolve(&chosen->endpoint, &runtime->endpoint);
  status = leme_public_budget_child(account, limits.memory_bytes,
                                    &runtime->language_account);
  if (status == LEME_PUBLIC_OK && chosen->signals)
    status = timao_runtime_signal_create(account, &runtime->signals);
  if (status != LEME_PUBLIC_OK) {
    if (timao_runtime_destroy(runtime) < 0)
      return LEME_PUBLIC_UNAVAILABLE;
    return status;
  }
  *out = runtime;
  return LEME_PUBLIC_OK;
}

int timao_runtime_destroy(struct timao_runtime *runtime) {
  if (runtime == NULL)
    return 0;
  if (runtime->executing || runtime->writing) {
    errno = EBUSY;
    return -1;
  }
  const int stopped = timao_runtime_shutdown(runtime);
  const bool busy = stopped < 0 && errno == EBUSY;
  if (stopped < 0 && !busy)
    runtime->cleanup_error = errno;
  if (!runtime->released) {
    timao_unpin(runtime->vm, runtime->result_pin);
    runtime->result_pin = 0;
    runtime->result = NULL;
    timao_client_destroy(runtime->client);
    runtime->client = NULL;
    if (timao_output_writer_destroy(runtime->output) < 0)
      runtime->cleanup_error = errno;
    runtime->output = NULL;
    if (timao_output_writer_destroy(runtime->descriptions) < 0)
      runtime->cleanup_error = errno;
    runtime->descriptions = NULL;
    timao_vm_destroy(runtime->vm);
    runtime->vm = NULL;
    timao_diagnostic_destroy(&runtime->diagnostic);
    leme_public_budget_unref(runtime->language_account);
    runtime->language_account = NULL;
    if (timao_runtime_signal_destroy(runtime->signals) < 0)
      runtime->cleanup_error = errno;
    runtime->signals = NULL;
    runtime->released = true;
  }
  if (busy) {
    errno = EBUSY;
    return -1;
  }
  const int error = runtime->cleanup_error;
  struct leme_public_budget *account = runtime->account;
  leme_control_free(runtime);
  leme_public_budget_unref(account);
  if (error != 0) {
    errno = error == EBUSY ? EIO : error;
    return -1;
  }
  return 0;
}

bool timao_runtime_cancelled(void *context) {
  struct timao_runtime *runtime = context;
  const int signal = timao_runtime_signal_read(runtime->signals);
  if (signal < 0) {
    runtime->io_error = true;
    return true;
  }
  if (signal != 0) {
    runtime->caught_signal = signal;
    runtime->cancelled = true;
  }
  if (runtime->input_cancelled != NULL &&
      runtime->input_cancelled(runtime->cancel_context))
    runtime->cancelled = true;
  return runtime->cancelled || runtime->io_error;
}

static int failure_status(const struct timao_runtime *runtime, int fallback) {
  if (runtime->io_error)
    return 1;
  if (runtime->caught_signal == SIGTERM)
    return 143;
  if (runtime->cancelled)
    return 130;
  if (fallback == 1 && runtime->diagnostic.payload == NULL &&
      strcmp(runtime->diagnostic.code, "connection_error") == 0)
    return 2;
  return fallback;
}

int timao_runtime_exit_status(struct timao_runtime *runtime, int fallback) {
  if (runtime == NULL)
    return fallback;
  (void)timao_runtime_cancelled(runtime);
  return failure_status(runtime, fallback);
}

int timao_runtime_execute(struct timao_runtime *runtime,
                          const struct timao_input *input,
                          const struct leme_public_value *args) {
  if (runtime == NULL || input == NULL)
    return 2;
  if (runtime->executing || runtime->stopping)
    return 1;
  if (runtime->executed && (!runtime->repl || input->mode != TIMAO_REPL)) {
    timao_error(&runtime->diagnostic, "invalid_argument",
                "runtime invocation already executed");
    return 1;
  }
  if (runtime->vm != NULL && args != NULL) {
    timao_error(&runtime->diagnostic, "invalid_argument",
                "runtime args are immutable");
    return 2;
  }
  runtime->repl = input->mode == TIMAO_REPL;
  runtime->executed = true;
  runtime->executing = true;
  if (runtime->repl && runtime->caught_signal != SIGTERM) {
    timao_runtime_signal_clear_interrupt(runtime->signals);
    runtime->caught_signal = 0;
    runtime->cancelled = false;
  }
  runtime->input_cancelled = input->cancelled;
  runtime->cancel_context = input->cancel_context;
  timao_unpin(runtime->vm, runtime->result_pin);
  runtime->result_pin = 0;
  runtime->result = NULL;
  timao_diagnostic_destroy(&runtime->diagnostic);
  struct timao_input selected = *input;
  selected.cancel_context = runtime;
  selected.cancelled = timao_runtime_cancelled;
  struct timao_program *program = NULL;
  int result = 0;
  if (timao_prepare(runtime->language_account, &runtime->limits, &selected,
                    &program, &runtime->diagnostic) != TIMAO_OK) {
    const bool resource =
        strcmp(runtime->diagnostic.code, "resource_limit") == 0 ||
        strcmp(runtime->diagnostic.code, "out_of_memory") == 0;
    result = failure_status(runtime, resource ? 1 : 2);
    goto done;
  }
  if (runtime->vm == NULL) {
    const struct timao_host host = {.context = runtime,
                                    .cancelled = timao_runtime_cancelled,
                                    .invoke = timao_runtime_host};
    if (timao_vm_create(runtime->language_account, &runtime->limits, &host,
                        args, &runtime->vm, &runtime->diagnostic) != TIMAO_OK) {
      result = failure_status(runtime, 1);
      goto done;
    }
  }
  for (size_t i = 0; i < timao_program_forms(program); ++i) {
    timao_unpin(runtime->vm, runtime->result_pin);
    runtime->result_pin = 0;
    if (timao_eval_form(runtime->vm, program, i, &runtime->result,
                        &runtime->diagnostic) != TIMAO_OK) {
      result = failure_status(runtime, 1);
      break;
    }
    if (timao_pin(runtime->vm, runtime->result, &runtime->result_pin,
                  &runtime->diagnostic) != TIMAO_OK) {
      result = failure_status(runtime, 1);
      break;
    }
    if (runtime->display_results && input->mode != TIMAO_FILE) {
      const enum timao_status shown =
          timao_runtime_show(runtime, runtime->result, &runtime->diagnostic);
      if (shown != TIMAO_OK) {
        timao_diagnostic_input(&runtime->diagnostic, input);
        result = failure_status(runtime, 1);
        break;
      }
    }
    runtime->executing = false;
    const int dispatched = timao_runtime_dispatch(runtime);
    runtime->executing = true;
    if (dispatched != 0) {
      result = failure_status(runtime, 1);
      break;
    }
  }
  while (result == 0 && !runtime->repl && timao_runtime_has_handlers(runtime)) {
    const enum timao_runtime_wait waited =
        timao_runtime_poll(runtime, UINT64_MAX);
    if (waited != TIMAO_RUNTIME_PROGRESS) {
      result = failure_status(runtime, 1);
      break;
    }
    runtime->executing = false;
    const int dispatched = timao_runtime_dispatch(runtime);
    runtime->executing = true;
    if (dispatched != 0)
      result = failure_status(runtime, 1);
  }
done:
  timao_program_unref(program);
  if (timao_runtime_cancelled(runtime)) {
    result = failure_status(runtime, 1);
    if (runtime->diagnostic.code[0] == '\0') {
      timao_error(&runtime->diagnostic,
                  runtime->io_error ? "io_error" : "cancelled",
                  "execution interrupted");
      timao_diagnostic_input(&runtime->diagnostic, input);
    }
  }
  if (!runtime->repl)
    timao_runtime_cancel_all(runtime);
  runtime->executing = false;
  runtime->input_cancelled = NULL;
  runtime->cancel_context = NULL;
  return result;
}

const struct timao_diagnostic *
timao_runtime_diagnostic(const struct timao_runtime *runtime) {
  return runtime != NULL ? &runtime->diagnostic : NULL;
}

enum leme_public_status timao_runtime_now(uint64_t *out) {
  if (out == NULL)
    return LEME_PUBLIC_INVALID;
  *out = 0;
  struct timespec now = {0};
  if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
    return LEME_PUBLIC_UNAVAILABLE;
  if (now.tv_sec < 0 || now.tv_nsec < 0 || now.tv_nsec >= 1000000000 ||
      (uint64_t)now.tv_sec > (UINT64_MAX - 999) / 1000)
    return LEME_PUBLIC_LIMIT;
  *out = (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
  return LEME_PUBLIC_OK;
}

enum timao_runtime_wait timao_runtime_poll(struct timao_runtime *runtime,
                                           uint64_t deadline) {
  return timao_runtime_poll_fd(runtime, deadline, NULL);
}

enum timao_runtime_wait
timao_runtime_poll_fd(struct timao_runtime *runtime, uint64_t deadline,
                      const struct timao_runtime_poll_input *input) {
  if (runtime == NULL)
    return TIMAO_RUNTIME_IO_ERROR;
  if (timao_runtime_cancelled(runtime) && !runtime->stopping)
    return runtime->io_error ? TIMAO_RUNTIME_IO_ERROR : TIMAO_RUNTIME_CANCELLED;
  uint64_t now = 0;
  if (timao_runtime_now(&now) != LEME_PUBLIC_OK) {
    runtime->io_error = true;
    return TIMAO_RUNTIME_IO_ERROR;
  }
  if (now >= deadline)
    return TIMAO_RUNTIME_TIMEOUT;
  timao_transport_step(runtime->transport, 0);
  timao_runtime_launch_tick(runtime, now);
  struct pollfd descriptors[20] = {
      {.fd = timao_runtime_signal_fd(runtime->signals), .events = POLLIN},
      {.fd = timao_transport_fd(runtime->transport),
       .events = timao_transport_events(runtime->transport)},
      {.fd = runtime->writing ? runtime->writing_fd : -1, .events = POLLOUT}};
  for (size_t i = 0; i < 16; ++i)
    descriptors[i + 3] = (struct pollfd){
        .fd = timao_launch_fd(runtime->launches[i]), .events = POLLIN};
  descriptors[19] =
      (struct pollfd){.fd = input != NULL ? input->fd : -1,
                      .events = input != NULL ? input->events : 0};
  const int timeout = deadline - now > 20 ? 20 : (int)(deadline - now);
  const int ready = poll(descriptors, 20, timeout);
  if (ready < 0 && errno != EINTR) {
    runtime->io_error = true;
    return TIMAO_RUNTIME_IO_ERROR;
  }
  if (timao_runtime_cancelled(runtime) && !runtime->stopping)
    return runtime->io_error ? TIMAO_RUNTIME_IO_ERROR : TIMAO_RUNTIME_CANCELLED;
  timao_transport_step(runtime->transport, descriptors[1].revents);
  if (timao_runtime_now(&now) != LEME_PUBLIC_OK) {
    runtime->io_error = true;
    return TIMAO_RUNTIME_IO_ERROR;
  }
  timao_runtime_launch_tick(runtime, now);
  return now >= deadline ? TIMAO_RUNTIME_TIMEOUT : TIMAO_RUNTIME_PROGRESS;
}
