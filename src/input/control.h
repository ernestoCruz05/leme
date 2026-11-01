#ifndef LEME_INPUT_CONTROL_H
#define LEME_INPUT_CONTROL_H

#include "control/action.h"
#include "control/error.h"

#include <stdbool.h>

struct leme_server;

enum leme_control_code
leme_input_control_run(struct leme_server *server,
                       const struct leme_control_call *call, bool *applied,
                       struct leme_control_error *error);

#endif
