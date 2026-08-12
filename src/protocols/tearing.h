#ifndef LEME_TEARING_H
#define LEME_TEARING_H

#include <stdbool.h>

struct leme_output;
struct leme_server;

bool leme_tearing_init(struct leme_server *server);
void leme_tearing_finish(struct leme_server *server);
bool leme_tearing_can_tear(const struct leme_server *server,
    struct leme_output *output);

#endif
