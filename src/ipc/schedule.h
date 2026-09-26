#ifndef LEME_IPC_SCHEDULE_H
#define LEME_IPC_SCHEDULE_H

#include "control/control.h"
#include "control/error.h"

#include <stdbool.h>
#include <stddef.h>

struct wl_event_loop;
struct leme_control_peer;
struct leme_control_scheduler;

struct leme_control_scheduler *leme_control_scheduler_create(
    struct leme_control_context *context,
    struct wl_event_loop *loop);

void leme_control_scheduler_destroy(struct leme_control_scheduler *scheduler);

bool leme_control_scheduler_add_peer(struct leme_control_scheduler *scheduler,
                                     struct leme_control_peer *peer);

void leme_control_scheduler_remove_peer(struct leme_control_scheduler *scheduler,
                                        struct leme_control_peer *peer);

void leme_control_scheduler_peer_ready(struct leme_control_scheduler *scheduler,
                                       struct leme_control_peer *peer);

void leme_control_scheduler_step(struct leme_control_scheduler *scheduler);
void leme_control_scheduler_defer(struct leme_control_scheduler *scheduler);

#endif
