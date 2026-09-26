#include "timao/limits.h"
#include "timao/diagnostic.h"

static const struct timao_limits defaults = {
    .source_bytes = 1048576,
    .nodes = 65536,
    .nesting = 128,
    .call_depth = 128,
    .memory_bytes = 33554432,
    .steps = 1000000,
};

enum timao_status timao_limits_resolve(const struct timao_limits *input,
                                       struct timao_limits *out,
                                       struct timao_diagnostic *error) {
  if (out == NULL)
    return timao_error(error, "invalid_argument", "missing limits output");
  struct timao_limits limits = input == NULL ? defaults : *input;
#define LIMIT_FIELD(field)                                                     \
  if (limits.field == 0)                                                       \
    limits.field = defaults.field;                                             \
  if (limits.field > defaults.field)                                           \
  return timao_error(error, "invalid_argument", "limit exceeds hard ceiling")
  LIMIT_FIELD(source_bytes);
  LIMIT_FIELD(nodes);
  LIMIT_FIELD(nesting);
  LIMIT_FIELD(call_depth);
  LIMIT_FIELD(memory_bytes);
  LIMIT_FIELD(steps);
#undef LIMIT_FIELD
  *out = limits;
  return TIMAO_OK;
}

void timao_meter_init(struct timao_meter *meter, size_t steps, void *context,
                      bool (*cancelled)(void *context)) {
  *meter = (struct timao_meter){
      .remaining = steps, .cancel_context = context, .cancelled = cancelled};
}

enum timao_status timao_charge(struct timao_meter *meter, size_t steps,
                               struct timao_diagnostic *error) {
  if (meter == NULL)
    return timao_error(error, "invalid_argument", "missing execution meter");
  if (meter->failure != NULL)
    return timao_error(error, meter->failure, "execution already stopped");
  if (meter->cancelled != NULL && meter->cancelled(meter->cancel_context)) {
    meter->failure = "cancelled";
    return timao_error(error, meter->failure, "execution cancelled");
  }
  if (steps > meter->remaining) {
    meter->failure = "resource_limit";
    return timao_error(error, meter->failure, "evaluation work exhausted");
  }
  meter->remaining -= steps;
  return TIMAO_OK;
}
