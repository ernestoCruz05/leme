#ifndef LEME_INPUT_CONTROL_H
#define LEME_INPUT_CONTROL_H

#include "control/action.h"
#include "control/error.h"
#include "public/budget.h"

#include <stddef.h>

struct leme_server;

enum leme_control_code leme_input_control_prepare(
    struct leme_server *server, const struct leme_control_intent *intents,
    size_t count, struct leme_public_budget *account,
    struct leme_control_prepared **out, struct leme_control_error *error);

enum leme_control_code
leme_input_control_execute_one(struct leme_server *server,
                               struct leme_control_prepared *prepared,
                               size_t index, enum leme_control_outcome *outcome,
                               struct leme_control_error *error);

void leme_input_control_discard(struct leme_server *server,
                                struct leme_control_prepared *prepared);

#endif
