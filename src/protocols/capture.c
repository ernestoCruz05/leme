#include "core/gate.h"
#include "protocols/capture.h"

#include "core/server.h"
#include "output/output.h"
#include "protocols/session.h"
#include "protocols/toplevel.h"
#include "render/render.h"
#include "shell/view.h"

#include <stdlib.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/util/log.h>
#include <wlr/xwayland/xwayland.h>

struct leme_capture {
  struct leme_server *server;
  struct wlr_screencopy_manager_v1 *screencopy;
  struct wlr_ext_output_image_capture_source_manager_v1 *output_source;
  struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1
      *toplevel_source;
  struct wlr_ext_image_copy_capture_manager_v1 *image_copy;
  struct wl_listener toplevel_request;
};

static const float leme_capture_blank_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};

static bool leme_capture_view_visible(const struct leme_view *view) {
  int x = 0;
  int y = 0;

  return view->render_tree != NULL &&
         wlr_scene_node_coords(&view->render_tree->node, &x, &y);
}

static void leme_capture_sync_view(struct leme_view *view) {
  bool visible;

  if (view->capture_content == NULL || view->capture_blank == NULL) {
    return;
  }
  visible = leme_capture_view_visible(view);
  if (!visible) {
    struct leme_box content = leme_render_view_content_box(view, view->box);

    wlr_scene_rect_set_size(view->capture_blank, content.width, content.height);
  }
  wlr_scene_node_set_enabled(&view->capture_content->node, visible);
  wlr_scene_node_set_enabled(&view->capture_blank->node, !visible);
}

static struct wlr_scene_tree *
leme_capture_popup_parent(struct leme_view *view, struct wlr_surface *parent) {
  struct leme_view_popup *popup;

  if (view->kind == LEME_VIEW_XDG && view->xdg_toplevel != NULL &&
      view->xdg_toplevel->base->surface == parent) {
    return view->capture_content;
  }
  wl_list_for_each(popup, &view->popups, link) {
    if (popup->wlr_popup->base->surface == parent) {
      return popup->capture_tree;
    }
  }
  return NULL;
}

void leme_capture_popup_create(struct leme_view_popup *popup) {
  struct wlr_scene_tree *parent;

  if (popup == NULL || popup->view->capture_scene == NULL ||
      popup->capture_tree != NULL) {
    return;
  }
  parent = leme_capture_popup_parent(popup->view, popup->wlr_popup->parent);
  if (parent == NULL) {
    return;
  }
  popup->capture_tree =
      wlr_scene_xdg_surface_create(parent, popup->wlr_popup->base);
}

void leme_capture_popup_destroy(struct leme_view_popup *popup) {
  if (popup == NULL || popup->capture_tree == NULL) {
    return;
  }
  wlr_scene_node_destroy(&popup->capture_tree->node);
  popup->capture_tree = NULL;
}

void leme_capture_invalidate_view(struct leme_view *view) {
  struct leme_view_popup *popup;
  struct wlr_scene *scene;

  if (view == NULL || view->capture_scene == NULL) {
    return;
  }
  scene = view->capture_scene;
  wl_list_for_each(popup, &view->popups, link) { popup->capture_tree = NULL; }
  view->capture_scene = NULL;
  view->capture_content = NULL;
  view->capture_blank = NULL;
  view->capture_source = NULL;
  wlr_scene_node_destroy(&scene->tree.node);
}

static bool leme_capture_scene_create(struct leme_view *view) {
  struct leme_server *server = view->server;
  struct leme_view_popup *popup;
  struct wlr_scene *scene = wlr_scene_create();

  if (scene == NULL) {
    return false;
  }
  view->capture_scene = scene;
  view->capture_blank =
      wlr_scene_rect_create(&scene->tree, 0, 0, leme_capture_blank_color);
  if (view->kind == LEME_VIEW_XDG && view->xdg_toplevel != NULL) {
    view->capture_content =
        wlr_scene_xdg_surface_create(&scene->tree, view->xdg_toplevel->base);
  } else if (view->xwayland_surface != NULL &&
             view->xwayland_surface->surface != NULL) {
    view->capture_content = wlr_scene_subsurface_tree_create(
        &scene->tree, view->xwayland_surface->surface);
  }
  if (view->capture_blank == NULL || view->capture_content == NULL) {
    leme_capture_invalidate_view(view);
    return false;
  }
  wl_list_for_each_reverse(popup, &view->popups, link) {
    leme_capture_popup_create(popup);
  }
  view->capture_source = wlr_ext_image_capture_source_v1_create_with_scene_node(
      &scene->tree.node, wl_display_get_event_loop(server->display),
      server->allocator, server->renderer);
  if (view->capture_source == NULL) {
    leme_capture_invalidate_view(view);
    return false;
  }
  leme_capture_sync_view(view);
  return true;
}

void leme_capture_invalidate_all(struct leme_server *server) {
  struct leme_view *view;

  if (server == NULL) {
    return;
  }
  wl_list_for_each(view, &server->views, link) {
    leme_capture_invalidate_view(view);
  }
}

void leme_capture_sync(struct leme_server *server) {
  struct leme_view *view;

  if (server == NULL) {
    return;
  }
  wl_list_for_each(view, &server->views, link) { leme_capture_sync_view(view); }
}

bool leme_capture_view_eligible(const struct leme_server *server,
                                const struct leme_view *view) {
  const struct leme_output *output;

  if (server == NULL || view == NULL || view->server != server ||
      leme_session_locked(server) ||
      !leme_ownership_direct_capture_eligible(view) ||
      view->scene_tree == NULL) {
    return false;
  }
  output = leme_view_output(view);
  return output != NULL && output->scene_output != NULL &&
         output->wlr_output != NULL && output->wlr_output->enabled;
}

static bool leme_capture_request_accept(
    struct leme_capture *capture,
    struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request
        *request,
    struct wlr_ext_image_capture_source_v1 *source) {
  (void)capture;
  if (leme_gate_capture_accept != NULL && !leme_gate_capture_accept()) {
    return false;
  }
  return wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request_accept(
      request, source);
}

static void leme_capture_handle_toplevel_request(struct wl_listener *listener,
                                                 void *data) {
  struct leme_capture *capture =
      wl_container_of(listener, capture, toplevel_request);
  struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request
      *request = data;
  struct leme_server *server = capture->server;
  struct leme_view *view;

  if (request == NULL) {
    return;
  }
  view = leme_toplevel_view_from_handle(request->toplevel_handle);
  if (!leme_capture_view_eligible(server, view)) {
    return;
  }
  if (view->capture_source == NULL && !leme_capture_scene_create(view)) {
    wlr_log(WLR_ERROR, "%s",
            "leme: failed to create a toplevel capture source");
    return;
  }
  if (!leme_capture_request_accept(capture, request, view->capture_source)) {
    wlr_log(WLR_ERROR, "%s",
            "leme: failed to accept a toplevel capture source");
  }
}

bool leme_capture_init(struct leme_server *server) {
  struct leme_capture *capture = calloc(1, sizeof(*capture));

  if (capture == NULL) {
    wlr_log(WLR_ERROR, "%s", "leme: failed to create output capture protocols");
    return false;
  }
  capture->server = server;
  capture->screencopy = wlr_screencopy_manager_v1_create(server->display);
  capture->output_source =
      wlr_ext_output_image_capture_source_manager_v1_create(server->display, 1);
  capture->toplevel_source =
      wlr_ext_foreign_toplevel_image_capture_source_manager_v1_create(
          server->display, 1);
  capture->image_copy =
      wlr_ext_image_copy_capture_manager_v1_create(server->display, 1);
  if (capture->screencopy == NULL || capture->output_source == NULL ||
      capture->toplevel_source == NULL || capture->image_copy == NULL) {
    wlr_log(WLR_ERROR, "%s", "leme: failed to create output capture protocols");
    free(capture);
    return false;
  }
  capture->toplevel_request.notify = leme_capture_handle_toplevel_request;
  wl_signal_add(&capture->toplevel_source->events.new_request,
                &capture->toplevel_request);
  server->capture = capture;
  return true;
}

void leme_capture_finish(struct leme_server *server) {
  if (server->capture == NULL) {
    return;
  }
  wl_list_remove(&server->capture->toplevel_request.link);
  free(server->capture);
  server->capture = NULL;
}
