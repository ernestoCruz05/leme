#ifndef LEME_PUBLICATION_H
#define LEME_PUBLICATION_H

#include <stdbool.h>

struct leme_server;

bool leme_publication_init(struct leme_server *server);
void leme_publication_finish(struct leme_server *server);
void leme_publication_invalidate(struct leme_server *server);

#endif
