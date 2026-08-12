#ifndef LEME_OUTPUT_POWER_H
#define LEME_OUTPUT_POWER_H

#include <stdbool.h>

struct leme_server;

bool leme_output_power_init(struct leme_server *server);
void leme_output_power_finish(struct leme_server *server);

#endif
