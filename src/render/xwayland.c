#include "render/render.h"

#include "core/server.h"
#include "shell/view.h"

#include <wlr/types/wlr_scene.h>
#include <wlr/xwayland/xwayland.h>

bool
leme_render_xwayland_create(struct leme_view *view)
{
    if (view == NULL || view->render_tree == NULL ||
            view->xwayland_surface == NULL ||
            view->xwayland_surface->surface == NULL) {
        return false;
    }
    view->scene_tree = wlr_scene_subsurface_tree_create(
        view->render_tree, view->xwayland_surface->surface);
    return view->scene_tree != NULL;
}

void
leme_render_xwayland_set_geometry(struct leme_view *view)
{
    struct leme_box content;

    if (view == NULL || view->scene_tree == NULL) {
        return;
    }
    content = leme_render_view_content_box(view, view->box);
    wlr_scene_node_set_position(&view->scene_tree->node,
        content.x - view->box.x, content.y - view->box.y);
}

void
leme_render_xwayland_destroy(struct leme_view *view)
{
    if (view != NULL && view->render_tree != NULL) {
        wlr_scene_node_destroy(&view->render_tree->node);
    }
    if (view != NULL) {
        view->render_tree = NULL;
        view->scene_tree = NULL;
    }
}
