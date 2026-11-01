#ifndef LEME_CONTROL_LIMITS_H
#define LEME_CONTROL_LIMITS_H

#include "control/error.h"
#include <stddef.h>
#include <stdint.h>

struct leme_control_limits {
  size_t clients, request_bytes, response_bytes;
  size_t json_depth, expression_depth, expression_nodes;
  size_t outstanding, subscriptions, targets, work_units, output_bytes;
  size_t retained_bytes;
};

#define LEME_CONTROL_WATCH_INTERVAL_NS 16000000ULL
#define LEME_CONTROL_STEP_JOBS 64

struct leme_control_meter {
  size_t remaining;
};

struct leme_control_limits leme_control_limits_default(void);
uint64_t leme_control_now_ns(void *context);

static inline enum leme_control_code
leme_control_charge(struct leme_control_meter *meter, size_t units) {
  if (meter == NULL)
    return LEME_CONTROL_OK;
  if (meter->remaining < units)
    return LEME_CONTROL_RESOURCE_LIMIT;
  meter->remaining -= units;
  return LEME_CONTROL_OK;
}

#endif
