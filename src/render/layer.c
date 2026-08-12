#include "render/render.h"

#include "shell/layer.h"
#include "output/output.h"
#include "core/server.h"
#include "protocols/session.h"

#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>

static struct wlr_scene_tree *
leme_render_layer_parent(struct leme_server *server, uint32_t layer)
{
    switch (layer) {
    case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
        return server->scene_background;
    case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
        return server->scene_bottom;
    case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
        return server->scene_top;
    case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
        return server->scene_overlay;
    default:
        return NULL;
    }
}

bool
leme_render_layer_create(struct leme_layer_surface *layer)
{
    struct wlr_scene_tree *parent = leme_render_layer_parent(
        layer->server, layer->wlr_layer_surface->pending.layer);

    if (parent == NULL) {
        return false;
    }
    layer->render_tree = wlr_scene_tree_create(parent);
    if (layer->render_tree == NULL) {
        return false;
    }
    layer->render_tree->node.data = layer;
    layer->scene_layer_surface = wlr_scene_layer_surface_v1_create(
        layer->render_tree, layer->wlr_layer_surface);
    if (layer->scene_layer_surface == NULL) {
        leme_render_layer_destroy(layer);
        return false;
    }
    leme_session_refresh_idle_inhibitors(layer->server);
    return true;
}

void
leme_render_layer_destroy(struct leme_layer_surface *layer)
{
    if (layer->render_tree != NULL) {
        wlr_scene_node_destroy(&layer->render_tree->node);
    }
    layer->render_tree = NULL;
    layer->scene_layer_surface = NULL;
    leme_session_refresh_idle_inhibitors(layer->server);
}

void
leme_render_layer_configure(struct leme_layer_surface *layer,
    struct leme_box full, struct leme_box *usable)
{
    struct leme_layer_popup *popup;
    struct wlr_box full_box = {
        .x = full.x,
        .y = full.y,
        .width = full.width,
        .height = full.height,
    };
    struct wlr_box usable_box = {
        .x = usable->x,
        .y = usable->y,
        .width = usable->width,
        .height = usable->height,
    };

    if (layer->scene_layer_surface == NULL) {
        return;
    }
    wlr_scene_layer_surface_v1_configure(
        layer->scene_layer_surface, &full_box, &usable_box);
    wl_list_for_each(popup, &layer->popups, link) {
        leme_render_layer_popup_unconstrain(popup, full);
        leme_render_layer_popup_update(popup);
    }
    *usable = (struct leme_box){
        .x = usable_box.x,
        .y = usable_box.y,
        .width = usable_box.width,
        .height = usable_box.height,
    };
}

bool
leme_render_layer_popup_create(struct leme_layer_popup *popup)
{
    if (popup->layer->scene_layer_surface == NULL) {
        return false;
    }
    popup->scene_tree = wlr_scene_xdg_surface_create(
        popup->layer->scene_layer_surface->tree, popup->wlr_popup->base);
    if (popup->scene_tree == NULL) {
        return false;
    }
    leme_render_layer_popup_update(popup);
    leme_session_refresh_idle_inhibitors(popup->layer->server);
    return true;
}

void
leme_render_layer_popup_update(struct leme_layer_popup *popup)
{
    double popup_x;
    double popup_y;
    int layer_x;
    int layer_y;

    if (popup->scene_tree == NULL) {
        return;
    }
    wlr_xdg_popup_get_position(popup->wlr_popup, &popup_x, &popup_y);
    wlr_xdg_popup_get_toplevel_coords(popup->wlr_popup,
        (int)popup_x, (int)popup_y, &layer_x, &layer_y);
    wlr_scene_node_set_position(&popup->scene_tree->node, layer_x, layer_y);
}

void
leme_render_layer_popup_destroy(struct leme_layer_popup *popup)
{
    if (popup->scene_tree != NULL) {
        wlr_scene_node_destroy(&popup->scene_tree->node);
        popup->scene_tree = NULL;
    }
    leme_session_refresh_idle_inhibitors(popup->layer->server);
}

void
leme_render_layer_popup_unconstrain(struct leme_layer_popup *popup,
    struct leme_box full)
{
    struct wlr_scene_tree *layer_tree =
        popup->layer->scene_layer_surface->tree;
    struct wlr_box constraint = {
        .x = full.x - layer_tree->node.x,
        .y = full.y - layer_tree->node.y,
        .width = full.width,
        .height = full.height,
    };

    wlr_xdg_popup_unconstrain_from_box(popup->wlr_popup, &constraint);
}

void
leme_render_layers_refresh_output(struct leme_server *server)
{
    struct leme_layer_surface *layer;

    wl_list_for_each(layer, &server->layer_surfaces, link) {
        if (layer->render_tree != NULL) {
            wlr_scene_node_set_enabled(&layer->render_tree->node,
                layer->output != NULL &&
                layer->output->wlr_output->enabled);
        }
    }
    leme_session_refresh_idle_inhibitors(server);
}

void
leme_render_layer_update_tree(struct leme_layer_surface *layer)
{
    struct wlr_scene_tree *parent;

    if (layer->render_tree == NULL) {
        return;
    }
    parent = leme_render_layer_parent(
        layer->server, layer->wlr_layer_surface->current.layer);
    if (parent != NULL && layer->render_tree->node.parent != parent) {
        wlr_scene_node_reparent(&layer->render_tree->node, parent);
    }
}
