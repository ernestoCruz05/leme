#ifndef TIMAO_LAUNCH_H
#define TIMAO_LAUNCH_H
#include "public/budget.h"
#include <signal.h>
#include <stdint.h>
#include <sys/types.h>

struct timao_launch;
struct timao_launch_options {
  uint64_t now_ms;
  const sigset_t *child_mask;
};
enum timao_launch_state {
  TIMAO_LAUNCH_PENDING,
  TIMAO_LAUNCH_ACCEPTED,
  TIMAO_LAUNCH_FAILED,
  TIMAO_LAUNCH_UNKNOWN
};
enum leme_public_status timao_launch_start(
    struct leme_public_budget *account, const struct leme_public_value *argv,
    const struct timao_launch_options *options, struct timao_launch **out);
int timao_launch_fd(const struct timao_launch *launch);
pid_t timao_launch_helper(const struct timao_launch *launch);
int timao_launch_error(const struct timao_launch *launch);
enum timao_launch_state timao_launch_step(struct timao_launch *launch,
                                          uint64_t now_ms, bool cancelled);
bool timao_launch_destroy(struct timao_launch *launch);
#endif
