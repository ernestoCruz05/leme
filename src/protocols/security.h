#ifndef LEME_SECURITY_H
#define LEME_SECURITY_H

#include <stdbool.h>

struct leme_server;

bool leme_security_init(struct leme_server *server);

#endif
