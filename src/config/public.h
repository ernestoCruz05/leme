#ifndef LEME_CONFIG_PUBLIC_H
#define LEME_CONFIG_PUBLIC_H

#include "public/value.h"

struct leme_server;
enum leme_public_status
leme_config_public_capture(struct leme_public_builder *b,
                           const struct leme_server *server,
                           struct leme_public_value **out);

#endif
