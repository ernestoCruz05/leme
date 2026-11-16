#ifndef LEME_OVERVIEW_H
#define LEME_OVERVIEW_H

#include <stdbool.h>

struct leme_output;
struct leme_server;

void leme_overview_toggle(struct leme_server *server);
void leme_overview_close(struct leme_server *server);
bool leme_overview_hides(const struct leme_output *output);
void leme_overview_refresh(struct leme_output *output);

#endif
