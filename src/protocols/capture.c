#include "core/gate.h"
#include "protocols/capture.h"

#include "core/server.h"
#include "output/output.h"
#include "protocols/session.h"
#include "protocols/toplevel.h"
#include "shell/view.h"

#include <stdlib.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/util/log.h>

struct leme_capture {
    struct leme_server *server;
    struct wlr_screencopy_manager_v1 *screencopy;
    struct wlr_ext_output_image_capture_source_manager_v1 *output_source;
    struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1
        *toplevel_source;
    struct wlr_ext_image_copy_capture_manager_v1 *image_copy;
    struct wl_listener toplevel_request;
};

static void
leme_capture_handle_epoch_destroy(struct wl_listener *listener, void *data)
{
    struct leme_view *view = wl_container_of(listener, view,
        capture_epoch_destroy);

    (void)data;
    view->capture_epoch = NULL;
    view->capture_epoch_destroy_count++;
    wl_list_remove(&listener->link);
    wl_list_init(&listener->link);
}

static struct wlr_scene_node *
leme_capture_previous_sibling(struct wlr_scene_node *node)
{
    if (node->link.prev == &node->parent->children) {
        return NULL;
    }
    return wl_container_of(node->link.prev, node, link);
}

static struct wlr_scene_node *
leme_capture_next_sibling(struct wlr_scene_node *node)
{
    if (node->link.next == &node->parent->children) {
        return NULL;
    }
    return wl_container_of(node->link.next, node, link);
}

static void
leme_capture_place_at_slot(struct wlr_scene_node *node,
    struct wlr_scene_node *below, struct wlr_scene_node *above)
{
    if (below != NULL) {
        wlr_scene_node_place_above(node, below);
    } else if (above != NULL) {
        wlr_scene_node_place_below(node, above);
    }
}

static bool
leme_capture_epoch_create(struct leme_view *view)
{
    struct wlr_scene_tree *epoch;
    struct wlr_scene_node *below;
    struct wlr_scene_node *above;

    if (view->capture_epoch != NULL) {
        return true;
    }
    if (view->render_tree == NULL || view->scene_tree == NULL) {
        return false;
    }
    below = leme_capture_previous_sibling(&view->scene_tree->node);
    above = leme_capture_next_sibling(&view->scene_tree->node);
    epoch = wlr_scene_tree_create(view->render_tree);
    if (epoch == NULL) {
        return false;
    }
    wlr_scene_node_reparent(&view->scene_tree->node, epoch);
    leme_capture_place_at_slot(&epoch->node, below, above);
    view->capture_epoch = epoch;
    view->capture_epoch_destroy.notify = leme_capture_handle_epoch_destroy;
    wl_signal_add(&epoch->node.events.destroy, &view->capture_epoch_destroy);
    return true;
}

void
leme_capture_invalidate_view(struct leme_view *view)
{
    struct wlr_scene_tree *epoch;

    if (view == NULL || view->capture_epoch == NULL) {
        return;
    }
    epoch = view->capture_epoch;
    view->capture_epoch = NULL;
    if (view->scene_tree != NULL && view->render_tree != NULL) {
        struct wlr_scene_node *below =
            leme_capture_previous_sibling(&epoch->node);
        struct wlr_scene_node *above = leme_capture_next_sibling(&epoch->node);

        wlr_scene_node_reparent(&view->scene_tree->node, view->render_tree);
        leme_capture_place_at_slot(&view->scene_tree->node, below, above);
    }
    wlr_scene_node_destroy(&epoch->node);
}

void
leme_capture_invalidate_all(struct leme_server *server)
{
    struct leme_view *view;

    if (server == NULL) {
        return;
    }
    wl_list_for_each(view, &server->views, link) {
        leme_capture_invalidate_view(view);
    }
}

void
leme_capture_reconcile_outputs(struct leme_server *server)
{
    struct leme_view *view;

    if (server == NULL) {
        return;
    }
    wl_list_for_each(view, &server->views, link) {
        if (view->capture_epoch != NULL &&
                !leme_capture_view_eligible(server, view)) {
            leme_capture_invalidate_view(view);
        }
    }
}

bool
leme_capture_view_eligible(const struct leme_server *server,
    const struct leme_view *view)
{
    const struct leme_output *output;

    if (server == NULL || view == NULL || view->server != server ||
            leme_session_locked(server) || !leme_view_protocol_eligible(view) ||
            view->scene_tree == NULL) {
        return false;
    }
    output = leme_view_output(view);
    return output != NULL && output->scene_output != NULL &&
        output->wlr_output != NULL && output->wlr_output->enabled;
}

static bool
leme_capture_request_accept(struct leme_capture *capture,
    struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request
        *request,
    struct wlr_ext_image_capture_source_v1 *source)
{
    (void)capture;
    if (leme_gate_capture_accept != NULL && !leme_gate_capture_accept()) {
        return false;
    }
    return wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request_accept(
        request, source);
}

static void
leme_capture_handle_toplevel_request(struct wl_listener *listener, void *data)
{
    struct leme_capture *capture =
        wl_container_of(listener, capture, toplevel_request);
    struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request
        *request = data;
    struct wlr_ext_image_capture_source_v1 *source;
    struct leme_server *server = capture->server;
    struct leme_view *view;

    if (request == NULL) {
        return;
    }
    view = leme_toplevel_view_from_handle(request->toplevel_handle);
    if (!leme_capture_view_eligible(server, view)) {
        return;
    }
    if (!leme_capture_epoch_create(view)) {
        wlr_log(WLR_ERROR, "%s",
            "leme: failed to create a toplevel capture epoch");
        return;
    }
    source = wlr_ext_image_capture_source_v1_create_with_scene_node(
        &view->capture_epoch->node, wl_display_get_event_loop(server->display),
        server->allocator, server->renderer);
    if (source == NULL) {
        leme_capture_invalidate_view(view);
        wlr_log(WLR_ERROR, "%s",
            "leme: failed to create a toplevel capture source");
        return;
    }
    if (!leme_capture_request_accept(capture, request, source)) {
        leme_capture_invalidate_view(view);
        wlr_log(WLR_ERROR, "%s",
            "leme: failed to accept a toplevel capture source");
    }
}

bool
leme_capture_init(struct leme_server *server)
{
    struct leme_capture *capture = calloc(1, sizeof(*capture));

    if (capture == NULL) {
        wlr_log(WLR_ERROR, "%s",
            "leme: failed to create output capture protocols");
        return false;
    }
    capture->server = server;
    capture->screencopy = wlr_screencopy_manager_v1_create(server->display);
    capture->output_source =
        wlr_ext_output_image_capture_source_manager_v1_create(
            server->display, 1);
    capture->toplevel_source =
        wlr_ext_foreign_toplevel_image_capture_source_manager_v1_create(
            server->display, 1);
    capture->image_copy = wlr_ext_image_copy_capture_manager_v1_create(
        server->display, 1);
    if (capture->screencopy == NULL || capture->output_source == NULL ||
            capture->toplevel_source == NULL ||
            capture->image_copy == NULL) {
        wlr_log(WLR_ERROR, "%s",
            "leme: failed to create output capture protocols");
        free(capture);
        return false;
    }
    capture->toplevel_request.notify = leme_capture_handle_toplevel_request;
    wl_signal_add(&capture->toplevel_source->events.new_request,
        &capture->toplevel_request);
    server->capture = capture;
    return true;
}

void
leme_capture_finish(struct leme_server *server)
{
    if (server->capture == NULL) {
        return;
    }
    wl_list_remove(&server->capture->toplevel_request.link);
    free(server->capture);
    server->capture = NULL;
}
