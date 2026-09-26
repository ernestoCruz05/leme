#ifndef LEME_CONTROL_WATCH_INTERNAL_H
#define LEME_CONTROL_WATCH_INTERNAL_H

#include "control/watch.h"
#include "control/eval.h"

#define LEME_CONTROL_WATCH_MAX 32

enum leme_control_watch_state {
  LEME_WATCH_REMOVED,
  LEME_WATCH_ARMING,
  LEME_WATCH_ACTIVE,
  LEME_WATCH_SUSPENDED_LOCKED
};

struct leme_control_watch_slot {
  struct leme_control_request *request;
  struct leme_control_program *program;
  struct leme_control_evaluation *baseline;
  uint64_t subscription;
  uint64_t sequence;
  uint64_t dirty_epoch;
  uint32_t roots;
  enum leme_control_watch_state state;
  bool busy;
  bool dirty;
  bool sensitive;
  bool reset;
  bool suspension_pending;
};

struct leme_control_watch_set {
  struct leme_public_budget *account;
  struct leme_control_limits limits;
  struct leme_control_watch_sink sink;
  struct leme_control_watch_slot slots[LEME_CONTROL_WATCH_MAX];
  uint64_t next_subscription;
  uint64_t dirty_epoch;
  uint64_t privacy_epoch;
  size_t count;
  size_t cursor;
  bool closed;
};

#endif
