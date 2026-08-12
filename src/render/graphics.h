#ifndef LEME_GRAPHICS_H
#define LEME_GRAPHICS_H

#include <stdbool.h>

struct leme_server;

bool leme_graphics_init(struct leme_server *server);
void leme_graphics_finish(struct leme_server *server);

#endif
