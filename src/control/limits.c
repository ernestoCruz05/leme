#include "control/limits.h"

#include <time.h>

uint64_t leme_control_now_ns(void *context) {
  (void)context;
  struct timespec ts = {0};
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return UINT64_MAX;
  }
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

struct leme_control_limits leme_control_limits_default(void) {
  return (struct leme_control_limits){
      .clients = 16,
      .request_bytes = 65536,
      .response_bytes = 1048576,
      .json_depth = 64,
      .expression_depth = 64,
      .expression_nodes = 4096,
      .field_depth = 16,
      .outstanding = 16,
      .subscriptions = 32,
      .targets = 256,
      .work_units = 100000,
      .output_bytes = 2097152,
      .retained_bytes = 8388608,
      .snapshot_bytes = 33554432,
      .total_bytes = 67108864,
      .deadline_ns = 5000000,
  };
}
