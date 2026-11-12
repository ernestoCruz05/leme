#ifndef LEME_SHELL_CONTROL_H
#define LEME_SHELL_CONTROL_H

#include "control/action.h"
#include "control/error.h"

#include <stdbool.h>

struct leme_server;

struct leme_control_domain
leme_server_control_domain(struct leme_server *server);
bool leme_server_control_act(struct leme_server *server, const char *source);

#endif
