#ifndef LEME_INPUT_PROTOCOL_H
#define LEME_INPUT_PROTOCOL_H

#include <stdbool.h>
#include <stdint.h>

struct leme_server;

bool leme_input_protocols_init(struct leme_server *server);
void leme_input_protocols_finish(struct leme_server *server);
void leme_input_protocols_send_relative(struct leme_server *server,
    uint32_t time_msec, double dx, double dy,
    double unaccel_dx, double unaccel_dy);
bool leme_input_protocols_constrain_motion(struct leme_server *server,
    double *dx, double *dy);
void leme_input_protocols_update_pointer_focus(struct leme_server *server);
void leme_input_protocols_cancel_constraint(struct leme_server *server);
void leme_input_protocols_update_keyboard_focus(struct leme_server *server);
bool leme_input_protocols_shortcuts_inhibited(
    const struct leme_server *server);
bool leme_input_protocols_pointer_locked(const struct leme_server *server);

#endif
