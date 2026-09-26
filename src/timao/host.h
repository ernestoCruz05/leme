#ifndef TIMAO_HOST_H
#define TIMAO_HOST_H

#include "timao/language.h"

struct timao_lowered;
enum timao_host_op {
  TIMAO_HOST_QUERY,
  TIMAO_HOST_ACT,
  TIMAO_HOST_WATCH,
  TIMAO_HOST_ON,
  TIMAO_HOST_CANCEL,
  TIMAO_HOST_AWAIT,
  TIMAO_HOST_EMIT,
  TIMAO_HOST_LAUNCH
};

struct timao_vm *timao_execution_vm(const struct timao_execution *execution);
struct leme_public_budget *
timao_execution_account(const struct timao_execution *execution);
enum timao_context
timao_execution_context(const struct timao_execution *execution);
enum timao_status timao_execution_charge(struct timao_execution *execution,
                                         size_t units,
                                         struct timao_diagnostic *error);

struct timao_host {
  void *context;
  bool (*cancelled)(void *context);
  enum timao_status (*invoke)(void *context, struct timao_execution *execution,
                              enum timao_host_op operation,
                              const struct timao_lowered *descriptor,
                              const struct timao_value *const *arguments,
                              size_t count, const struct timao_value **out,
                              struct timao_diagnostic *error);
};

#endif
