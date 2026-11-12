#include "shell/view.h"
#include "shell/xwayland.h"

#include "core/server.h"
#include "desktop/strip.h"
#include "desktop/grid.h"
#include "desktop/target.h"
#include "shell/container.h"
#include "output/output.h"
#include "input/input.h"
#include "protocols/capture.h"
#include "protocols/input.h"
#include "protocols/publication.h"
#include "protocols/session.h"
#include "public/server.h"
#include "render/render.h"
#include "render/workspace_transition.h"
#include "workspace/tag.h"

#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/xwayland/xwayland.h>

static bool leme_minimize_eligible(const struct leme_view *view) {
  const struct leme_tag *tag = leme_ownership_tag(view);
  return view != NULL && view->server != NULL && view->mapped &&
         !view->unmanaged && !view->fullscreen && !view->detached &&
         tag != NULL && tag->layout.kind == LEME_LAYOUT_DESKTOP;
}

static struct leme_view *leme_minimize_parent(const struct leme_view *view) {
  if (view == NULL) {
    return NULL;
  }
  struct leme_view *parent = NULL;
  if (view->kind == LEME_VIEW_XDG && view->xdg_toplevel != NULL &&
      view->xdg_toplevel->parent != NULL &&
      view->xdg_toplevel->parent->base != NULL) {
    const struct wlr_scene_tree *tree = view->xdg_toplevel->parent->base->data;
    if (tree != NULL && tree->node.parent != NULL) {
      const struct leme_render_owner *owner = tree->node.parent->node.data;
      parent = owner != NULL && owner->kind == LEME_RENDER_OWNER_VIEW
                   ? owner->value.view
                   : NULL;
    }
  } else if (view->kind == LEME_VIEW_XWAYLAND) {
    parent = leme_xwayland_public_parent(view);
  }
  return leme_minimize_eligible(parent) && parent->server == view->server &&
                 parent->kind == view->kind &&
                 leme_ownership_tag(parent) == leme_ownership_tag(view)
             ? parent
             : NULL;
}

static bool leme_minimize_ancestry(const struct leme_view *view,
                                   const struct leme_view **root,
                                   const struct leme_container **container) {
  *root = NULL;
  *container = NULL;
  if (!leme_minimize_eligible(view)) {
    return false;
  }
  const struct leme_view *cursor = view;
  const struct leme_view *fast = view;
  while (cursor != NULL) {
    if (cursor->minimized) {
      *root = cursor;
      *container = NULL;
    }
    if (cursor->container != NULL && cursor->container->minimized) {
      *root = NULL;
      *container = cursor->container;
    }
    cursor = leme_minimize_parent(cursor);
    fast = leme_minimize_parent(leme_minimize_parent(fast));
    if (cursor != NULL && cursor == fast) {
      *root = NULL;
      *container = NULL;
      return false;
    }
  }
  return true;
}

static bool
leme_container_can_minimize(const struct leme_container *container) {
  if (container == NULL || container->count < 2 || container->tag == NULL ||
      container->tag->layout.kind != LEME_LAYOUT_DESKTOP) {
    return false;
  }
  for (size_t i = 0; i < container->count; i++) {
    if (!leme_minimize_eligible(container->views[i]) ||
        leme_ownership_tag(container->views[i]) != container->tag) {
      return false;
    }
  }
  return true;
}

bool leme_view_can_minimize(const struct leme_view *view) {
  const struct leme_view *root = NULL;
  const struct leme_container *container = NULL;
  return leme_minimize_ancestry(view, &root, &container) &&
         (view->container == NULL ||
          leme_container_can_minimize(view->container));
}

const struct leme_view *leme_view_minimized_root(const struct leme_view *view) {
  const struct leme_view *root = NULL;
  const struct leme_container *container = NULL;
  (void)leme_minimize_ancestry(view, &root, &container);
  return root;
}

bool leme_view_minimized_unit(const struct leme_view *view,
                              struct leme_desk_target *out) {
  if (out == NULL) {
    return false;
  }
  *out = (struct leme_desk_target){0};
  const struct leme_view *root = NULL;
  const struct leme_container *container = NULL;
  if (!leme_minimize_ancestry(view, &root, &container)) {
    return false;
  }
  if (container != NULL) {
    *out = (struct leme_desk_target){LEME_DESK_TARGET_CONTAINER, container->id};
    return true;
  }
  if (root != NULL) {
    *out =
        (struct leme_desk_target){LEME_DESK_TARGET_VIEW, root->public_meta.id};
    return true;
  }
  return false;
}

bool leme_view_is_minimized(const struct leme_view *view) {
  struct leme_desk_target unit = {0};
  return leme_view_minimized_unit(view, &unit);
}

static bool leme_minimize_recovery_available(const struct leme_view *view) {
  struct leme_server *server = view->server;
  if (server->config != NULL && !server->config->desktop_minimized_strip) {
    return true;
  }
  const struct leme_output *output = leme_view_output(view);
  if (server->desk_strip == NULL || output == NULL ||
      output->wlr_output == NULL) {
    return false;
  }
  size_t count = leme_minimize_unit_count(leme_ownership_tag(view));
  if (!leme_view_is_minimized(view)) {
    if (view->container != NULL) {
      const struct leme_view *candidate = NULL;
      wl_list_for_each(candidate, &view->container->tag->views, tag_link) {
        if (leme_view_minimized_root(candidate) == candidate &&
            leme_container_for_view(candidate) == view->container &&
            count > 0) {
          count--;
        }
      }
    }
    if (count == SIZE_MAX) {
      return false;
    }
    count++;
  }
  int pixels = 0;
  return leme_desk_strip_capacity(output->usable_box, count) > 0 &&
         leme_desk_grid_pixels(output->wlr_output->scale, LEME_DESK_STRIP_ICON,
                               &pixels);
}

bool leme_container_minimize_available(const struct leme_container *container) {
  return leme_container_can_minimize(container) && container->focused != NULL &&
         leme_minimize_recovery_available(container->focused);
}

bool leme_view_minimize_available(const struct leme_view *view) {
  return leme_view_can_minimize(view) && leme_minimize_recovery_available(view);
}

static bool leme_minimize_pointer_hidden(struct leme_server *server) {
  struct wlr_surface *surface =
      server->seat == NULL ? NULL : server->seat->pointer_state.focused_surface;
  if (surface == NULL) {
    return false;
  }
  struct leme_view *owner = leme_view_from_surface(server, surface);
  if (owner != NULL) {
    return leme_view_is_minimized(owner);
  }
  surface = wlr_surface_get_root_surface(surface);
  struct leme_view *view = NULL;
  wl_list_for_each(view, &server->views, link) {
    if (!leme_view_is_minimized(view) || view->popups.next == NULL) {
      continue;
    }
    struct leme_view_popup *popup = NULL;
    wl_list_for_each(popup, &view->popups, link) {
      if (popup->wlr_popup != NULL && popup->wlr_popup->base != NULL &&
          popup->wlr_popup->base->surface == surface) {
        return true;
      }
    }
  }
  return false;
}

void leme_view_reconcile_minimized(struct leme_server *server) {
  if (server == NULL || server->views.next == NULL) {
    return;
  }
  leme_view_refresh_begin(server);
  bool changed = false;
  struct leme_view *view;
  wl_list_for_each(view, &server->views, link) {
    if (view->container != NULL && view->container->minimized &&
        !leme_container_can_minimize(view->container)) {
      view->container->minimized = false;
      changed = true;
    }
    if (view->minimized && !leme_view_can_minimize(view)) {
      view->minimized = false;
      changed = true;
    }
  }
  if (server->outputs.next != NULL) {
    struct leme_output *output = NULL;
    wl_list_for_each(output, &server->outputs, link) {
      if (leme_desk_strip_sync_output(output)) {
        continue;
      }
      struct leme_tag *tag = output->tags.table[output->tags.focused_id];
      struct leme_view *fullscreen = NULL;
      if (tag->containers.next != NULL) {
        struct leme_container *container = NULL;
        wl_list_for_each(container, &tag->containers, link) {
          changed = changed || container->minimized;
          container->minimized = false;
        }
      }
      wl_list_for_each(view, &tag->views, tag_link) {
        if (view->mapped && view->fullscreen) {
          fullscreen = view;
        }
        changed = changed || view->minimized;
        view->minimized = false;
      }
      wl_list_for_each(view, &tag->views, tag_link) {
        leme_render_set_view_visible(
            view, leme_ownership_scene_visible(view) &&
                      (fullscreen == NULL || fullscreen == view));
      }
      if (tag->containers.next != NULL) {
        struct leme_container *container = NULL;
        wl_list_for_each(container, &tag->containers, link) {
          leme_render_container_set_visible(container, fullscreen == NULL);
        }
      }
      (void)leme_desk_strip_sync_output(output);
    }
  }
  wl_list_for_each(view, &server->views, link) {
    if (leme_view_is_minimized(view)) {
      if (!leme_render_view_flying(view)) {
        leme_render_view_finish_animation(view);
      }
      leme_render_set_view_visible(view, false);
      if (view->container != NULL) {
        leme_render_container_set_visible(view->container, false);
      }
    }
    leme_view_sync_wm_state(view);
  }
  if (leme_view_is_minimized(server->focused_view)) {
    leme_view_clear_focus(server);
  }
  if (!leme_session_locked(server) && leme_minimize_pointer_hidden(server)) {
    wlr_seat_pointer_notify_clear_focus(server->seat);
    leme_input_protocols_update_pointer_focus(server);
  }
  if (changed) {
    leme_public_server_invalidate(server);
    leme_publication_invalidate(server);
  }
  leme_capture_sync(server);
  leme_view_refresh_end(server);
}

static bool leme_minimize_commit(struct leme_server *server,
                                 struct leme_tag *tag, bool *flag,
                                 uint64_t *order, bool minimized) {
  const uint64_t previous_order = *order;
  *flag = minimized;
  if (minimized) {
    *order = server->minimize_order + 1;
  }
  server->view_refresh.reconciled = false;
  server->view_refresh.desktop_synced = false;
  leme_view_refresh_begin(server);
  if (!leme_desk_strip_sync_output(tag->owner->output)) {
    *flag = !minimized;
    *order = previous_order;
    leme_tags_refresh_visibility_passive(tag->owner);
    leme_view_refresh_end(server);
    return false;
  }
  if (minimized) {
    server->minimize_order = *order;
  }
  return true;
}

static void leme_minimize_refresh(struct leme_server *server,
                                  const struct leme_tag *tag, bool minimized) {
  leme_view_refresh_reconcile(server);
  if (tag != NULL) {
    if (minimized) {
      leme_tags_refresh_visibility(tag->owner);
    } else {
      leme_tags_refresh_visibility_passive(tag->owner);
    }
  }
  leme_public_server_invalidate(server);
  leme_publication_invalidate(server);
  leme_capture_sync(server);
  leme_view_refresh_end(server);
}

static struct leme_box leme_minimize_icon_box(const struct leme_output *output,
                                              struct leme_desk_target target,
                                              struct leme_box box) {
  struct leme_box icon = {0};
  if (leme_desk_strip_icon_box(output, target, &icon)) {
    return icon;
  }
  icon.width = (int)((double)box.width * 0.6);
  icon.height = (int)((double)box.height * 0.6);
  icon.x = box.x + (box.width - icon.width) / 2;
  icon.y = box.y + (box.height - icon.height) / 2;
  return icon;
}

bool leme_container_set_minimized(struct leme_container *container,
                                  bool minimized) {
  if (container == NULL || container->tag == NULL ||
      container->tag->owner == NULL) {
    return false;
  }
  struct leme_server *server = container->tag->owner->server;
  if (server == NULL || leme_session_locked(server) ||
      (minimized && (!leme_container_minimize_available(container) ||
                     leme_input_interacting(server)))) {
    return false;
  }
  if (container->minimized == minimized) {
    return true;
  }
  const struct leme_box from = container->box;
  const struct leme_desk_target target = {LEME_DESK_TARGET_CONTAINER,
                                          container->id};
  struct leme_box icon =
      leme_minimize_icon_box(container->tag->owner->output, target, from);
  if (!leme_minimize_commit(server, container->tag, &container->minimized,
                            &container->minimized_order, minimized)) {
    return false;
  }
  container->generation++;
  leme_render_output_animations_finish(container->tag->owner->output);
  leme_minimize_refresh(server, container->tag, minimized);
  if (minimized) {
    icon = leme_minimize_icon_box(container->tag->owner->output, target, from);
  }
  leme_render_container_fly(container, minimized ? from : icon,
                            minimized ? icon : container->box, !minimized);
  return true;
}

bool leme_view_set_minimized(struct leme_view *view, bool minimized) {
  if (view == NULL || view->server == NULL || !view->mapped ||
      view->unmanaged || leme_session_locked(view->server)) {
    return false;
  }
  struct leme_server *server = view->server;
  if (view->container != NULL) {
    return leme_container_set_minimized(view->container, minimized);
  }
  if (!minimized) {
    struct leme_desk_target unit = {0};
    if (leme_view_minimized_unit(view, &unit) &&
        unit.kind == LEME_DESK_TARGET_CONTAINER &&
        !leme_container_set_minimized(
            leme_container_by_public_id(server, unit.id), false)) {
      return false;
    }
  }
  struct leme_view *root = view;
  const struct leme_view *hidden_root = leme_view_minimized_root(view);
  if (!minimized && hidden_root != NULL) {
    struct leme_view *candidate = NULL;
    wl_list_for_each(candidate, &server->views, link) {
      if (candidate == hidden_root) {
        root = candidate;
        break;
      }
    }
  }
  const struct leme_box from = root->box;
  const struct leme_desk_target target = {LEME_DESK_TARGET_VIEW,
                                          root->public_meta.id};
  struct leme_box icon =
      leme_minimize_icon_box(leme_view_output(root), target, from);
  if (minimized) {
    if (!leme_view_can_minimize(view) ||
        !leme_minimize_recovery_available(view) ||
        leme_input_interacting(server)) {
      return false;
    }
    if (leme_view_is_minimized(view)) {
      return true;
    }
    if (!leme_minimize_commit(server, leme_ownership_tag(view),
                              &view->minimized, &view->minimized_order, true)) {
      return false;
    }
  } else {
    if (!view->minimized && !leme_view_is_minimized(view)) {
      return true;
    }
    size_t remaining = (size_t)wl_list_length(&server->views);
    struct leme_view *cursor = view;
    while (cursor != NULL && remaining > 0) {
      remaining--;
      cursor->minimized = false;
      cursor = leme_minimize_parent(cursor);
    }
    server->view_refresh.reconciled = false;
    server->view_refresh.desktop_synced = false;
    leme_view_refresh_begin(server);
  }
  leme_minimize_refresh(server, leme_ownership_tag(view), minimized);
  if (minimized) {
    icon = leme_minimize_icon_box(leme_view_output(root), target, from);
  }
  leme_render_view_fly(root, minimized ? from : icon,
                       minimized ? icon : root->box, !minimized);
  return true;
}

bool leme_view_show_desktop(struct leme_server *server) {
  struct leme_tags *tags = leme_focused_tags(server);
  struct leme_tag *tag = tags == NULL || tags->focused_is_candidate
                             ? NULL
                             : tags->table[tags->focused_id];
  if (tag == NULL || tag->layout.kind != LEME_LAYOUT_DESKTOP ||
      leme_session_locked(server)) {
    return false;
  }
  bool visible = false;
  struct leme_view *view;
  wl_list_for_each(view, &tag->views, tag_link) {
    visible = visible ||
              (leme_view_can_minimize(view) && !leme_view_is_minimized(view));
  }
  bool ok = true;
  wl_list_for_each(view, &tag->views, tag_link) {
    if (visible && leme_view_can_minimize(view) &&
        !leme_view_is_minimized(view)) {
      view->desktop_hidden = leme_view_set_minimized(view, true);
      ok = ok && view->desktop_hidden;
    } else if (!visible && view->desktop_hidden) {
      view->desktop_hidden = false;
      ok = (!leme_view_is_minimized(view) ||
            leme_view_set_minimized(view, false)) &&
           ok;
    }
  }
  return ok;
}
