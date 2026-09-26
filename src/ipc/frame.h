#ifndef LEME_IPC_FRAME_H
#define LEME_IPC_FRAME_H

#include "control/error.h"
#include "public/budget.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct leme_control_frame;
struct leme_control_framer;

enum leme_control_code
leme_control_frame_create(struct leme_public_budget *account,
                          const char *bytes, size_t length,
                          struct leme_control_frame **out);

enum leme_control_code
leme_control_frame_create_capacity(struct leme_public_budget *account,
                                   size_t capacity,
                                   struct leme_control_frame **out);

void leme_control_frame_destroy(struct leme_control_frame *frame);

const char *leme_control_frame_bytes(const struct leme_control_frame *frame);
char *leme_control_frame_buffer(struct leme_control_frame *frame,
                                size_t *out_capacity);
size_t leme_control_frame_length(const struct leme_control_frame *frame);
void leme_control_frame_set_length(struct leme_control_frame *frame,
                                   size_t length);

enum leme_control_frame_kind {
  LEME_CONTROL_FRAME_OTHER = 0,
  LEME_CONTROL_FRAME_QUERY = 1,
  LEME_CONTROL_FRAME_ACTION = 2,
  LEME_CONTROL_FRAME_EVENT = 3,
  LEME_CONTROL_FRAME_TERMINAL_EVENT = 4,
};

enum leme_control_watch_discard {
  LEME_WATCH_DISCARD_CANCEL,
  LEME_WATCH_DISCARD_LOCK
};

struct leme_control_watch_disposal {
  uint64_t subscription;
  enum leme_control_watch_discard reason;
};

void leme_control_frame_set_subscription(struct leme_control_frame *frame,
                                         uint64_t subscription);
uint64_t
leme_control_frame_subscription(const struct leme_control_frame *frame);

void leme_control_frame_set_metadata(
    struct leme_control_frame *frame,
    enum leme_control_frame_kind kind,
    bool sensitive,
    const char *id,
    size_t id_len);

enum leme_control_frame_kind
leme_control_frame_get_kind(const struct leme_control_frame *frame);

bool leme_control_frame_is_sensitive(const struct leme_control_frame *frame);

const char *leme_control_frame_id(const struct leme_control_frame *frame,
                                  size_t *out_len);

enum leme_control_code
leme_control_framer_create(struct leme_public_budget *account,
                           size_t max_record_bytes,
                           struct leme_control_framer **out);

void leme_control_framer_destroy(struct leme_control_framer *framer);

enum leme_control_code leme_control_framer_feed(
    struct leme_control_framer *framer, const char *bytes, size_t length,
    size_t *consumed, struct leme_control_frame **out);

bool leme_control_framer_is_empty(const struct leme_control_framer *framer);
size_t
leme_control_framer_pending_bytes(const struct leme_control_framer *framer);

#endif
