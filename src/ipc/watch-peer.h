#ifndef LEME_IPC_WATCH_PEER_H
#define LEME_IPC_WATCH_PEER_H

#include "control/request.h"
#include "ipc/frame.h"
#include "public/model.h"

struct leme_control_peer;

enum leme_control_code
leme_control_watch_peer_open(struct leme_control_peer *peer);
enum leme_control_code
leme_control_watch_peer_request(struct leme_control_peer *peer,
                                struct leme_control_request *request, int slot);

void leme_control_watch_peer_step(struct leme_control_peer *peer);
void leme_control_watch_peer_notify(struct leme_control_peer *peer,
                                    enum leme_public_change change);

#endif
