#ifndef LEME_OVERVIEW_H
#define LEME_OVERVIEW_H

#include "core/command.h"

#include <stdbool.h>
#include <stdint.h>

struct leme_output;
struct leme_server;

void leme_overview_toggle(struct leme_server *server);
void leme_overview_close(struct leme_server *server);
void leme_overview_act(struct leme_server *server,
                       enum leme_overview_action action);
bool leme_overview_go(struct leme_server *server, uint16_t id);
void leme_overview_pick(struct leme_server *server, uint16_t id, bool swap);
bool leme_overview_hides(const struct leme_output *output);
void leme_overview_refresh(struct leme_output *output);

#endif
