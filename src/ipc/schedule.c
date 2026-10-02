#include "ipc/schedule.h"
#include "control/control.h"
#include "control/limits.h"
#include "control/watch.h"
#include "ipc/connection-internal.h"
#include "ipc/watch-peer.h"
#include "public/model.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <wayland-server-core.h>

struct leme_control_scheduler {
  struct leme_control_context *context;
  struct wl_event_loop *loop;
  struct wl_event_source *timer;
  struct wl_list peers;
  struct wl_list runnable_peers;
  size_t peer_count;
  bool in_step;
  bool armed;
};

static int on_scheduler_timer(void *data) {
  struct leme_control_scheduler *scheduler = data;
  scheduler->armed = false;
  leme_control_scheduler_defer(scheduler);
  leme_control_scheduler_step(scheduler);
  return 0;
}

static void close_peers(struct leme_control_scheduler *scheduler) {
  struct leme_control_peer *peer = NULL;
  wl_list_for_each(peer, &scheduler->peers, link) leme_control_peer_close(peer);
}

static int paced_delay_ms(const struct leme_control_scheduler *scheduler) {
  const struct leme_control_peer *peer = NULL;
  uint64_t wake = 0;

  wl_list_for_each(peer, &scheduler->peers, link) {
    const uint64_t peer_wake = leme_control_peer_watch_wake_ns(peer);

    if (peer_wake != 0 && (wake == 0 || peer_wake < wake))
      wake = peer_wake;
  }
  if (wake == 0)
    return 0;
  const uint64_t now = leme_control_now_ns(NULL);
  const uint64_t remaining = wake > now ? wake - now : 0;
  return (int)(remaining / 1000000ULL) + 1;
}

static void arm(struct leme_control_scheduler *scheduler) {
  if (scheduler->armed || scheduler->in_step)
    return;
  const int delay =
      wl_list_empty(&scheduler->runnable_peers) ? paced_delay_ms(scheduler) : 1;
  if (delay == 0)
    return;
  if (scheduler->timer == NULL ||
      wl_event_source_timer_update(scheduler->timer, delay) != 0) {
    close_peers(scheduler);
    return;
  }
  scheduler->armed = true;
}

static void observe(void *context, enum leme_public_change change) {
  struct leme_control_scheduler *scheduler = context;
  struct leme_control_peer *peer = NULL;
  wl_list_for_each(peer, &scheduler->peers, link)
      leme_control_watch_peer_notify(peer, change);
  leme_control_scheduler_defer(scheduler);
}

struct leme_control_scheduler *
leme_control_scheduler_create(struct leme_control_context *context,
                              struct wl_event_loop *loop) {
  if (context == NULL || loop == NULL)
    return NULL;
  struct leme_control_scheduler *scheduler = calloc(1, sizeof(*scheduler));
  if (scheduler == NULL)
    return NULL;
  scheduler->context = context;
  scheduler->loop = loop;
  wl_list_init(&scheduler->peers);
  wl_list_init(&scheduler->runnable_peers);
  leme_public_model_set_observer(leme_control_context_model(context), observe,
                                 scheduler);
  return scheduler;
}

void leme_control_scheduler_destroy(struct leme_control_scheduler *scheduler) {
  if (scheduler == NULL)
    return;
  leme_public_model_set_observer(leme_control_context_model(scheduler->context),
                                 NULL, NULL);
  if (scheduler->timer != NULL && wl_event_source_remove(scheduler->timer) != 0)
    abort();
  free(scheduler);
}

bool leme_control_scheduler_add_peer(struct leme_control_scheduler *scheduler,
                                     struct leme_control_peer *peer) {
  if (scheduler == NULL || peer == NULL)
    return false;
  if (scheduler->timer == NULL) {
    scheduler->timer =
        wl_event_loop_add_timer(scheduler->loop, on_scheduler_timer, scheduler);
    if (scheduler->timer == NULL)
      return false;
  }
  wl_list_insert(&scheduler->peers, &peer->link);
  ++scheduler->peer_count;
  return true;
}

void leme_control_scheduler_remove_peer(
    struct leme_control_scheduler *scheduler, struct leme_control_peer *peer) {
  if (scheduler == NULL || peer == NULL)
    return;
  if (peer->in_runnable) {
    wl_list_remove(&peer->runnable_link);
    wl_list_init(&peer->runnable_link);
    peer->in_runnable = false;
  }
  wl_list_remove(&peer->link);
  wl_list_init(&peer->link);
  if (scheduler->peer_count > 0)
    --scheduler->peer_count;
  if (scheduler->peer_count == 0 && scheduler->timer != NULL) {
    if (wl_event_source_remove(scheduler->timer) != 0)
      abort();
    scheduler->timer = NULL;
    scheduler->armed = false;
  }
}

void leme_control_scheduler_defer(struct leme_control_scheduler *scheduler) {
  if (scheduler == NULL)
    return;
  struct leme_control_peer *peer = NULL;
  wl_list_for_each(peer, &scheduler->peers, link) {
    if (!peer->in_runnable && leme_control_peer_has_work(peer)) {
      wl_list_insert(scheduler->runnable_peers.prev, &peer->runnable_link);
      peer->in_runnable = true;
    }
  }
  arm(scheduler);
}

void leme_control_scheduler_peer_ready(struct leme_control_scheduler *scheduler,
                                       struct leme_control_peer *peer) {
  if (scheduler == NULL || peer == NULL ||
      peer->state == LEME_PEER_STATE_CLOSED)
    return;
  if (!peer->in_runnable) {
    wl_list_insert(scheduler->runnable_peers.prev, &peer->runnable_link);
    peer->in_runnable = true;
  }
  if (!scheduler->in_step) {
    const struct leme_control_peer *next =
        wl_container_of(scheduler->runnable_peers.next, next, runnable_link);
    if (leme_control_peer_needs_fresh_turn(next))
      arm(scheduler);
    else
      leme_control_scheduler_step(scheduler);
  }
}

void leme_control_scheduler_step(struct leme_control_scheduler *scheduler) {
  if (scheduler == NULL || scheduler->in_step)
    return;
  if (scheduler->armed) {
    scheduler->armed = false;
    if (wl_event_source_timer_update(scheduler->timer, 0) != 0) {
      close_peers(scheduler);
      return;
    }
  }
  scheduler->in_step = true;
  const struct leme_control_limits *limits =
      leme_control_context_limits(scheduler->context);
  const uint64_t duration = limits != NULL ? limits->deadline_ns : 5000000ULL;
  const uint64_t start = leme_control_now_ns(NULL);
  const uint64_t previous = leme_control_context_deadline(scheduler->context);
  uint64_t deadline =
      duration > UINT64_MAX - start ? UINT64_MAX : start + duration;
  if (previous != 0 && previous < deadline)
    deadline = previous;
  leme_control_context_set_deadline(scheduler->context, deadline);
  bool serviced = false;
  while (!wl_list_empty(&scheduler->runnable_peers)) {
    if (leme_control_now_ns(NULL) >= deadline)
      break;
    struct leme_control_peer *peer =
        wl_container_of(scheduler->runnable_peers.next, peer, runnable_link);
    if (serviced && leme_control_peer_needs_fresh_turn(peer))
      break;
    wl_list_remove(&peer->runnable_link);
    wl_list_init(&peer->runnable_link);
    peer->in_runnable = false;
    leme_control_peer_step(peer);
    serviced = true;
    if (leme_control_peer_has_work(peer) && !peer->in_runnable) {
      wl_list_insert(scheduler->runnable_peers.prev, &peer->runnable_link);
      peer->in_runnable = true;
    }
  }
  scheduler->in_step = false;
  leme_control_context_set_deadline(scheduler->context, previous);
  arm(scheduler);
}
