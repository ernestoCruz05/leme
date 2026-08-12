#ifndef LEME_IPC_H
#define LEME_IPC_H

#include <stdbool.h>

struct leme_server;

bool leme_ipc_init(struct leme_server *server);
void leme_ipc_finish(struct leme_server *server);
void leme_ipc_invalidate(struct leme_server *server);
const char *leme_ipc_socket_path(const struct leme_server *server);

#endif
