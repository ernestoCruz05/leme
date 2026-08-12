#include "render/render.h"

#include "output/output.h"
#include "core/server.h"
#include "protocols/session.h"

#include <limits.h>
#include <stdint.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_session_lock_v1.h>

static struct wlr_box
leme_render_session_layout_box(const struct leme_session *session)
{
    struct wlr_box box = {0};

    wlr_output_layout_get_box(session->server->output_layout, NULL, &box);
    return box;
}

static void
leme_render_session_set_box(
    struct leme_session *session, struct leme_box box)
{
    if (session->lock_blocker == NULL) {
        return;
    }
    wlr_scene_rect_set_size(session->lock_blocker, box.width, box.height);
    wlr_scene_node_set_position(
        &session->lock_blocker->node, box.x, box.y);
}

bool
leme_render_session_create(struct leme_session *session)
{
    static const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};

    session->lock_blocker = wlr_scene_rect_create(
        session->server->scene_lock, 0, 0, black);
    if (session->lock_blocker == NULL) {
        return false;
    }
    wlr_scene_node_set_enabled(&session->server->scene_lock->node, false);
    return true;
}

void
leme_render_session_destroy(struct leme_session *session)
{
    if (session->lock_blocker != NULL) {
        wlr_scene_node_destroy(&session->lock_blocker->node);
        session->lock_blocker = NULL;
    }
}

void
leme_render_session_set_locked(struct leme_session *session, bool locked)
{
    struct wlr_box layout_box = leme_render_session_layout_box(session);
    struct leme_box box = {
        .x = layout_box.x,
        .y = layout_box.y,
        .width = layout_box.width,
        .height = layout_box.height,
    };

    if (locked) {
        /* O done() da área de trabalho limpa todas as máscaras de
         * apresentação de forma síncrona; o conteúdo do bloqueio não pode
         * aparecer antes desse desmantelamento. */
        leme_animation_manager_finish_all(&session->server->animations);
    }
    if (session->lock_blocker == NULL) {
        return;
    }
    leme_render_session_set_box(session, box);
    wlr_scene_node_set_enabled(
        &session->server->scene_lock->node, locked);
}

static bool
leme_render_session_box_edges(
    struct leme_box box, int64_t *right, int64_t *bottom)
{
    if (box.width < 0 || box.height < 0) {
        return false;
    }
    *right = (int64_t)box.x + (int64_t)box.width;
    *bottom = (int64_t)box.y + (int64_t)box.height;
    return *right >= INT_MIN && *right <= INT_MAX &&
        *bottom >= INT_MIN && *bottom <= INT_MAX;
}

bool
leme_render_session_prepare_output_wake(
    struct leme_session *session, struct leme_box box)
{
    struct wlr_box layout_box = leme_render_session_layout_box(session);
    int64_t box_right;
    int64_t box_bottom;

    if (session->lock_blocker == NULL ||
            !leme_render_session_box_edges(
                box, &box_right, &box_bottom)) {
        return false;
    }
    if (layout_box.width > 0 && layout_box.height > 0) {
        struct leme_box layout = {
            .x = layout_box.x,
            .y = layout_box.y,
            .width = layout_box.width,
            .height = layout_box.height,
        };
        int64_t layout_right;
        int64_t layout_bottom;
        int64_t left;
        int64_t top;
        int64_t right;
        int64_t bottom;
        int64_t width;
        int64_t height;

        if (!leme_render_session_box_edges(
                layout, &layout_right, &layout_bottom)) {
            return false;
        }
        left = layout.x < box.x ? layout.x : box.x;
        top = layout.y < box.y ? layout.y : box.y;
        right = layout_right > box_right ? layout_right : box_right;
        bottom = layout_bottom > box_bottom ? layout_bottom : box_bottom;
        width = right - left;
        height = bottom - top;
        if (width < 0 || width > INT_MAX ||
                height < 0 || height > INT_MAX) {
            return false;
        }
        box.x = (int)left;
        box.y = (int)top;
        box.width = (int)width;
        box.height = (int)height;
    }
    leme_render_session_set_box(session, box);
    return true;
}

bool
leme_render_lock_surface_create(struct leme_lock_surface *surface)
{
    surface->scene_tree = wlr_scene_subsurface_tree_create(
        surface->session->server->scene_lock,
        surface->wlr_lock_surface->surface);
    if (surface->scene_tree == NULL) {
        return false;
    }
    leme_render_lock_surface_configure(surface);
    return true;
}

void
leme_render_lock_surface_configure(struct leme_lock_surface *surface)
{
    struct leme_output *output;
    struct leme_box box;

    if (surface->scene_tree == NULL) {
        return;
    }
    output = leme_output_from_wlr_output(surface->session->server,
        surface->wlr_lock_surface->output);
    if (output == NULL) {
        return;
    }
    box = leme_output_full_box(output);
    wlr_scene_node_set_position(&surface->scene_tree->node, box.x, box.y);
}

void
leme_render_lock_surface_destroy(struct leme_lock_surface *surface)
{
    if (surface->scene_tree != NULL) {
        wlr_scene_node_destroy(&surface->scene_tree->node);
        surface->scene_tree = NULL;
    }
}
