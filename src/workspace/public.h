#ifndef LEME_WORKSPACE_PUBLIC_H
#define LEME_WORKSPACE_PUBLIC_H

#include "public/value.h"
#include "workspace/layout.h"

struct leme_server;
const char *leme_layout_public_name(enum leme_layout_kind kind);
enum leme_public_status
leme_tags_public_capture(struct leme_public_builder *b,
                         const struct leme_server *server,
                         struct leme_public_value **out);

#endif
