#ifndef LEME_DESK_TARGET_H
#define LEME_DESK_TARGET_H

#include "public/identity.h"
#include "render/render.h"

enum leme_desk_target_kind {
  LEME_DESK_TARGET_VIEW,
  LEME_DESK_TARGET_CONTAINER,
};

struct leme_desk_target {
  enum leme_desk_target_kind kind;
  struct leme_public_id id;
};

struct leme_server;
struct leme_view;
struct leme_container;

struct leme_view *leme_desk_target_view(struct leme_server *server,
                                        struct leme_desk_target target);
struct leme_container *
leme_desk_target_container(struct leme_server *server,
                           struct leme_desk_target target);
struct leme_view *leme_desk_target_member(struct leme_server *server,
                                          struct leme_desk_target target);
void leme_desk_target_act(struct leme_server *server,
                          struct leme_desk_target target,
                          enum leme_render_decoration_part part);

static inline bool leme_desk_target_equal(struct leme_desk_target a,
                                          struct leme_desk_target b) {
  return a.kind == b.kind && a.id.serial == b.id.serial;
}

#endif
