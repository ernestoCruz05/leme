#ifndef TIMAO_RUNTIME_H
#define TIMAO_RUNTIME_H

#include "timao/endpoint.h"
#include "timao/language.h"
#include "timao/limits.h"
#include "timao/output.h"

struct timao_runtime;
struct timao_runtime_options {
  struct leme_public_budget *account;
  struct timao_endpoint_options endpoint;
  struct timao_limits language_limits;
  size_t total_bytes;
  enum timao_output_mode output_mode;
  int output_fd, error_fd;
  bool signals;
  bool display_results;
};

enum leme_public_status
timao_runtime_create(const struct timao_runtime_options *options,
                     struct timao_runtime **out);
int timao_runtime_shutdown(struct timao_runtime *runtime);
int timao_runtime_destroy(struct timao_runtime *runtime);
int timao_runtime_execute(struct timao_runtime *runtime,
                          const struct timao_input *input,
                          const struct leme_public_value *args);
int timao_runtime_dispatch(struct timao_runtime *runtime);
bool timao_runtime_has_handlers(const struct timao_runtime *runtime);
const struct timao_diagnostic *
timao_runtime_diagnostic(const struct timao_runtime *runtime);

#endif
