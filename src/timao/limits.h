#ifndef TIMAO_LIMITS_H
#define TIMAO_LIMITS_H

#include "timao/language.h"

struct timao_limits {
  size_t source_bytes;
  size_t nodes;
  size_t nesting;
  size_t call_depth;
  size_t memory_bytes;
  size_t steps;
};

struct timao_meter {
  size_t remaining;
  void *cancel_context;
  bool (*cancelled)(void *context);
  const char *failure;
};

enum timao_status timao_limits_resolve(const struct timao_limits *input,
                                       struct timao_limits *out,
                                       struct timao_diagnostic *error);
void timao_meter_init(struct timao_meter *meter, size_t steps, void *context,
                      bool (*cancelled)(void *context));
enum timao_status timao_charge(struct timao_meter *meter, size_t steps,
                               struct timao_diagnostic *error);

#endif
