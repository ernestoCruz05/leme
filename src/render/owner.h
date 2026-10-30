#ifndef LEME_RENDER_OWNER_H
#define LEME_RENDER_OWNER_H

struct leme_view;
struct leme_container;

enum leme_render_owner_kind {
  LEME_RENDER_OWNER_VIEW,
  LEME_RENDER_OWNER_CONTAINER,
};

struct leme_render_owner {
  enum leme_render_owner_kind kind;
  union {
    struct leme_view *view;
    struct leme_container *container;
  } value;
};

#endif
