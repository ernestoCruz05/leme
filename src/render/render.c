#include "render/render.h"

#include "protocols/data.h"
#include "shell/layer.h"
#include "output/output.h"
#include "core/server.h"
#include "protocols/tearing.h"
#include "shell/view.h"

#include <time.h>
#include <wlr/render/swapchain.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>

static void
leme_render_handle_output_layout_change(
    struct wl_listener *listener, void *data)
{
    struct leme_server *server =
        wl_container_of(listener, server, render_output_layout_change);
    struct leme_output *output;

    (void)data;
    if (server->outputs.next == NULL) {
        return;
    }
    wl_list_for_each(output, &server->outputs, link) {
        struct wlr_output_layout_output *layout_output;

        if (output->scene_output == NULL) {
            continue;
        }
        layout_output = wlr_output_layout_get(
            server->output_layout, output->wlr_output);
        if (layout_output != NULL) {
            wlr_scene_output_set_position(output->scene_output,
                layout_output->x, layout_output->y);
        }
    }
}

void
leme_render_init(struct leme_server *server)
{
    server->scene = wlr_scene_create();
    if (server->scene == NULL) {
        return;
    }
    server->scene_output_layout = wlr_scene_attach_output_layout(
        server->scene, server->output_layout);
    server->scene_background = wlr_scene_tree_create(&server->scene->tree);
    server->scene_bottom = wlr_scene_tree_create(&server->scene->tree);
    server->scene_tiled = wlr_scene_tree_create(&server->scene->tree);
    server->scene_floating = wlr_scene_tree_create(&server->scene->tree);
    server->scene_top = wlr_scene_tree_create(&server->scene->tree);
    server->scene_overlay = wlr_scene_tree_create(&server->scene->tree);
    server->scene_drag = wlr_scene_tree_create(&server->scene->tree);
    server->scene_lock = wlr_scene_tree_create(&server->scene->tree);
    if (server->scene_output_layout == NULL ||
            server->scene_background == NULL || server->scene_bottom == NULL ||
            server->scene_tiled == NULL || server->scene_floating == NULL ||
            server->scene_top == NULL || server->scene_overlay == NULL ||
            server->scene_drag == NULL || server->scene_lock == NULL) {
        leme_render_finish(server);
        return;
    }
    server->render_output_layout_change.notify =
        leme_render_handle_output_layout_change;
    wl_signal_add(&server->output_layout->events.change,
        &server->render_output_layout_change);
}

void
leme_render_finish(struct leme_server *server)
{
    if (server->render_output_layout_change.link.next != NULL) {
        wl_list_remove(&server->render_output_layout_change.link);
        server->render_output_layout_change.link.next = NULL;
        server->render_output_layout_change.link.prev = NULL;
    }
    if (server->scene != NULL) {
        wlr_scene_node_destroy(&server->scene->tree.node);
    }
    server->scene = NULL;
    server->scene_output_layout = NULL;
    server->scene_background = NULL;
    server->scene_bottom = NULL;
    server->scene_tiled = NULL;
    server->scene_floating = NULL;
    server->scene_top = NULL;
    server->scene_overlay = NULL;
    server->scene_drag = NULL;
    server->scene_lock = NULL;
}

bool
leme_render_prepare_output_attachment(struct leme_output *output)
{
    if (output->scene_output != NULL) {
        return true;
    }
    if (output->server->scene == NULL) {
        return false;
    }
    output->scene_output = wlr_scene_output_create(
        output->server->scene, output->wlr_output);
    return output->scene_output != NULL;
}

bool
leme_render_attach_output(struct leme_output *output)
{
    struct leme_server *server = output->server;
    struct wlr_output_layout_output *layout_output;

    if (!leme_render_prepare_output_attachment(output)) {
        return false;
    }
    layout_output = wlr_output_layout_get(
        server->output_layout, output->wlr_output);
    if (layout_output == NULL) {
        return false;
    }
    if (server->scene_output_layout != NULL) {
        wlr_scene_output_layout_add_output(server->scene_output_layout,
            layout_output, output->scene_output);
    }
    return true;
}

void
leme_render_position_output(struct leme_output *output)
{
    if (output->scene_output != NULL) {
        wlr_scene_output_set_position(output->scene_output,
            output->layout_x, output->layout_y);
    }
}

void
leme_render_detach_output(struct leme_output *output)
{
    if (output->scene_output != NULL) {
        wlr_scene_output_destroy(output->scene_output);
        output->scene_output = NULL;
    }
}

static bool
leme_render_prepare_isolated_output_state(struct leme_output *output,
    struct wlr_output_state *state)
{
    struct wlr_scene *scene = wlr_scene_create();
    struct wlr_scene_output *scene_output = NULL;
    struct wlr_swapchain *swapchain = NULL;
    bool valid = false;

    if (scene == NULL) {
        return false;
    }
    scene_output = wlr_scene_output_create(scene, output->wlr_output);
    if (scene_output == NULL ||
            !wlr_output_configure_primary_swapchain(
                output->wlr_output, state, &swapchain)) {
        goto cleanup;
    }
    valid = wlr_scene_output_build_state(scene_output, state,
        &(struct wlr_scene_output_state_options){
            .swapchain = swapchain,
        });

cleanup:
    if (scene_output != NULL) {
        wlr_scene_output_destroy(scene_output);
    }
    wlr_scene_node_destroy(&scene->tree.node);
    wlr_swapchain_destroy(swapchain);
    return valid;
}

bool
leme_render_prepare_output_state(struct leme_output *output,
    struct wlr_output_state *state, bool testing)
{
    bool enabled = output->wlr_output->enabled;

    if (state->committed & WLR_OUTPUT_STATE_ENABLED) {
        enabled = state->enabled;
    }
    if (!enabled) {
        return true;
    }
    if (testing || output->scene_output == NULL) {
        return leme_render_prepare_isolated_output_state(output, state);
    }
    return wlr_scene_output_build_state(output->scene_output, state, NULL);
}

void
leme_render_handle_session_active(struct leme_server *server, bool active)
{
    if (server != NULL && !active) {
        leme_animation_manager_finish_all(&server->animations);
    }
}

void
leme_render_output_frame(struct leme_output *output)
{
    struct wlr_output_state state;
    struct timespec now;
    bool request_tearing;
    bool committed;

    if (output->scene_output == NULL) {
        return;
    }
    /* Um único configure por frame, em vez de um por evento de ponteiro. */
    leme_view_flush_deferred_configures(output->server);
    clock_gettime(CLOCK_MONOTONIC, &now);
    leme_animation_manager_tick(&output->server->animations, &now);
    if (leme_animation_manager_active(&output->server->animations)) {
        wlr_output_schedule_frame(output->wlr_output);
    }
    if (!output->tearing_fallback &&
            !wlr_scene_output_needs_frame(output->scene_output)) {
        return;
    }
    wlr_output_state_init(&state);
    if (!wlr_scene_output_build_state(output->scene_output, &state, NULL)) {
        wlr_output_state_finish(&state);
        return;
    }
    request_tearing = !output->tearing_fallback &&
        leme_tearing_can_tear(output->server, output);
    if (output->tearing_fallback) {
        output->tearing_fallback = false;
    }
    if (request_tearing) {
        state.tearing_page_flip = true;
        if (!wlr_output_test_state(output->wlr_output, &state)) {
            state.tearing_page_flip = false;
        }
    }
    committed = wlr_output_commit_state(output->wlr_output, &state);
    if (!committed && state.tearing_page_flip) {
        output->tearing_fallback = true;
        wlr_output_schedule_frame(output->wlr_output);
    }
    wlr_output_state_finish(&state);
    if (!committed) {
        return;
    }
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(output->scene_output, &now);
}

bool
leme_render_drag_icon_create(struct leme_drag_icon *icon)
{
    icon->scene_tree = wlr_scene_drag_icon_create(
        icon->data->server->scene_drag, icon->wlr_drag_icon);
    return icon->scene_tree != NULL;
}

void
leme_render_drag_icon_update(struct leme_drag_icon *icon,
    double layout_x, double layout_y)
{
    if (icon->scene_tree != NULL) {
        wlr_scene_node_set_position(&icon->scene_tree->node,
            (int)layout_x, (int)layout_y);
    }
}

void
leme_render_drag_icon_destroy(struct leme_drag_icon *icon)
{
    if (icon->scene_tree != NULL) {
        wlr_scene_node_destroy(&icon->scene_tree->node);
        icon->scene_tree = NULL;
    }
}

struct leme_render_visibility {
    struct wlr_surface *root;
    bool visible;
};

static void
leme_render_check_visible_buffer(struct wlr_scene_buffer *buffer,
    int sx, int sy, void *data)
{
    struct leme_render_visibility *visibility = data;
    struct wlr_scene_surface *scene_surface;

    (void)sx;
    (void)sy;
    if (visibility->visible) {
        return;
    }
    scene_surface = wlr_scene_surface_try_from_buffer(buffer);
    if (scene_surface != NULL &&
            wlr_surface_get_root_surface(scene_surface->surface) ==
                visibility->root) {
        visibility->visible = true;
    }
}

bool
leme_render_surface_visible(struct leme_server *server,
    struct wlr_surface *surface)
{
    struct leme_render_visibility visibility;

    if (surface == NULL || leme_output_focused(server) == NULL ||
            leme_output_focused(server)->scene_output == NULL ||
            !leme_output_focused(server)->wlr_output->enabled) {
        return false;
    }
    visibility = (struct leme_render_visibility){
        .root = wlr_surface_get_root_surface(surface),
    };
    wlr_scene_output_for_each_buffer(leme_output_focused(server)->scene_output,
        leme_render_check_visible_buffer, &visibility);
    return visibility.visible;
}

bool
leme_render_at(struct leme_server *server, double lx, double ly,
    struct leme_render_hit *hit)
{
    struct wlr_scene_node *node;
    struct wlr_scene_node *ancestor;
    struct wlr_scene_surface *scene_surface;

    *hit = (struct leme_render_hit){0};
    node = wlr_scene_node_at(&server->scene->tree.node,
        lx, ly, &hit->surface_x, &hit->surface_y);
    if (node == NULL) {
        return false;
    }
    if (node->type == WLR_SCENE_NODE_BUFFER) {
        scene_surface = wlr_scene_surface_try_from_buffer(
            wlr_scene_buffer_from_node(node));
        if (scene_surface != NULL) {
            hit->surface = scene_surface->surface;
        }
    }
    for (ancestor = node; ancestor != NULL;
            ancestor = ancestor->parent == NULL ? NULL :
            &ancestor->parent->node) {
        if ((ancestor->parent == server->scene_tiled ||
                ancestor->parent == server->scene_floating) &&
                ancestor->data != NULL) {
            struct leme_view *view = ancestor->data;
            hit->view = view->mapped ? view : NULL;
            break;
        }
        if ((ancestor->parent == server->scene_background ||
                ancestor->parent == server->scene_bottom ||
                ancestor->parent == server->scene_top ||
                ancestor->parent == server->scene_overlay) &&
                ancestor->data != NULL) {
            struct leme_layer_surface *layer = ancestor->data;
            hit->layer = layer->mapped ? layer : NULL;
            break;
        }
    }
    return hit->surface != NULL;
}
