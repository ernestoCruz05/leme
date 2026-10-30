#ifndef LEME_DESK_OPEN_H
#define LEME_DESK_OPEN_H

#include <stdbool.h>
#include <stddef.h>

struct leme_server;

bool leme_desk_open_path(struct leme_server *server, const char *path);
char **leme_desk_open_expand(char *const *argv, const char *directory,
                             char *const *names, size_t count);

#endif
