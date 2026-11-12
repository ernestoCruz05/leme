#include "shell/xwayland.h"
#include "public/server.h"

#include "output/output.h"
#include "render/render.h"
#include "input/input.h"
#include "core/server.h"
#include "protocols/toplevel.h"
#include "protocols/publication.h"
#include "workspace/tag.h"
#include "shell/policy.h"
#include "shell/rules.h"
#include "shell/scratchpad.h"
#include "shell/sticky.h"
#include "shell/view.h"
#include "shell/container.h"
#include "desktop/decoration.h"
#include "desktop/buttons.h"

#include <stddef.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/box.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>
#include <wlr/xcursor.h>
#include <wlr/xwayland/server.h>
#include <wlr/xwayland/shell.h>
#include <wlr/xwayland/xwayland.h>

struct leme_xwayland_view {
  struct leme_xwayland *xwayland;
  struct wlr_xwayland_surface *xsurface;
  struct leme_view *view;
  struct wl_list link;
  struct wl_listener associate;
  struct wl_listener dissociate;
  struct wl_listener destroy;
  struct wl_listener map;
  struct wl_listener unmap;
  struct wl_listener request_configure;
  struct wl_listener request_activate;
  struct wl_listener request_fullscreen;
  struct wl_listener request_move;
  struct wl_listener request_resize;
  struct wl_listener request_minimize;
  struct wl_listener request_maximize;
  struct wl_listener set_geometry;
  struct wl_listener set_title;
  struct wl_listener set_app_id;
  struct wl_listener set_parent;
  struct wl_listener set_window_type;
  struct wl_listener set_override_redirect;
  struct wl_listener set_decorations;
  struct wl_listener set_size_hints;
  bool associated;
};

bool leme_xwayland_global_allowed(const struct leme_server *server,
                                  const struct wl_client *client,
                                  const struct wl_global *global) {
  const struct leme_xwayland *xwayland = server->xwayland;

  if (xwayland == NULL || xwayland->wlr_xwayland == NULL ||
      xwayland->wlr_xwayland->shell_v1 == NULL ||
      global != xwayland->wlr_xwayland->shell_v1->global) {
    return true;
  }
  return xwayland->wlr_xwayland->server != NULL &&
         client == xwayland->wlr_xwayland->server->client;
}

static void leme_xwayland_handle_ready(struct wl_listener *listener,
                                       void *data) {
  struct leme_xwayland *xwayland = wl_container_of(listener, xwayland, ready);

  (void)data;
  xwayland->is_ready = true;
  leme_xwayland_update_workarea(xwayland->server);
  wlr_log(WLR_INFO, "leme: XWayland ready on %s",
          xwayland->wlr_xwayland->display_name);
}

static struct leme_box
leme_xwayland_requested_box(struct leme_xwayland_view *wrapper,
                            struct leme_box requested) {
  struct leme_output *output = leme_view_output(wrapper->view);

  if (output == NULL) {
    output = leme_output_focused(wrapper->xwayland->server);
  }
  return leme_view_policy_requested_box(requested,
                                        leme_output_usable_box(output),
                                        wrapper->xsurface->override_redirect);
}

static bool
leme_xwayland_initial_floating(struct wlr_xwayland_surface *surface) {
  bool fixed = surface->size_hints != NULL &&
               leme_view_policy_fixed_size(surface->size_hints->min_width,
                                           surface->size_hints->min_height,
                                           surface->size_hints->max_width,
                                           surface->size_hints->max_height);

  return surface->parent != NULL || surface->modal || fixed ||
         wlr_xwayland_surface_has_window_type(
             surface, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DIALOG) ||
         wlr_xwayland_surface_has_window_type(
             surface, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_UTILITY) ||
         wlr_xwayland_surface_has_window_type(
             surface, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_SPLASH) ||
         wlr_xwayland_surface_has_window_type(
             surface, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_TOOLBAR) ||
         wlr_xwayland_surface_has_window_type(
             surface, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_NOTIFICATION) ||
         wlr_xwayland_surface_has_window_type(
             surface, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_POPUP_MENU) ||
         wlr_xwayland_surface_has_window_type(
             surface, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DROPDOWN_MENU) ||
         wlr_xwayland_surface_has_window_type(
             surface, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_MENU) ||
         wlr_xwayland_surface_has_window_type(
             surface, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_TOOLTIP) ||
         wlr_xwayland_surface_has_window_type(
             surface, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_COMBO) ||
         wlr_xwayland_surface_has_window_type(
             surface, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DND);
}

static void leme_xwayland_surface_map(struct leme_xwayland_view *wrapper) {
  struct leme_view *view = wrapper->view;
  const bool requested_maximized =
      wrapper->xsurface->maximized_horz && wrapper->xsurface->maximized_vert;
  const enum leme_scratchpad_map_result scratchpad_map =
      leme_scratchpad_try_adopt_map(view);
  struct leme_xwayland_view *parent_wrapper =
      wrapper->xsurface->parent == NULL ? NULL
                                        : wrapper->xsurface->parent->data;
  struct leme_view *parent =
      parent_wrapper == NULL ? NULL : parent_wrapper->view;
  struct leme_view_destination destination;
  struct leme_sticky_adoption *adoption = NULL;

  if (parent != NULL && (!parent->mapped || parent->unmanaged ||
                         (!leme_view_is_sticky(parent) &&
                          (leme_ownership_tag(parent) == NULL ||
                           leme_ownership_tag(parent)->owner == NULL)))) {
    parent = NULL;
  }
  destination = leme_view_policy_destination(view->server, parent);

  const struct leme_view_rules rules = leme_view_rules_match(
      view->server->config, leme_view_identity(view), leme_view_title(view));
  bool floating = leme_xwayland_initial_floating(wrapper->xsurface);

  leme_view_policy_apply_rules(view->server, &rules, &destination, &floating,
                               parent != NULL);
  view->rule_fields =
      rules.fields |
      (parent != NULL ? LEME_WINDOW_RULE_TAG | LEME_WINDOW_RULE_OUTPUT : 0);

  const bool sticky_parent = parent != NULL && leme_view_is_sticky(parent) &&
                             !wrapper->xsurface->override_redirect;
  const struct leme_view_map_options options = {
      .tags = sticky_parent ? NULL : destination.tags,
      .tag_id = sticky_parent ? 0 : destination.tag_id,
      .parent = parent,
      .floating = floating,
      .unmanaged = wrapper->xsurface->override_redirect,
      .durable_direct = sticky_parent,
  };

  if (scratchpad_map == LEME_SCRATCHPAD_MAP_ADOPTED) {
    return;
  }
  if (scratchpad_map == LEME_SCRATCHPAD_MAP_FAILED) {
    wlr_log(WLR_ERROR, "%s", "leme: failed to adopt pending X11 scratchpad");
    return;
  }
  if (sticky_parent &&
      !leme_sticky_prepare_transient(parent, view, &adoption)) {
    wlr_log(WLR_ERROR, "leme: failed to prepare sticky X11 transient 0x%x",
            wrapper->xsurface->window_id);
    return;
  }
  if (!leme_view_map(view, &options)) {
    leme_sticky_discard_transient(&adoption);
    wlr_log(WLR_ERROR, "leme: failed to map X11 window 0x%x",
            wrapper->xsurface->window_id);
    return;
  }
  if (adoption != NULL) {
    leme_view_apply_layout_box(
        view,
        leme_view_policy_center_box(
            view->box, parent->box,
            leme_output_usable_box(leme_view_output(parent))),
        false);
    leme_sticky_commit_transient(&adoption);
  }
  if (requested_maximized) {
    (void)leme_view_set_maximized(view, true);
  }
  leme_view_sync_wm_state(view);
  if (wrapper->xsurface->fullscreen ||
      ((rules.fields & LEME_WINDOW_RULE_FULLSCREEN) != 0 && rules.fullscreen)) {
    leme_view_set_fullscreen(view, true);
  }
  if (view->unmanaged &&
      wlr_xwayland_surface_override_redirect_wants_focus(wrapper->xsurface)) {
    leme_view_focus_unmanaged_xwayland(view);
  }
  wlr_log(WLR_INFO, "leme: mapped X11 window 0x%x%s%s",
          wrapper->xsurface->window_id,
          wrapper->xsurface->class == NULL ? "" : " class=",
          wrapper->xsurface->class == NULL ? "" : wrapper->xsurface->class);
}

static void leme_xwayland_handle_map(struct wl_listener *listener, void *data) {
  struct leme_xwayland_view *wrapper = wl_container_of(listener, wrapper, map);

  (void)data;
  leme_xwayland_surface_map(wrapper);
}

static void leme_xwayland_handle_unmap(struct wl_listener *listener,
                                       void *data) {
  struct leme_xwayland_view *wrapper =
      wl_container_of(listener, wrapper, unmap);

  (void)data;
  leme_view_unmap(wrapper->view);
}

static void
leme_xwayland_finish_association(struct leme_xwayland_view *wrapper) {
  if (!wrapper->associated) {
    return;
  }
  leme_view_unmap(wrapper->view);
  wl_list_remove(&wrapper->map.link);
  wl_list_remove(&wrapper->unmap.link);
  wrapper->associated = false;
}

static void leme_xwayland_handle_associate(struct wl_listener *listener,
                                           void *data) {
  struct leme_xwayland_view *wrapper =
      wl_container_of(listener, wrapper, associate);
  struct wlr_surface *surface = wrapper->xsurface->surface;

  (void)data;
  wrapper->view->xwayland_surface = wrapper->xsurface;
  wrapper->map.notify = leme_xwayland_handle_map;
  wl_signal_add(&surface->events.map, &wrapper->map);
  wrapper->unmap.notify = leme_xwayland_handle_unmap;
  wl_signal_add(&surface->events.unmap, &wrapper->unmap);
  wrapper->associated = true;
  if (surface->mapped) {
    leme_xwayland_surface_map(wrapper);
  }
}

static void leme_xwayland_handle_dissociate(struct wl_listener *listener,
                                            void *data) {
  struct leme_xwayland_view *wrapper =
      wl_container_of(listener, wrapper, dissociate);

  (void)data;
  leme_xwayland_finish_association(wrapper);
}

static void leme_xwayland_handle_request_configure(struct wl_listener *listener,
                                                   void *data) {
  struct leme_xwayland_view *wrapper =
      wl_container_of(listener, wrapper, request_configure);
  struct wlr_xwayland_surface_configure_event *event = data;
  struct leme_view *view = wrapper->view;
  struct leme_box requested = {
      .x = event->x,
      .y = event->y,
      .width = event->width,
      .height = event->height,
  };

  if (!view->mapped) {
    view->box = leme_xwayland_requested_box(wrapper, requested);
    leme_view_configure(view, view->box);
  } else if (leme_view_geometry_owner(view) == LEME_VIEW_GEOMETRY_FLOATING &&
             !leme_view_is_placed(view) && !view->fullscreen) {
    view->box = leme_xwayland_requested_box(
        wrapper, leme_render_view_frame_box(view, requested));
    leme_render_view_set_box(view, view->box);
  } else {
    leme_view_configure(view, leme_render_view_content_box(view, view->box));
  }
}

static void leme_xwayland_handle_request_activate(struct wl_listener *listener,
                                                  void *data) {
  struct leme_xwayland_view *wrapper =
      wl_container_of(listener, wrapper, request_activate);
  struct leme_view *view = wrapper->view;

  (void)data;
  if (!view->mapped) {
    return;
  }
  if (view->unmanaged) {
    if (wlr_xwayland_surface_override_redirect_wants_focus(wrapper->xsurface)) {
      leme_view_focus_unmanaged_xwayland(view);
    }
    return;
  }
  leme_toplevel_activate_view(view);
}

static void
leme_xwayland_handle_request_fullscreen(struct wl_listener *listener,
                                        void *data) {
  struct leme_xwayland_view *wrapper =
      wl_container_of(listener, wrapper, request_fullscreen);

  (void)data;
  if (wrapper->view->mapped) {
    leme_view_set_fullscreen(wrapper->view, wrapper->xsurface->fullscreen);
  } else {
    leme_view_ack_fullscreen(wrapper->view, wrapper->xsurface->fullscreen);
  }
}

static void
leme_xwayland_reassert_geometry(struct leme_xwayland_view *wrapper) {
  if (wrapper->view->mapped) {
    leme_view_configure(wrapper->view, leme_render_view_content_box(
                                           wrapper->view, wrapper->view->box));
  }
}

static void leme_xwayland_handle_request_move(struct wl_listener *listener,
                                              void *data) {
  struct leme_xwayland_view *wrapper =
      wl_container_of(listener, wrapper, request_move);

  (void)data;
  if (!leme_input_pointer_grab_start_xwayland(wrapper->view, false,
                                              WLR_EDGE_NONE)) {
    wlr_log(WLR_DEBUG, "%s", "leme: rejected XWayland move request");
    leme_xwayland_reassert_geometry(wrapper);
  }
}

static void leme_xwayland_handle_request_resize(struct wl_listener *listener,
                                                void *data) {
  struct leme_xwayland_view *wrapper =
      wl_container_of(listener, wrapper, request_resize);
  struct wlr_xwayland_resize_event *event = data;

  if (!leme_input_pointer_grab_start_xwayland(wrapper->view, true,
                                              event->edges)) {
    wlr_log(WLR_DEBUG, "%s", "leme: rejected XWayland resize request");
    leme_xwayland_reassert_geometry(wrapper);
  }
}

static void leme_xwayland_handle_request_minimize(struct wl_listener *listener,
                                                  void *data) {
  struct leme_xwayland_view *wrapper =
      wl_container_of(listener, wrapper, request_minimize);

  const struct wlr_xwayland_minimize_event *event = data;
  if (event != NULL && wrapper->view->mapped) {
    (void)leme_view_set_minimized(wrapper->view, event->minimize);
  }
  wlr_xwayland_surface_set_minimized(wrapper->xsurface,
                                     leme_view_is_minimized(wrapper->view));
}

static void leme_xwayland_handle_request_maximize(struct wl_listener *listener,
                                                  void *data) {
  struct leme_xwayland_view *wrapper =
      wl_container_of(listener, wrapper, request_maximize);

  (void)data;
  if (!wrapper->view->mapped) {
    return;
  }
  if (wrapper->xsurface->maximized_horz == wrapper->xsurface->maximized_vert) {
    (void)leme_view_set_maximized(wrapper->view,
                                  wrapper->xsurface->maximized_horz);
  }
  leme_view_sync_wm_state(wrapper->view);
}

static void leme_xwayland_handle_set_size_hints(struct wl_listener *listener,
                                                void *data) {
  struct leme_xwayland_view *wrapper =
      wl_container_of(listener, wrapper, set_size_hints);
  (void)data;
  if (wrapper->view->mapped) {
    leme_desk_buttons_invalidate_view(wrapper->view);
    leme_view_reconcile_placement(wrapper->view->server);
    leme_view_sync_wm_state(wrapper->view);
    leme_desk_decoration_refresh(wrapper->view);
  }
}

static bool
leme_xwayland_geometry_is_echo(const struct leme_xwayland_view *wrapper) {
  const struct leme_view *view = wrapper->view;

  return view->configured_box_valid &&
         view->configured_box.x == wrapper->xsurface->x &&
         view->configured_box.y == wrapper->xsurface->y &&
         view->configured_box.width == wrapper->xsurface->width &&
         view->configured_box.height == wrapper->xsurface->height;
}

#define LEME_GEOMETRY_STORM_WINDOW_MS 1000u
#define LEME_GEOMETRY_STORM_LIMIT 200u

static bool leme_xwayland_geometry_storm(struct leme_xwayland_view *wrapper) {
  struct leme_view *view = wrapper->view;
  struct timespec now = {0};
  uint64_t milliseconds;

  clock_gettime(CLOCK_MONOTONIC, &now);
  milliseconds =
      (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
  if (milliseconds - view->geometry_window_ms >=
      LEME_GEOMETRY_STORM_WINDOW_MS) {
    view->geometry_window_ms = milliseconds;
    view->geometry_events = 0;
    view->geometry_storm = false;
  }
  view->geometry_events++;
  if (view->geometry_events <= LEME_GEOMETRY_STORM_LIMIT) {
    return false;
  }
  if (!view->geometry_storm) {
    view->geometry_storm = true;
    wlr_log(WLR_ERROR,
            "leme: X11 0x%x geometry storm, ignoring: %u events/s, "
            "surface=%dx%d+%d+%d configured=%dx%d+%d+%d(valid=%d) "
            "box=%dx%d+%d+%d mapped=%d floating=%d unmanaged=%d fullscreen=%d",
            wrapper->xsurface->window_id, view->geometry_events,
            (int)wrapper->xsurface->width, (int)wrapper->xsurface->height,
            (int)wrapper->xsurface->x, (int)wrapper->xsurface->y,
            view->configured_box.width, view->configured_box.height,
            view->configured_box.x, view->configured_box.y,
            view->configured_box_valid ? 1 : 0, view->box.width,
            view->box.height, view->box.x, view->box.y, view->mapped ? 1 : 0,
            view->floating ? 1 : 0, view->unmanaged ? 1 : 0,
            view->fullscreen ? 1 : 0);
  }
  return true;
}

static void leme_xwayland_handle_set_geometry(struct wl_listener *listener,
                                              void *data) {
  struct leme_xwayland_view *wrapper =
      wl_container_of(listener, wrapper, set_geometry);

  (void)data;
  if (leme_xwayland_geometry_is_echo(wrapper)) {
    return;
  }
  if (leme_xwayland_geometry_storm(wrapper)) {
    return;
  }
  if (wrapper->view->mapped &&
      leme_view_geometry_owner(wrapper->view) == LEME_VIEW_GEOMETRY_CONTAINER) {
    leme_xwayland_reassert_geometry(wrapper);
    return;
  }
  if (wrapper->view->mapped && (leme_view_geometry_owner(wrapper->view) ==
                                LEME_VIEW_GEOMETRY_FLOATING)) {
    wrapper->view->box = leme_xwayland_requested_box(
        wrapper, leme_render_view_frame_box(
                     wrapper->view, (struct leme_box){
                                        .x = wrapper->xsurface->x,
                                        .y = wrapper->xsurface->y,
                                        .width = wrapper->xsurface->width,
                                        .height = wrapper->xsurface->height,
                                    }));
    leme_render_view_set_box(wrapper->view, wrapper->view->box);
  }
}

static void leme_xwayland_handle_set_title(struct wl_listener *listener,
                                           void *data) {
  struct leme_xwayland_view *wrapper =
      wl_container_of(listener, wrapper, set_title);

  (void)data;
  leme_view_apply_late_rules(wrapper->view);
  leme_public_server_invalidate(wrapper->view->server);
  leme_desk_decoration_refresh(wrapper->view);
  leme_render_view_set_activated(
      wrapper->view, wrapper->view == wrapper->view->server->focused_view);
}

static void leme_xwayland_handle_set_class(struct wl_listener *listener,
                                           void *data) {
  struct leme_xwayland_view *wrapper =
      wl_container_of(listener, wrapper, set_app_id);

  (void)data;
  leme_scratchpad_handle_identity_change(wrapper->view);
  leme_view_apply_late_rules(wrapper->view);
  if (wrapper->view->mapped) {
    leme_view_arrange_instant(wrapper->view->server);
  }
  leme_public_server_invalidate(wrapper->view->server);
  leme_render_view_set_activated(
      wrapper->view, wrapper->view == wrapper->view->server->focused_view);
}

static void leme_xwayland_handle_set_parent(struct wl_listener *listener,
                                            void *data) {
  struct leme_xwayland_view *wrapper =
      wl_container_of(listener, wrapper, set_parent);
  (void)data;
  struct leme_view *view = wrapper->view;
  leme_container_reconcile_view(view);
  leme_public_server_invalidate(view->server);
  if (view->mapped && !view->unmanaged) {
    const bool refocus = leme_view_is_minimized(view->server->focused_view);
    leme_view_refresh_begin(view->server);
    leme_view_refresh_reconcile(view->server);
    const struct leme_tag *tag = leme_ownership_tag(view);
    if (refocus) {
      leme_view_refresh_tag_focus(view->server);
    }
    if (tag != NULL) {
      leme_tags_refresh_visibility_passive(tag->owner);
    }
    leme_view_refresh_end(view->server);
    leme_publication_invalidate(view->server);
  }
}

static void leme_xwayland_handle_set_window_type(struct wl_listener *listener,
                                                 void *data) {
  struct leme_xwayland_view *wrapper =
      wl_container_of(listener, wrapper, set_window_type);
  (void)data;
  leme_container_reconcile_view(wrapper->view);
}

static void
leme_xwayland_handle_set_override_redirect(struct wl_listener *listener,
                                           void *data) {
  struct leme_xwayland_view *wrapper =
      wl_container_of(listener, wrapper, set_override_redirect);
  (void)data;
  leme_container_reconcile_view(wrapper->view);
}

static void leme_xwayland_handle_set_decorations(struct wl_listener *listener,
                                                 void *data) {
  struct leme_xwayland_view *wrapper =
      wl_container_of(listener, wrapper, set_decorations);
  (void)data;
  if (wrapper->view->mapped) {
    leme_view_arrange_instant(wrapper->view->server);
  }
}

static void leme_xwayland_handle_view_destroy(struct wl_listener *listener,
                                              void *data) {
  struct leme_xwayland_view *wrapper =
      wl_container_of(listener, wrapper, destroy);

  (void)data;
  leme_xwayland_finish_association(wrapper);
  leme_view_destroy_core(wrapper->view);
  wl_list_remove(&wrapper->associate.link);
  wl_list_remove(&wrapper->dissociate.link);
  wl_list_remove(&wrapper->destroy.link);
  wl_list_remove(&wrapper->request_configure.link);
  wl_list_remove(&wrapper->request_activate.link);
  wl_list_remove(&wrapper->request_fullscreen.link);
  wl_list_remove(&wrapper->request_move.link);
  wl_list_remove(&wrapper->request_resize.link);
  wl_list_remove(&wrapper->request_minimize.link);
  wl_list_remove(&wrapper->request_maximize.link);
  wl_list_remove(&wrapper->set_geometry.link);
  wl_list_remove(&wrapper->set_title.link);
  wl_list_remove(&wrapper->set_app_id.link);
  wl_list_remove(&wrapper->set_parent.link);
  wl_list_remove(&wrapper->set_window_type.link);
  wl_list_remove(&wrapper->set_override_redirect.link);
  wl_list_remove(&wrapper->set_decorations.link);
  wl_list_remove(&wrapper->set_size_hints.link);
  wl_list_remove(&wrapper->link);
  wrapper->xsurface->data = NULL;
  free(wrapper->view);
  free(wrapper);
}

static void leme_xwayland_handle_new_surface(struct wl_listener *listener,
                                             void *data) {
  struct leme_xwayland *xwayland =
      wl_container_of(listener, xwayland, new_surface);
  struct wlr_xwayland_surface *xsurface = data;
  struct leme_xwayland_view *wrapper = calloc(1, sizeof(*wrapper));
  struct leme_view *view = calloc(1, sizeof(*view));

  if (wrapper == NULL || view == NULL) {
    free(wrapper);
    free(view);
    wlr_log(WLR_ERROR, "%s", "leme: failed to track X11 window");
    return;
  }
  wrapper->xwayland = xwayland;
  wrapper->xsurface = xsurface;
  wrapper->view = view;
  view->server = xwayland->server;
  view->kind = LEME_VIEW_XWAYLAND;
  view->xwayland_surface = xsurface;
  view->render_opacity = 1.0f;
  leme_ownership_init(view);
  wl_list_init(&view->link);
  wl_list_init(&view->tag_link);
  wl_list_init(&view->focus_link);
  wl_list_init(&view->scratchpad_link);
  wl_list_init(&view->popups);
  wl_list_insert(&view->server->views, &view->link);
  wl_list_insert(&xwayland->views, &wrapper->link);
  xsurface->data = wrapper;

  wrapper->associate.notify = leme_xwayland_handle_associate;
  wl_signal_add(&xsurface->events.associate, &wrapper->associate);
  wrapper->dissociate.notify = leme_xwayland_handle_dissociate;
  wl_signal_add(&xsurface->events.dissociate, &wrapper->dissociate);
  wrapper->destroy.notify = leme_xwayland_handle_view_destroy;
  wl_signal_add(&xsurface->events.destroy, &wrapper->destroy);
  wrapper->request_configure.notify = leme_xwayland_handle_request_configure;
  wl_signal_add(&xsurface->events.request_configure,
                &wrapper->request_configure);
  wrapper->request_activate.notify = leme_xwayland_handle_request_activate;
  wl_signal_add(&xsurface->events.request_activate, &wrapper->request_activate);
  wrapper->request_fullscreen.notify = leme_xwayland_handle_request_fullscreen;
  wl_signal_add(&xsurface->events.request_fullscreen,
                &wrapper->request_fullscreen);
  wrapper->request_move.notify = leme_xwayland_handle_request_move;
  wl_signal_add(&xsurface->events.request_move, &wrapper->request_move);
  wrapper->request_resize.notify = leme_xwayland_handle_request_resize;
  wl_signal_add(&xsurface->events.request_resize, &wrapper->request_resize);
  wrapper->request_minimize.notify = leme_xwayland_handle_request_minimize;
  wl_signal_add(&xsurface->events.request_minimize, &wrapper->request_minimize);
  wrapper->request_maximize.notify = leme_xwayland_handle_request_maximize;
  wl_signal_add(&xsurface->events.request_maximize, &wrapper->request_maximize);
  wrapper->set_geometry.notify = leme_xwayland_handle_set_geometry;
  wl_signal_add(&xsurface->events.set_geometry, &wrapper->set_geometry);
  wrapper->set_title.notify = leme_xwayland_handle_set_title;
  wl_signal_add(&xsurface->events.set_title, &wrapper->set_title);
  wrapper->set_app_id.notify = leme_xwayland_handle_set_class;
  wl_signal_add(&xsurface->events.set_class, &wrapper->set_app_id);
  wrapper->set_parent.notify = leme_xwayland_handle_set_parent;
  wl_signal_add(&xsurface->events.set_parent, &wrapper->set_parent);
  wrapper->set_window_type.notify = leme_xwayland_handle_set_window_type;
  wl_signal_add(&xsurface->events.set_window_type, &wrapper->set_window_type);
  wrapper->set_override_redirect.notify =
      leme_xwayland_handle_set_override_redirect;
  wl_signal_add(&xsurface->events.set_override_redirect,
                &wrapper->set_override_redirect);
  wrapper->set_decorations.notify = leme_xwayland_handle_set_decorations;
  wl_signal_add(&xsurface->events.set_decorations, &wrapper->set_decorations);
  wrapper->set_size_hints.notify = leme_xwayland_handle_set_size_hints;
  wl_signal_add(&xsurface->events.set_size_hints, &wrapper->set_size_hints);
}

static void leme_xwayland_handle_destroy(struct wl_listener *listener,
                                         void *data) {
  struct leme_xwayland *xwayland = wl_container_of(listener, xwayland, destroy);

  (void)data;
  wl_list_remove(&xwayland->ready.link);
  wl_list_remove(&xwayland->new_surface.link);
  wl_list_remove(&xwayland->destroy.link);
  xwayland->wlr_xwayland = NULL;
  xwayland->is_ready = false;
}

struct leme_view *leme_xwayland_public_parent(const struct leme_view *view) {
  if (view == NULL || view->kind != LEME_VIEW_XWAYLAND ||
      view->xwayland_surface == NULL || view->xwayland_surface->parent == NULL)
    return NULL;
  const struct leme_xwayland_view *parent =
      view->xwayland_surface->parent->data;
  return parent == NULL ? NULL : parent->view;
}

void leme_xwayland_init(struct leme_server *server) {
  struct leme_xwayland *xwayland = calloc(1, sizeof(*xwayland));

  if (xwayland == NULL) {
    wlr_log(WLR_ERROR, "%s",
            "leme: XWayland unavailable; continuing Wayland-only");
    return;
  }
  xwayland->server = server;
  wl_list_init(&xwayland->views);
  xwayland->wlr_xwayland =
      wlr_xwayland_create(server->display, server->compositor, true);
  if (xwayland->wlr_xwayland == NULL) {
    wlr_log(WLR_ERROR, "%s",
            "leme: XWayland unavailable; continuing Wayland-only");
    free(xwayland);
    return;
  }
  wlr_xwayland_set_seat(xwayland->wlr_xwayland, server->seat);
  xwayland->ready.notify = leme_xwayland_handle_ready;
  wl_signal_add(&xwayland->wlr_xwayland->events.ready, &xwayland->ready);
  xwayland->new_surface.notify = leme_xwayland_handle_new_surface;
  wl_signal_add(&xwayland->wlr_xwayland->events.new_surface,
                &xwayland->new_surface);
  xwayland->destroy.notify = leme_xwayland_handle_destroy;
  wl_signal_add(&xwayland->wlr_xwayland->events.destroy, &xwayland->destroy);
  server->xwayland = xwayland;
  wlr_log(WLR_INFO, "leme: reserved XWayland DISPLAY=%s",
          xwayland->wlr_xwayland->display_name);
}

void leme_xwayland_finish(struct leme_server *server) {
  struct leme_xwayland *xwayland = server->xwayland;

  if (xwayland == NULL) {
    return;
  }
  if (xwayland->wlr_xwayland != NULL) {
    wlr_xwayland_destroy(xwayland->wlr_xwayland);
  }
  free(xwayland);
  server->xwayland = NULL;
}

const char *leme_xwayland_display(const struct leme_server *server) {
  if (server->xwayland == NULL || server->xwayland->wlr_xwayland == NULL ||
      server->xwayland->wlr_xwayland->display_name == NULL ||
      server->xwayland->wlr_xwayland->display_name[0] == '\0') {
    return NULL;
  }
  return server->xwayland->wlr_xwayland->display_name;
}

static struct wlr_box leme_xwayland_workarea_union(struct leme_server *server) {
  struct leme_output *output;
  struct wlr_box result = {0};
  bool any = false;

  if (server->outputs.next == NULL) {
    return result;
  }
  wl_list_for_each(output, &server->outputs, link) {
    struct leme_box usable;
    struct wlr_box box;

    if (!output->wlr_output->enabled) {
      continue;
    }
    usable = leme_output_usable_box(output);
    box = (struct wlr_box){
        .x = usable.x,
        .y = usable.y,
        .width = usable.width,
        .height = usable.height,
    };
    if (wlr_box_empty(&box)) {
      continue;
    }
    if (!any) {
      result = box;
      any = true;
      continue;
    }
    if (box.x < result.x) {
      result.width += result.x - box.x;
      result.x = box.x;
    }
    if (box.y < result.y) {
      result.height += result.y - box.y;
      result.y = box.y;
    }
    if (box.x + box.width > result.x + result.width) {
      result.width = box.x + box.width - result.x;
    }
    if (box.y + box.height > result.y + result.height) {
      result.height = box.y + box.height - result.y;
    }
  }
  return result;
}

void leme_xwayland_update_workarea(struct leme_server *server) {
  struct leme_xwayland *xwayland = server->xwayland;

  if (xwayland == NULL || xwayland->wlr_xwayland == NULL ||
      !xwayland->is_ready ||
      wlr_xwayland_get_xwm_connection(xwayland->wlr_xwayland) == NULL) {
    return;
  }
  if (leme_output_focused(server) == NULL) {
    wlr_xwayland_set_workareas(xwayland->wlr_xwayland, NULL, 0);
  } else {
    struct wlr_box box = leme_xwayland_workarea_union(server);

    wlr_xwayland_set_workareas(xwayland->wlr_xwayland, &box, 1);
  }
}

void leme_xwayland_set_root_cursor(struct leme_server *server,
                                   struct wlr_xcursor_manager *manager,
                                   const char *name, float scale) {
  struct leme_xwayland *xwayland = server == NULL ? NULL : server->xwayland;
  struct wlr_xcursor *xcursor;
  struct wlr_buffer *buffer;

  if (xwayland == NULL || xwayland->wlr_xwayland == NULL || manager == NULL ||
      name == NULL) {
    return;
  }
  xcursor = wlr_xcursor_manager_get_xcursor(manager, name, scale);
  if (xcursor == NULL || xcursor->image_count == 0) {
    return;
  }
  buffer = wlr_xcursor_image_get_buffer(xcursor->images[0]);
  if (buffer == NULL) {
    return;
  }
  wlr_xwayland_set_cursor(xwayland->wlr_xwayland, buffer,
                          (int32_t)xcursor->images[0]->hotspot_x,
                          (int32_t)xcursor->images[0]->hotspot_y);
}
