#include "shell/view.h"

#include "input/input.h"
#include "protocols/input.h"
#include "protocols/publication.h"
#include "protocols/toplevel.h"
#include "shell/layer.h"
#include "shell/policy.h"
#include "output/output.h"
#include "render/render.h"
#include "render/workspace_transition.h"
#include "core/server.h"
#include "protocols/session.h"
#include "workspace/tag.h"

#include <stdint.h>
#include <stdlib.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>
#include <wlr/xwayland/xwayland.h>

struct wlr_surface *
leme_view_surface(const struct leme_view *view)
{
    if (view == NULL) {
        return NULL;
    }
    if (view->kind == LEME_VIEW_XDG && view->xdg_toplevel != NULL) {
        return view->xdg_toplevel->base->surface;
    }
    if (view->kind == LEME_VIEW_XWAYLAND &&
            view->xwayland_surface != NULL) {
        return view->xwayland_surface->surface;
    }
    return NULL;
}

const char *
leme_view_identity(const struct leme_view *view)
{
    if (view == NULL) {
        return NULL;
    }
    if (view->kind == LEME_VIEW_XDG) {
        return view->xdg_toplevel == NULL ? NULL : view->xdg_toplevel->app_id;
    }
    return view->xwayland_surface == NULL ? NULL :
        view->xwayland_surface->class;
}

const char *
leme_view_title(const struct leme_view *view)
{
    if (view == NULL) {
        return NULL;
    }
    if (view->kind == LEME_VIEW_XDG) {
        return view->xdg_toplevel == NULL ? NULL : view->xdg_toplevel->title;
    }
    return view->xwayland_surface == NULL ? NULL :
        view->xwayland_surface->title;
}

struct leme_view *
leme_view_from_surface(struct leme_server *server, struct wlr_surface *surface)
{
    struct leme_view *view;

    if (server == NULL || surface == NULL) {
        return NULL;
    }
    surface = wlr_surface_get_root_surface(surface);
    wl_list_for_each(view, &server->views, link) {
        struct wlr_surface *candidate = leme_view_surface(view);

        if (candidate != NULL &&
                wlr_surface_get_root_surface(candidate) == surface) {
            return view;
        }
    }
    return NULL;
}

void
leme_view_set_activated(struct leme_view *view, bool activated)
{
    if (view != NULL && view->kind == LEME_VIEW_XDG &&
            view->xdg_toplevel != NULL) {
        wlr_xdg_toplevel_set_activated(view->xdg_toplevel, activated);
    } else if (view != NULL && view->kind == LEME_VIEW_XWAYLAND &&
            view->xwayland_surface != NULL) {
        wlr_xwayland_surface_activate(view->xwayland_surface, activated);
    }
    leme_render_view_set_activated(view, activated);
}

static int16_t
leme_view_xwayland_coordinate(int value)
{
    if (value < INT16_MIN) {
        return INT16_MIN;
    }
    if (value > INT16_MAX) {
        return INT16_MAX;
    }
    return (int16_t)value;
}

static uint16_t
leme_view_xwayland_dimension(int value)
{
    if (value < 1) {
        return 1;
    }
    if (value > UINT16_MAX) {
        return UINT16_MAX;
    }
    return (uint16_t)value;
}

void
leme_view_configure(struct leme_view *view, struct leme_box box)
{
    if (view != NULL && view->kind == LEME_VIEW_XWAYLAND &&
            view->server != NULL &&
            view->server->scratchpads.render_configure_count != NULL) {
        (*view->server->scratchpads.render_configure_count)++;
    }
    if (view != NULL && view->kind == LEME_VIEW_XDG &&
            view->xdg_toplevel != NULL) {
        wlr_xdg_toplevel_set_size(view->xdg_toplevel,
            box.width, box.height);
    } else if (view != NULL && view->kind == LEME_VIEW_XWAYLAND &&
            view->xwayland_surface != NULL) {
        wlr_xwayland_surface_configure(view->xwayland_surface,
            leme_view_xwayland_coordinate(box.x),
            leme_view_xwayland_coordinate(box.y),
            leme_view_xwayland_dimension(box.width),
            leme_view_xwayland_dimension(box.height));
    }
}

void
leme_view_flush_deferred_configure(struct leme_view *view)
{
    if (view == NULL || !view->configure_dirty) {
        return;
    }
    view->configure_dirty = false;
    if (!view->mapped) {
        return;
    }
    leme_view_configure(view, view->deferred_configure_box);
}

void
leme_view_set_configure_deferred(struct leme_view *view, bool deferred)
{
    if (view == NULL) {
        return;
    }
    view->configure_deferred = deferred;
    if (!deferred) {
        leme_view_flush_deferred_configure(view);
    }
}

void
leme_view_flush_deferred_configures(struct leme_server *server)
{
    struct leme_view *view;

    if (server == NULL) {
        return;
    }
    wl_list_for_each(view, &server->views, link) {
        leme_view_flush_deferred_configure(view);
    }
}

void
leme_view_ack_fullscreen(struct leme_view *view, bool fullscreen)
{
    if (view != NULL && view->kind == LEME_VIEW_XDG &&
            view->xdg_toplevel != NULL) {
        wlr_xdg_toplevel_set_fullscreen(view->xdg_toplevel, fullscreen);
    } else if (view != NULL && view->kind == LEME_VIEW_XWAYLAND &&
            view->xwayland_surface != NULL) {
        wlr_xwayland_surface_set_fullscreen(
            view->xwayland_surface, fullscreen);
    }
}

void
leme_view_protocol_close(struct leme_view *view)
{
    if (view != NULL && view->kind == LEME_VIEW_XDG &&
            view->xdg_toplevel != NULL) {
        wlr_xdg_toplevel_send_close(view->xdg_toplevel);
    } else if (view != NULL && view->kind == LEME_VIEW_XWAYLAND &&
            view->xwayland_surface != NULL) {
        wlr_xwayland_surface_close(view->xwayland_surface);
    }
}

static bool
leme_view_focus_history_linked(const struct leme_view *view)
{
    return view->focus_link.next != NULL &&
        view->focus_link.next != &view->focus_link;
}

void
leme_view_focus_history_remove(struct leme_view *view)
{
    if (leme_view_focus_history_linked(view)) {
        wl_list_remove(&view->focus_link);
    }
    if (view->focus_link.next != NULL) {
        wl_list_init(&view->focus_link);
    }
}

static void
leme_view_update_tiled(struct leme_view *view)
{
    uint32_t edges = WLR_EDGE_NONE;

    if (view->kind != LEME_VIEW_XDG || view->xdg_toplevel == NULL ||
            wl_resource_get_version(
                view->xdg_toplevel->base->client->resource) <
                XDG_TOPLEVEL_STATE_TILED_LEFT_SINCE_VERSION) {
        return;
    }
    if (!view->floating && !view->fullscreen) {
        edges = WLR_EDGE_TOP | WLR_EDGE_BOTTOM |
            WLR_EDGE_LEFT | WLR_EDGE_RIGHT;
    }
    wlr_xdg_toplevel_set_tiled(view->xdg_toplevel, edges);
}

void
leme_view_clear_focus(struct leme_server *server)
{
    if (server->focused_view != NULL) {
        leme_view_set_activated(server->focused_view, false);
        server->focused_view = NULL;
    }
    if (server->seat != NULL && !leme_session_locked(server)) {
        wlr_seat_keyboard_notify_clear_focus(server->seat);
        leme_input_protocols_update_keyboard_focus(server);
    }
    leme_publication_invalidate(server);
}

void
leme_view_arrange(struct leme_server *server)
{
    struct leme_output *output;
    int gap = server->config == NULL ? 0 : server->config->gap;

    if (server->outputs.next == NULL) {
        return;
    }
    wl_list_for_each(output, &server->outputs, link) {
        if (!output->wlr_output->enabled) {
            continue;
        }
        leme_tags_arrange_current(leme_output_tags(output),
            leme_output_usable_box(output), gap);
    }
}

void
leme_view_refresh_tag_focus(struct leme_server *server)
{
    leme_view_arrange(server);
    leme_tags_refresh_visibility(leme_focused_tags(server));
    /* A reconciliação de saídas não passa por aqui, e não pode passar. */
    if (server->focused_view == NULL) {
        leme_view_focus(leme_sticky_focus_candidate(server,
            leme_output_focused(server)));
    }
}

static bool
leme_view_adopt_to_output(struct leme_view *view,
    struct leme_output *output, bool follow, bool warp)
{
    struct leme_server *server = view->server;
    struct leme_output *source = leme_view_output(view);
    struct leme_tags *destination = leme_output_tags(output);

    if (destination == NULL || source == output) {
        return false;
    }
    if (!leme_tags_adopt_view(destination, view, 0)) {
        return false;
    }
    leme_view_arrange(server);
    if (view->floating && !view->fullscreen) {
        leme_render_view_set_box(view, view->box);
    }
    if (view->fullscreen) {
        leme_view_apply_layout_box(view, leme_output_full_box(output));
    }
    if (follow) {
        leme_output_set_focused(server, output, warp);
        leme_view_focus(view);
    }
    if (source != NULL) {
        leme_tags_refresh_visibility(leme_output_tags(source));
    }
    leme_tags_refresh_visibility(destination);
    return true;
}

bool
leme_view_move_to_output(struct leme_view *view,
    struct leme_output *output, bool follow)
{
    struct leme_output *source;
    bool warp;

    if (view == NULL || output == NULL || view->unmanaged ||
            !view->mapped || leme_view_is_scratchpad(view)) {
        return false;
    }
    source = leme_view_output(view);
    if (leme_output_tags(output) == NULL || source == output) {
        return false;
    }
    if (view->floating && !view->fullscreen && source != NULL) {
        struct leme_box from = leme_output_full_box(source);
        struct leme_box to = leme_output_full_box(output);

        view->box.x += to.x - from.x;
        view->box.y += to.y - from.y;
        view->box = leme_view_policy_clamp_box(view->box,
            leme_output_usable_box(output));
    }
    warp = view->server->config == NULL ||
        view->server->config->output_policy.warp_cursor;
    return leme_view_adopt_to_output(view, output, follow, warp);
}

bool
leme_view_drop_to_output(struct leme_view *view,
    struct leme_layout_detach **detach, struct leme_output *output,
    struct leme_view *target, enum leme_direction direction)
{
    struct leme_tags *destination;

    if (view == NULL || output == NULL || view->unmanaged ||
            !view->mapped || view->fullscreen) {
        return false;
    }
    destination = leme_output_tags(output);
    if (destination == NULL || leme_view_output(view) == output) {
        return false;
    }
    if (view->floating) {
        return leme_view_adopt_to_output(view, output, true, false);
    }
    if (detach == NULL || *detach == NULL || !view->detached) {
        return false;
    }
    if (!leme_view_adopt_to_output(view, output, true, false)) {
        return false;
    }
    if (target != NULL) {
        return leme_view_commit_tiled_drag(view, detach, target, direction);
    }
    leme_layout_discard_detached(detach);
    view->detached = false;
    leme_layout_add_view(&leme_ownership_tag(view)->layout, NULL, view);
    leme_render_view_update_layer(view);
    leme_view_arrange(view->server);
    leme_tags_refresh_visibility(destination);
    leme_view_focus(view);
    return true;
}

static void
leme_view_set_initial_floating_box(struct leme_view *view,
    const struct leme_view *parent)
{
    const struct leme_box area =
        leme_output_usable_box(leme_view_output(view));
    const struct leme_box anchor = parent == NULL ? area : parent->box;
    struct wlr_box geometry = {0};
    struct leme_box frame = {0};

    if (view->kind == LEME_VIEW_XDG) {
        geometry = view->xdg_toplevel->base->geometry;
    } else if (view->xwayland_surface != NULL) {
        geometry = (struct wlr_box){
            .x = view->xwayland_surface->x,
            .y = view->xwayland_surface->y,
            .width = view->xwayland_surface->width,
            .height = view->xwayland_surface->height,
        };
    }
    if (geometry.width > 0 && geometry.height > 0) {
        frame = leme_render_view_frame_box(view, (struct leme_box){
            .x = view->kind == LEME_VIEW_XWAYLAND ? geometry.x : 0,
            .y = view->kind == LEME_VIEW_XWAYLAND ? geometry.y : 0,
            .width = geometry.width,
            .height = geometry.height,
        });
    } else {
        frame.width = area.width / 2;
        frame.height = area.height / 2;
    }
    if (view->kind == LEME_VIEW_XWAYLAND &&
            geometry.width > 0 && geometry.height > 0) {
        view->box = leme_view_policy_requested_box(
            frame, area, view->unmanaged);
    } else {
        view->box = leme_view_policy_center_box(frame, anchor, area);
    }
}

bool
leme_view_map(struct leme_view *view,
    const struct leme_view_map_options *options)
{
    bool visible_destination;

    if (view == NULL || view->mapped || options == NULL ||
            (!options->durable_direct &&
                (options->tags == NULL || options->tag_id == 0))) {
        return false;
    }
    if (!options->durable_direct && !options->floating &&
            !options->unmanaged) {
        leme_input_pointer_grab_cancel_tiled(view->server);
    }
    view->mapped = true;
    view->floating = options->floating || options->unmanaged ||
        options->durable_direct;
    view->unmanaged = options->unmanaged;
    /* Criar a cena pode falhar, ao contrário de dar posse a uma tag.
     * Prepara-se antes de comprometer a vista com a tag, para que um
     * map falhado não deixe efeitos secundários. */
    leme_render_view_create(view);
    if (view->render_tree == NULL) {
        view->mapped = false;
        view->floating = false;
        view->unmanaged = false;
        return false;
    }
    if (options->durable_direct) {
        wlr_scene_node_set_enabled(&view->render_tree->node, false);
        leme_render_view_set_activated(view, false);
        return true;
    }
    if (options->unmanaged) {
        leme_ownership_set_unmanaged_output(view,
            options->tags->output);
        leme_view_set_initial_floating_box(view, options->parent);
        leme_render_view_set_activated(view, false);
        leme_render_view_set_box(view, view->box);
        leme_render_view_animate(view, LEME_ANIMATION_OPEN);
        return true;
    }
    if (!leme_tags_assign_view_to(
            options->tags, view, options->tag_id)) {
        leme_render_view_destroy(view);
        view->mapped = false;
        view->floating = false;
        view->unmanaged = false;
        return false;
    }
    leme_view_update_tiled(view);
    if (view->floating) {
        leme_view_set_initial_floating_box(view, options->parent);
    }
    leme_render_view_set_activated(view, false);
    leme_view_arrange(view->server);
    if (view->floating) {
        leme_render_view_set_box(view, view->box);
    }
    visible_destination = options->tags == leme_focused_tags(view->server) &&
        !options->tags->focused_is_candidate &&
        leme_ownership_tag(view)->id == options->tags->focused_id;
    if (!view->unmanaged && visible_destination) {
        leme_view_focus(view);
    }
    leme_tags_refresh_visibility(options->tags);
    leme_render_view_animate(view, LEME_ANIMATION_OPEN);
    leme_publication_invalidate(view->server);
    return true;
}

void
leme_view_unmap(struct leme_view *view)
{
    struct leme_server *server;
    const bool hidden = view != NULL &&
        leme_view_is_scratchpad(view) && !leme_view_is_shown_scratchpad(view);

    if (view == NULL || !view->mapped) {
        return;
    }
    server = view->server;
    /* Um scratchpad exposto continua dono da saída onde aparece até o
     * instantâneo de fecho estar pronto. Os escondidos nunca criam
     * instantâneos. */
    if (!hidden) {
        leme_render_view_animate(view, LEME_ANIMATION_CLOSE);
    }
    leme_scratchpad_handle_unmap(view);
    leme_sticky_handle_unmap(view);
    leme_input_pointer_grab_cancel_view(view);
    leme_view_focus_history_remove(view);
    view->mapped = false;
    if (server->focused_view == view) {
        leme_view_clear_focus(server);
    }
    leme_tags_remove_view(view);
    leme_render_view_destroy(view);
    leme_view_refresh_tag_focus(server);
    leme_publication_invalidate(server);
}

void
leme_view_destroy_core(struct leme_view *view)
{
    if (view == NULL) {
        return;
    }
    leme_toplevel_untrack(view);
    leme_view_unmap(view);
    if (leme_ownership_tag(view) != NULL) {
        leme_tags_remove_view(view);
    }
    leme_render_view_destroy(view);
    leme_view_focus_history_remove(view);
    if (view->link.next != NULL) {
        wl_list_remove(&view->link);
        wl_list_init(&view->link);
    }
    leme_publication_invalidate(view->server);
}

static void
leme_view_focus_eligible(struct leme_view *view)
{
    struct leme_server *server;
    struct wlr_keyboard *keyboard;

    server = view->server;
    if (leme_session_locked(server)) {
        return;
    }
    if (leme_layer_keyboard_is_exclusive(server)) {
        return;
    }
    leme_layer_release_for_view(server);
    if (server->focused_view != view) {
        if (server->focused_view != NULL) {
            leme_view_set_activated(server->focused_view, false);
        }
        leme_view_set_activated(view, true);
        server->focused_view = view;
    }
    if (leme_ownership_tag(view) != NULL) {
        leme_ownership_tag(view)->focused_view = view;
    }
    if (leme_ownership_tag(view) != NULL || leme_view_is_sticky(view)) {
        leme_view_focus_history_remove(view);
        wl_list_insert(&server->focus_history, &view->focus_link);
    }
    if (leme_view_is_sticky(view)) {
        leme_sticky_raise_group(view);
    } else {
        leme_render_view_focus(view);
    }
    keyboard = wlr_seat_get_keyboard(server->seat);
    if (keyboard != NULL) {
        wlr_seat_keyboard_notify_enter(server->seat,
            leme_view_surface(view), keyboard->keycodes,
            keyboard->num_keycodes, &keyboard->modifiers);
    }
    leme_input_protocols_update_keyboard_focus(server);
    leme_publication_invalidate(server);
}

void
leme_view_focus(struct leme_view *view)
{
    if (view != NULL && leme_ownership_focus_eligible(view)) {
        leme_view_focus_eligible(view);
    }
}

void
leme_view_focus_unmanaged_xwayland(struct leme_view *view)
{
    if (view != NULL && view->mapped && view->unmanaged &&
            view->kind == LEME_VIEW_XWAYLAND &&
            leme_ownership_kind(view) == LEME_VIEW_OWNER_NONE &&
            leme_ownership_effective_output(view) != NULL) {
        leme_view_focus_eligible(view);
    }
}

void
leme_view_focus_next(struct leme_server *server)
{
    struct leme_tags *tags = leme_focused_tags(server);
    struct leme_tag *tag;
    struct wl_list *start;
    struct wl_list *link;
    struct leme_view *view;

    if (tags->focused_is_candidate) {
        return;
    }
    tag = tags->table[tags->focused_id];
    if (tag == NULL || wl_list_empty(&tag->views)) {
        return;
    }
    start = server->focused_view == NULL || leme_ownership_tag(server->focused_view) != tag ?
        tag->views.next : server->focused_view->tag_link.next;
    link = start;
    do {
        if (link == &tag->views) {
            link = link->next;
            continue;
        }
        view = wl_container_of(link, view, tag_link);
        if (view->mapped && (!view->fullscreen || view == server->focused_view)) {
            leme_view_focus(view);
            return;
        }
        link = link->next;
    } while (link != start);
}

static bool
leme_view_focus_across_outputs(
    struct leme_server *server, enum leme_direction direction)
{
    struct leme_output *adjacent;
    struct leme_tags *tags;
    struct leme_view *target;
    bool warp;

    if (server->config != NULL &&
            !server->config->output_policy.cross_output_focus) {
        return false;
    }
    adjacent = leme_output_adjacent(
        server, leme_output_focused(server), direction);
    if (adjacent == NULL) {
        return false;
    }
    warp = server->config == NULL ||
        server->config->output_policy.warp_cursor;
    leme_output_set_focused(server, adjacent, warp);
    tags = leme_output_tags(adjacent);
    target = leme_tags_directional_view(tags, NULL, direction, false);
    if (target != NULL) {
        leme_view_focus(target);
    }
    return true;
}

bool
leme_view_focus_direction(
    struct leme_server *server, enum leme_direction direction)
{
    struct leme_tags *tags = leme_focused_tags(server);
    struct leme_tag *current;
    struct leme_view *origin = NULL;
    struct leme_view *target;

    if (tags == NULL || tags->focused_is_candidate) {
        return false;
    }
    current = tags->table[tags->focused_id];
    if (current == NULL) {
        return false;
    }
    if (server->focused_view != NULL && server->focused_view->mapped &&
            !server->focused_view->unmanaged &&
            leme_ownership_tag(server->focused_view) == current) {
        origin = server->focused_view;
    }
    target = leme_tags_directional_view(tags, origin, direction, false);
    if (target == NULL) {
        return leme_view_focus_across_outputs(server, direction);
    }
    leme_view_focus(target);
    return server->focused_view == target;
}

bool
leme_view_focus_previous(struct leme_server *server)
{
    struct leme_tags *tags = leme_focused_tags(server);
    struct leme_tag *current;
    struct leme_view *view;

    if (tags == NULL || tags->focused_is_candidate) {
        return false;
    }
    current = tags->table[tags->focused_id];
    if (current == NULL) {
        return false;
    }
    wl_list_for_each(view, &server->focus_history, focus_link) {
        const bool current_tag = leme_ownership_tag(view) == current;
        const bool visible_sticky = leme_view_is_sticky(view) &&
            leme_ownership_effective_output(view) ==
                leme_output_focused(server);

        if (view != server->focused_view && view->mapped &&
                !view->unmanaged && (current_tag || visible_sticky)) {
            leme_view_focus(view);
            return server->focused_view == view;
        }
    }
    return false;
}

bool
leme_view_move_direction(
    struct leme_server *server, enum leme_direction direction, int amount)
{
    struct leme_tags *tags;
    struct leme_view *view;
    struct leme_tag *current;

    if (server == NULL || amount <= 0 || server->focused_view == NULL) {
        return false;
    }
    view = server->focused_view;
    if (!view->mapped || view->unmanaged || view->fullscreen) {
        return false;
    }
    if (leme_view_is_shown_scratchpad(view)) {
        return leme_scratchpad_apply_shown_box(view,
            leme_layout_move_box(view->box, direction, amount), false);
    }
    if (leme_view_is_scratchpad(view)) {
        return false;
    }
    if (leme_view_is_sticky(view)) {
        return leme_sticky_apply_box(view,
            leme_layout_move_box(view->box, direction, amount), false);
    }
    tags = leme_focused_tags(server);
    if (tags == NULL || tags->focused_is_candidate) {
        return false;
    }
    current = tags->table[tags->focused_id];
    if (leme_ownership_tag(view) == NULL || leme_ownership_tag(view) != current) {
        return false;
    }
    if (view->floating) {
        view->box = leme_layout_move_box(view->box, direction, amount);
        leme_render_view_set_box(view, view->box);
        return true;
    }
    leme_input_pointer_grab_cancel_tiled(server);
    if (leme_tags_swap_directional(tags, view, direction)) {
        leme_view_arrange(server);
        return true;
    }
    if (server->config != NULL &&
            server->config->output_policy.cross_output_move) {
        struct leme_output *adjacent = leme_output_adjacent(
            server, leme_output_focused(server), direction);

        if (adjacent != NULL) {
            return leme_view_move_to_output(view, adjacent, true);
        }
    }
    return true;
}

bool
leme_view_begin_tiled_drag(struct leme_view *view,
    struct leme_layout_detach **detach)
{
    struct leme_tags *tags;
    struct leme_box box;

    if (view == NULL || detach == NULL || *detach != NULL ||
            !view->mapped || view->unmanaged || view->floating ||
            view->detached || view->fullscreen || leme_ownership_tag(view) == NULL ||
            !leme_layout_drag_supported(&leme_ownership_tag(view)->layout)) {
        return false;
    }
    tags = leme_focused_tags(view->server);
    if (tags == NULL || tags->focused_is_candidate ||
            tags->focused_id == 0 || tags->focused_id > tags->max_tags ||
            tags->table[tags->focused_id] != leme_ownership_tag(view)) {
        return false;
    }
    box = view->box;
    *detach = leme_layout_detach_view(&leme_ownership_tag(view)->layout.root, view);
    if (*detach == NULL) {
        return false;
    }
    view->detached = true;
    leme_render_view_update_layer(view);
    leme_view_arrange(view->server);
    view->box = box;
    leme_render_view_set_box(view, view->box);
    return true;
}

bool
leme_view_commit_tiled_drag(struct leme_view *view,
    struct leme_layout_detach **detach, struct leme_view *target,
    enum leme_direction direction)
{
    if (view == NULL || detach == NULL || *detach == NULL ||
            !view->detached || leme_ownership_tag(view) == NULL || target == NULL ||
            leme_ownership_tag(target) != leme_ownership_tag(view) || !target->mapped ||
            target->floating || target->detached || target->fullscreen ||
            !leme_layout_insert_detached(
                &leme_ownership_tag(view)->layout.root, detach, target, direction)) {
        return false;
    }
    view->detached = false;
    leme_render_view_update_layer(view);
    leme_view_arrange(view->server);
    leme_tags_refresh_visibility(leme_focused_tags(view->server));
    leme_view_focus(view);
    return true;
}

bool
leme_view_restore_tiled_drag(struct leme_view *view,
    struct leme_layout_detach **detach)
{
    bool restored;

    if (view == NULL || detach == NULL) {
        return false;
    }
    restored = *detach == NULL || (leme_ownership_tag(view) != NULL &&
        leme_layout_restore_view(&leme_ownership_tag(view)->layout.root, detach));
    if (!restored) {
        wlr_log(WLR_ERROR, "%s",
            "leme: tiled drag transaction could not be restored");
        leme_layout_discard_detached(detach);
        view->detached = false;
        view->floating = true;
        leme_view_update_tiled(view);
        leme_render_view_update_layer(view);
        leme_render_view_set_box(view, view->box);
    } else {
        view->detached = false;
        leme_render_view_update_layer(view);
    }
    if (leme_focused_tags(view->server) != NULL) {
        leme_view_arrange(view->server);
        leme_tags_refresh_visibility(leme_focused_tags(view->server));
    }
    if (view->mapped) {
        leme_view_focus(view);
    }
    return restored;
}

void
leme_view_discard_tiled_drag(struct leme_view *view,
    struct leme_layout_detach **detach)
{
    if (view == NULL || detach == NULL) {
        return;
    }
    leme_layout_discard_detached(detach);
    view->detached = false;
}

void
leme_view_close(struct leme_view *view)
{
    leme_view_protocol_close(view);
}

bool
leme_view_set_floating(struct leme_view *view, bool floating)
{
    if (view == NULL || leme_view_is_scratchpad(view) ||
            leme_view_is_sticky(view)) {
        return false;
    }
    if (leme_ownership_tag(view) == NULL || view->floating == floating || view->fullscreen) {
        return view->floating == floating;
    }
    leme_input_pointer_grab_cancel_tiled(view->server);
    if (!floating) {
        leme_input_pointer_grab_cancel_view(view);
    }
    if (floating) {
        leme_layout_remove_view(&leme_ownership_tag(view)->layout, view);
        view->floating = true;
        if (view->box.width <= 0 || view->box.height <= 0) {
            leme_view_set_initial_floating_box(view, NULL);
        }
    } else {
        view->floating = false;
        leme_layout_add_view(&leme_ownership_tag(view)->layout,
            leme_ownership_tag(view)->focused_view, view);
    }
    leme_view_update_tiled(view);
    leme_render_view_update_layer(view);
    if (floating) {
        leme_render_view_set_box(view, view->box);
    }
    leme_view_arrange(view->server);
    leme_tags_refresh_visibility(leme_focused_tags(view->server));
    return true;
}

bool
leme_view_set_fullscreen(struct leme_view *view, bool fullscreen)
{
    struct leme_box area;

    if (view == NULL) {
        return false;
    }
    if (leme_view_is_scratchpad(view)) {
        view->fullscreen = false;
        leme_view_ack_fullscreen(view, false);
        return false;
    }
    if (fullscreen && leme_view_is_sticky(view)) {
        return leme_sticky_enter_fullscreen(view);
    }
    if (leme_ownership_tag(view) == NULL) {
        return false;
    }
    if (view->fullscreen == fullscreen) {
        return true;
    }
    leme_render_output_animations_finish(leme_view_output(view));
    leme_input_pointer_grab_cancel_tiled(view->server);
    if (fullscreen) {
        leme_input_pointer_grab_cancel_view(view);
        view->saved_box = view->box;
    }
    view->fullscreen = fullscreen;
    leme_view_update_tiled(view);
    leme_render_view_update_layer(view);
    leme_view_ack_fullscreen(view, fullscreen);
    if (fullscreen) {
        area = leme_output_usable_box(leme_view_output(view));
        view->box = area;
        leme_render_view_set_box(view, area);
    } else {
        if (view->floating) {
            view->box = view->saved_box;
            leme_render_view_set_box(view, view->box);
        } else {
            leme_view_arrange(view->server);
        }
    }
    leme_render_view_set_activated(view, view == view->server->focused_view);
    leme_tags_refresh_visibility(leme_focused_tags(view->server));
    return true;
}

bool
leme_view_resize(struct leme_view *view,
    enum leme_resize_edge edge, int amount)
{
    struct leme_box area;
    int limit;

    if (view == NULL || view->fullscreen || amount <= 0 ||
            (!leme_view_is_shown_scratchpad(view) &&
                !leme_view_is_sticky(view) &&
                leme_ownership_tag(view) == NULL)) {
        return false;
    }
    if (!view->floating) {
        leme_input_pointer_grab_cancel_tiled(view->server);
        if (!leme_layout_resize(&leme_ownership_tag(view)->layout, view, edge, amount,
                leme_output_usable_box(leme_view_output(view)))) {
            return false;
        }
        leme_view_arrange(view->server);
        return true;
    }

    area = leme_output_usable_box(leme_view_output(view));
    view->box = leme_view_policy_clamp_box(view->box, area);
    if (edge == LEME_RESIZE_LEFT) {
        limit = view->box.x - area.x;
        if (amount > limit) {
            amount = limit;
        }
        view->box.x -= amount;
        view->box.width += amount;
    } else if (edge == LEME_RESIZE_RIGHT) {
        limit = area.x + area.width - view->box.x - view->box.width;
        if (amount > limit) {
            amount = limit;
        }
        view->box.width += amount;
    } else if (edge == LEME_RESIZE_UP) {
        limit = view->box.y - area.y;
        if (amount > limit) {
            amount = limit;
        }
        view->box.y -= amount;
        view->box.height += amount;
    } else if (edge == LEME_RESIZE_DOWN) {
        limit = area.y + area.height - view->box.y - view->box.height;
        if (amount > limit) {
            amount = limit;
        }
        view->box.height += amount;
    } else {
        return false;
    }
    if (leme_view_is_shown_scratchpad(view)) {
        return leme_scratchpad_apply_shown_box(view, view->box, true);
    }
    if (leme_view_is_sticky(view)) {
        return leme_sticky_apply_box(view, view->box, true);
    }
    leme_render_view_set_box(view, view->box);
    return true;
}

bool
leme_view_apply_interactive_box(
    struct leme_view *view, struct leme_box box, bool resizing)
{
    if (view == NULL || !view->mapped ||
            (!view->floating && !view->detached) || view->fullscreen ||
            (leme_view_is_scratchpad(view) &&
                !leme_view_is_shown_scratchpad(view))) {
        return false;
    }
    if (leme_view_is_sticky(view)) {
        return leme_sticky_apply_box(view, box, resizing);
    }
    view->box = box;
    if (resizing && view->kind == LEME_VIEW_XDG) {
        leme_view_configure(view,
            leme_render_view_content_box(view, view->box));
    }
    leme_render_view_set_box(view, view->box);
    return true;
}

void
leme_view_apply_layout_box(struct leme_view *view, struct leme_box box)
{
    if (view->fullscreen) {
        return;
    }
    view->box = box;
    leme_render_view_set_box(view, box);
}
