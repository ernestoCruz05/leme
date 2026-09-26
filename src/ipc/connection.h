#ifndef LEME_IPC_CONNECTION_H
#define LEME_IPC_CONNECTION_H

#include "control/control.h"
#include "control/error.h"

#include <stdbool.h>
#include <stddef.h>

struct wl_event_loop;
struct leme_control_peer;

enum leme_control_code leme_control_peer_open(
    struct leme_control_context *context,
    struct wl_event_loop *loop,
    int fd,
    struct leme_control_peer **out);

void leme_control_peer_destroy(struct leme_control_peer *peer);

void leme_control_peer_ready(struct leme_control_peer *peer);

int leme_control_peer_fd(const struct leme_control_peer *peer);

enum leme_control_code
leme_control_peer_reserve_reply(struct leme_control_peer *peer, size_t bytes);

#endif
