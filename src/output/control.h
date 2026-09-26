#ifndef LEME_OUTPUT_CONTROL_H
#define LEME_OUTPUT_CONTROL_H

#include "control/action.h"
#include "control/error.h"

#include <stdbool.h>
#include <stddef.h>

struct leme_server;
struct leme_public_budget;
struct wlr_output_configuration_v1;

enum leme_control_code leme_output_control_prepare(
    struct leme_server *server, const struct leme_control_intent *intents,
    size_t count, struct leme_public_budget *account,
    struct leme_control_prepared **out, struct leme_control_error *error);

enum leme_control_code leme_output_control_execute_one(
    struct leme_server *server, struct leme_control_prepared *prepared,
    size_t index, enum leme_control_outcome *outcome,
    struct leme_control_error *error);

void leme_output_control_discard(struct leme_server *server,
                                 struct leme_control_prepared *prepared);

bool leme_output_control_test_configuration(
    struct leme_server *server,
    struct wlr_output_configuration_v1 *configuration);

bool leme_output_control_commit_configuration(
    struct leme_server *server,
    struct wlr_output_configuration_v1 *configuration);

bool leme_output_control_configuration_matches_current(
    struct leme_server *server,
    const struct wlr_output_configuration_v1 *configuration);

bool leme_output_control_heads_overlap(
    const struct wlr_output_configuration_v1 *configuration);

#endif
