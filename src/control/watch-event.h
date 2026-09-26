#ifndef LEME_CONTROL_WATCH_EVENT_H
#define LEME_CONTROL_WATCH_EVENT_H

#include "control/limits.h"
#include "ipc/frame.h"
#include "public/value.h"

enum leme_control_watch_event_kind {
  LEME_WATCH_SNAPSHOT,
  LEME_WATCH_CHANGE,
  LEME_WATCH_RESET,
  LEME_WATCH_SUSPENDED,
  LEME_WATCH_ERROR,
  LEME_WATCH_EVENT_COUNT
};

struct leme_control_watch_event {
  enum leme_control_watch_event_kind kind;
  uint64_t subscription;
  uint64_t sequence;
  struct leme_public_text instance;
  struct leme_public_text revision;
  const struct leme_public_value *value;
  const struct leme_control_error *error;
  bool sensitive;
};

enum leme_control_code leme_control_watch_event_create(
    struct leme_public_budget *account, size_t maximum,
    const struct leme_control_watch_event *event,
    struct leme_control_meter *meter, struct leme_control_frame **out);

#endif
