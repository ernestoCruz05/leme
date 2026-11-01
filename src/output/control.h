#ifndef LEME_OUTPUT_CONTROL_H
#define LEME_OUTPUT_CONTROL_H

#include "control/action.h"
#include "control/error.h"

#include <stdbool.h>
#include <stddef.h>

struct leme_server;
struct wlr_output_configuration_v1;

enum leme_control_code
leme_output_control_run(struct leme_server *server,
                        const struct leme_control_call *call, bool *applied,
                        struct leme_control_error *error);

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
