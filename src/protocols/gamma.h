#ifndef LEME_GAMMA_H
#define LEME_GAMMA_H

#include <stdbool.h>

struct leme_output;
struct leme_server;
struct wlr_output_state;

bool leme_gamma_init(struct leme_server *server);
void leme_gamma_finish(struct leme_server *server);
void leme_gamma_apply(struct leme_output *output,
                      struct wlr_output_state *state);

#endif
