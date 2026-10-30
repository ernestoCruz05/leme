#include "desktop/target.h"

#include "core/server.h"
#include "shell/container.h"
#include "shell/view.h"

struct leme_view *leme_desk_target_view(struct leme_server *server,
                                        struct leme_desk_target target) {
  return target.kind == LEME_DESK_TARGET_VIEW
             ? leme_view_by_public_id(server, target.id)
             : NULL;
}

struct leme_container *
leme_desk_target_container(struct leme_server *server,
                           struct leme_desk_target target) {
  return target.kind == LEME_DESK_TARGET_CONTAINER
             ? leme_container_by_public_id(server, target.id)
             : NULL;
}

struct leme_view *leme_desk_target_member(struct leme_server *server,
                                          struct leme_desk_target target) {
  struct leme_container *container = leme_desk_target_container(server, target);
  return container != NULL ? container->focused
                           : leme_desk_target_view(server, target);
}

void leme_desk_target_act(struct leme_server *server,
                          struct leme_desk_target target,
                          enum leme_render_decoration_part part) {
  struct leme_container *container = leme_desk_target_container(server, target);
  struct leme_view *view = leme_desk_target_view(server, target);
  switch (part) {
  case LEME_RENDER_DECORATION_MINIMIZE:
    (void)(container != NULL ? leme_container_set_minimized(container, true)
                             : leme_view_set_minimized(view, true));
    break;
  case LEME_RENDER_DECORATION_MAXIMIZE:
    if (container != NULL) {
      (void)leme_container_set_placement(
          container, container->placement.kind == LEME_VIEW_PLACEMENT_MAXIMIZED
                         ? LEME_VIEW_PLACEMENT_NORMAL
                         : LEME_VIEW_PLACEMENT_MAXIMIZED);
    } else {
      (void)leme_view_set_maximized(view, !leme_view_is_maximized(view));
    }
    break;
  case LEME_RENDER_DECORATION_CLOSE:
    if (container != NULL) {
      (void)leme_container_close(container);
    } else if (view != NULL) {
      leme_view_protocol_close(view);
    }
    break;
  default:
    break;
  }
}
