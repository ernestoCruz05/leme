#ifndef LEME_IPC_CONNECTION_INTERNAL_H
#define LEME_IPC_CONNECTION_INTERNAL_H

#include "control/error.h"
#include "ipc/frame.h"
#include "public/budget.h"

#include <stdbool.h>
#include <stddef.h>

struct leme_control_queue_node {
  struct leme_control_frame *frame;
  size_t offset;
  struct leme_control_queue_node *next;
};

typedef void (*leme_control_queue_completion_fn)(
    struct leme_control_frame *frame, void *user_data);

struct leme_control_queue {
  struct leme_public_budget *account;
  size_t max_bytes;
  size_t queued_bytes;
  size_t reserved_bytes;
  struct leme_control_queue_node *reserved_node;
  struct leme_control_queue_node *head;
  struct leme_control_queue_node *tail;
  leme_control_queue_completion_fn completion_cb;
  void *completion_data;
};

enum leme_control_flush_status {
  LEME_CONTROL_FLUSH_OK,
  LEME_CONTROL_FLUSH_AGAIN,
  LEME_CONTROL_FLUSH_CLOSED,
  LEME_CONTROL_FLUSH_ERROR
};

enum leme_control_code
leme_control_queue_create(struct leme_public_budget *account, size_t max_bytes,
                          struct leme_control_queue **out);

void leme_control_queue_destroy(struct leme_control_queue *queue);

enum leme_control_code
leme_control_queue_reserve(struct leme_control_queue *queue, size_t bytes);

void leme_control_queue_cancel_reservation(struct leme_control_queue *queue,
                                           size_t bytes);

enum leme_control_code
leme_control_queue_push(struct leme_control_queue *queue,
                        struct leme_control_frame *frame);

enum leme_control_code
leme_control_queue_push_batch(struct leme_control_queue *queue,
                              struct leme_control_frame **frames, size_t count);
bool leme_control_queue_discard_watch(
    struct leme_control_queue *queue,
    struct leme_control_watch_disposal disposal);

enum leme_control_code
leme_control_queue_push_reserved(struct leme_control_queue *queue,
                                 struct leme_control_frame *frame);

enum leme_control_flush_status
leme_control_queue_flush(struct leme_control_queue *queue, int fd,
                         size_t *written);

size_t leme_control_queue_bytes(const struct leme_control_queue *queue);
bool leme_control_queue_is_empty(const struct leme_control_queue *queue);

void leme_control_queue_set_completion(
    struct leme_control_queue *queue, leme_control_queue_completion_fn callback,
    void *user_data);

#include "control/control.h"
#include "control/limits.h"
#include <wayland-server-core.h>

enum leme_control_peer_state {
  LEME_PEER_STATE_NEGOTIATING = 0,
  LEME_PEER_STATE_READY = 1,
  LEME_PEER_STATE_DRAINING = 2,
  LEME_PEER_STATE_CLOSED = 3,
};

struct leme_control_outstanding {
  bool active;
  char id[129];
  size_t id_len;
  struct leme_control_frame *frame;
};

struct leme_control_watch_set;

struct leme_control_peer {
  struct leme_control_context *context;
  struct wl_event_loop *loop;
  struct wl_event_source *source;
  int fd;
  struct leme_public_budget *account;
  struct leme_control_limits limits;
  struct leme_control_framer *framer;
  struct leme_control_queue *queue;
  struct leme_control_scheduler *scheduler;
  struct leme_control_watch_set *watches;
  int watch_reply_slot;
  bool watch_next;
  enum leme_control_peer_state state;
  bool negotiated;
  struct leme_control_outstanding outstanding[16];
  size_t outstanding_count;
  struct wl_list link;
  struct wl_list runnable_link;
  bool in_runnable;
  struct wl_list pending_requests;
  char *read_buffer;
  size_t read_length;
  size_t read_capacity;
};

void leme_control_peer_close(struct leme_control_peer *peer);
void leme_control_peer_step(struct leme_control_peer *peer);
bool leme_control_peer_has_work(const struct leme_control_peer *peer);
bool leme_control_peer_needs_fresh_turn(const struct leme_control_peer *peer);

#endif
