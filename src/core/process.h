#ifndef LEME_PROCESS_H
#define LEME_PROCESS_H

#include <stdbool.h>

struct leme_server;

bool leme_process_spawn_detached(struct leme_server *server, char *const *argv);

#endif
