#ifndef LEME_CONTROL_DISPATCH_H
#define LEME_CONTROL_DISPATCH_H

#include "control/control.h"
#include "control/error.h"
#include "control/request.h"
#include "ipc/frame.h"

struct leme_control_peer;

enum leme_control_code
leme_control_dispatch_request(struct leme_control_context *context,
                              struct leme_control_peer *peer,
                              const struct leme_control_request *request,
                              struct leme_control_frame **out_frame);

#endif
