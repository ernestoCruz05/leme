#ifndef LEME_CONTROL_LIMITS_H
#define LEME_CONTROL_LIMITS_H

#include "control/error.h"
#include <stddef.h>
#include <stdint.h>

struct leme_control_limits {
  size_t clients, request_bytes, response_bytes;
  size_t json_depth, expression_depth, expression_nodes, field_depth;
  size_t outstanding, subscriptions, targets, work_units, output_bytes;
  size_t retained_bytes;
  size_t snapshot_bytes, total_bytes;
  uint64_t deadline_ns;
};

struct leme_control_meter {
  size_t remaining;
  uint64_t deadline_ns;
  void *context;
  uint64_t (*now_ns)(void *context);
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
  if (meter->now_ns != NULL && meter->deadline_ns > 0) {
    if (meter->now_ns(meter->context) >= meter->deadline_ns)
      return LEME_CONTROL_RESOURCE_LIMIT;
  }
  return LEME_CONTROL_OK;
}

#endif
