#ifndef LEME_WORKSPACE_CONTROL_H
#define LEME_WORKSPACE_CONTROL_H

#include "control/action.h"
#include "control/error.h"

#include <stddef.h>

struct leme_server;
struct leme_public_budget;

enum leme_control_code leme_workspace_control_prepare(
    struct leme_server *server, const struct leme_control_intent *intents,
    size_t count, struct leme_public_budget *account,
    struct leme_control_prepared **out, struct leme_control_error *error);

enum leme_control_code leme_workspace_control_execute_one(
    struct leme_server *server, struct leme_control_prepared *prepared,
    size_t index, enum leme_control_outcome *outcome,
    struct leme_control_error *error);

void leme_workspace_control_discard(struct leme_server *server,
                                    struct leme_control_prepared *prepared);

#endif
