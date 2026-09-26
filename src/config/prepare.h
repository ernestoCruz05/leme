#ifndef LEME_CONFIG_PREPARE_H
#define LEME_CONFIG_PREPARE_H

#include "control/action.h"
#include "control/error.h"
#include "public/budget.h"

#include <stdbool.h>
#include <stddef.h>

struct leme_server;
struct leme_config;
struct leme_config_reload;

enum leme_control_code
leme_config_reload_prepare(struct leme_server *server, struct leme_config *next,
                           struct leme_public_budget *account,
                           struct leme_config_reload **out,
                           struct leme_control_error *error);

void leme_config_reload_commit(struct leme_server *server,
                               struct leme_config_reload *plan);

void leme_config_reload_discard(struct leme_config_reload **plan);

#endif
