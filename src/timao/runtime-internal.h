#ifndef TIMAO_RUNTIME_INTERNAL_H
#define TIMAO_RUNTIME_INTERNAL_H
#include "timao/runtime.h"
#include "timao/runtime-signal.h"
#include "timao/diagnostic.h"
#include "timao/transport.h"
#include "timao/host.h"

struct timao_launch;

struct timao_runtime_watch {
  uint64_t id, handle_pin, handler_pin;
  const struct timao_value *handle, *handler;
  const struct timao_lowered *descriptor;
  bool awaiting, printer;
};

struct timao_runtime {
  struct leme_public_budget *account, *language_account;
  struct timao_limits limits;
  struct timao_endpoint endpoint;
  enum leme_public_status endpoint_status;
  struct timao_vm *vm;
  struct timao_client *client;
  struct timao_transport *transport;
  struct timao_runtime_signal *signals;
  struct timao_output_writer *output, *descriptions;
  struct timao_diagnostic diagnostic;
  const struct timao_value *result;
  uint64_t result_pin, shutdown_deadline;
  int cleanup_error;
  bool stopping, released;
  struct timao_runtime_watch watches[32];
  struct timao_launch *launches[16], *active_launch;
  enum timao_output_mode output_mode;
  int output_fd, error_fd, writing_fd, caught_signal;
  bool executed, executing, writing, repl, cancelled, io_error;
  bool display_results, output_incomplete;
  void *output_context;
  enum timao_status (*output_notify)(void *context, bool before,
                                     struct timao_diagnostic *error);
  void *cancel_context;
  bool (*input_cancelled)(void *context);
};

enum timao_runtime_wait {
  TIMAO_RUNTIME_PROGRESS,
  TIMAO_RUNTIME_TIMEOUT,
  TIMAO_RUNTIME_CANCELLED,
  TIMAO_RUNTIME_IO_ERROR
};

void timao_runtime_launch_tick(struct timao_runtime *runtime, uint64_t now);
enum timao_status timao_runtime_launch_host(struct timao_runtime *runtime,
                                            struct timao_execution *execution,
                                            const struct timao_value *arguments,
                                            const struct timao_value **out,
                                            struct timao_diagnostic *error);
enum timao_status timao_runtime_export(struct timao_runtime *runtime,
                                       struct timao_execution *execution,
                                       const struct timao_value *value,
                                       struct leme_public_builder **owner,
                                       struct leme_public_value **json,
                                       struct timao_diagnostic *error);
void timao_runtime_cancel_all(struct timao_runtime *runtime);
enum timao_status timao_runtime_default_watch(struct timao_runtime *runtime,
                                              uint64_t id,
                                              struct timao_diagnostic *error);
enum timao_status timao_runtime_show(struct timao_runtime *runtime,
                                     const struct timao_value *value,
                                     struct timao_diagnostic *error);
enum timao_status timao_runtime_text(struct timao_runtime *runtime,
                                     struct leme_public_text text,
                                     struct timao_diagnostic *error);
enum timao_status timao_runtime_watch_host(
    struct timao_runtime *runtime, struct timao_execution *execution,
    enum timao_host_op operation, const struct timao_lowered *descriptor,
    const struct timao_value *const *arguments, size_t count,
    const struct timao_value **out, struct timao_diagnostic *error);
enum timao_status timao_runtime_connect(struct timao_runtime *runtime,
                                        struct timao_diagnostic *error);
struct timao_client_message *
timao_runtime_wait_reply(struct timao_runtime *runtime,
                         struct timao_client_ticket ticket);
enum timao_status timao_runtime_reply(struct timao_runtime *runtime,
                                      struct timao_execution *execution,
                                      struct timao_client_message *message,
                                      const struct timao_lowered *descriptor,
                                      const struct timao_value **out,
                                      struct timao_diagnostic *error);
enum timao_status timao_runtime_request(struct timao_runtime *runtime,
                                        struct timao_execution *execution,
                                        enum timao_host_op operation,
                                        const struct timao_lowered *descriptor,
                                        const struct timao_value **out,
                                        struct timao_diagnostic *error);
enum timao_status timao_runtime_host(
    void *context, struct timao_execution *execution,
    enum timao_host_op operation, const struct timao_lowered *descriptor,
    const struct timao_value *const *arguments, size_t count,
    const struct timao_value **out, struct timao_diagnostic *error);
enum timao_status timao_runtime_emit(struct timao_runtime *runtime,
                                     const struct leme_public_value *value,
                                     struct timao_diagnostic *error);
enum leme_public_status timao_runtime_now(uint64_t *out);
bool timao_runtime_cancelled(void *context);
int timao_runtime_exit_status(struct timao_runtime *runtime, int fallback);
struct timao_runtime_poll_input {
  int fd;
  short events;
};
enum timao_runtime_wait
timao_runtime_poll_fd(struct timao_runtime *runtime, uint64_t deadline,
                      const struct timao_runtime_poll_input *input);
enum timao_runtime_wait timao_runtime_poll(struct timao_runtime *runtime,
                                           uint64_t deadline);

#endif
