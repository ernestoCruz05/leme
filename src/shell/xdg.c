#include "shell/view.h"

#include "core/server.h"
#include "input/input.h"
#include "output/output.h"
#include "protocols/session.h"
#include "render/render.h"
#include "shell/policy.h"
#include "shell/rules.h"
#include "shell/sticky.h"
#include "workspace/tag.h"

#include <stdlib.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>

static struct leme_box
leme_view_full_box(const struct leme_view *view)
{
    return leme_output_full_box(leme_view_output(view));
}

static void
leme_view_popup_finish(struct leme_view_popup *popup)
{
    leme_render_view_popup_destroy(popup);
    wl_list_remove(&popup->commit.link);
    wl_list_remove(&popup->reposition.link);
    wl_list_remove(&popup->new_popup.link);
    wl_list_remove(&popup->destroy.link);
    wl_list_remove(&popup->link);
    free(popup);
}

static void
leme_view_popup_handle_commit(struct wl_listener *listener, void *data)
{
    struct leme_view_popup *popup =
        wl_container_of(listener, popup, commit);

    (void)data;
    leme_session_refresh_idle_inhibitors(popup->view->server);
    if (popup->wlr_popup->base->initial_commit) {
        leme_render_view_popup_unconstrain(
            popup, leme_view_full_box(popup->view));
    }
}

static void
leme_view_popup_handle_reposition(struct wl_listener *listener, void *data)
{
    struct leme_view_popup *popup =
        wl_container_of(listener, popup, reposition);

    (void)data;
    leme_render_view_popup_unconstrain(
        popup, leme_view_full_box(popup->view));
}

static void
leme_view_popup_handle_destroy(struct wl_listener *listener, void *data)
{
    struct leme_view_popup *popup =
        wl_container_of(listener, popup, destroy);

    (void)data;
    leme_view_popup_finish(popup);
}

static bool leme_view_popup_create(struct leme_view *view,
    struct wlr_xdg_popup *wlr_popup);

static void
leme_view_popup_handle_new_popup(struct wl_listener *listener, void *data)
{
    struct leme_view_popup *parent =
        wl_container_of(listener, parent, new_popup);
    struct wlr_xdg_popup *popup = data;

    if (!leme_view_popup_create(parent->view, popup)) {
        wlr_log(WLR_ERROR, "%s", "leme: failed to create nested view popup");
    }
}

static bool
leme_view_popup_create(struct leme_view *view,
    struct wlr_xdg_popup *wlr_popup)
{
    struct leme_view_popup *popup = calloc(1, sizeof(*popup));

    if (popup == NULL) {
        wlr_xdg_popup_destroy(wlr_popup);
        return false;
    }
    popup->view = view;
    popup->wlr_popup = wlr_popup;
    popup->commit.notify = leme_view_popup_handle_commit;
    wl_signal_add(&wlr_popup->base->surface->events.commit, &popup->commit);
    popup->reposition.notify = leme_view_popup_handle_reposition;
    wl_signal_add(&wlr_popup->events.reposition, &popup->reposition);
    popup->new_popup.notify = leme_view_popup_handle_new_popup;
    wl_signal_add(&wlr_popup->base->events.new_popup, &popup->new_popup);
    popup->destroy.notify = leme_view_popup_handle_destroy;
    wl_signal_add(&wlr_popup->events.destroy, &popup->destroy);
    wl_list_insert(&view->popups, &popup->link);
    if (!leme_render_view_popup_create(popup)) {
        wlr_xdg_popup_destroy(wlr_popup);
        return false;
    }
    return true;
}

static void
leme_view_handle_new_popup(struct wl_listener *listener, void *data)
{
    struct leme_view *view =
        wl_container_of(listener, view, new_popup);
    struct wlr_xdg_popup *popup = data;

    if (!leme_view_popup_create(view, popup)) {
        wlr_log(WLR_ERROR, "%s", "leme: failed to create view popup");
    }
}

static void
leme_view_popups_destroy(struct leme_view *view)
{
    while (!wl_list_empty(&view->popups)) {
        struct leme_view_popup *popup =
            wl_container_of(view->popups.next, popup, link);

        wlr_xdg_popup_destroy(popup->wlr_popup);
    }
}

static struct leme_view *
leme_xdg_parent_view(struct leme_server *server,
    const struct wlr_xdg_toplevel *toplevel)
{
    struct leme_view *candidate;

    if (toplevel->parent == NULL) {
        return NULL;
    }
    wl_list_for_each(candidate, &server->views, link) {
        if (candidate->kind == LEME_VIEW_XDG &&
                candidate->xdg_toplevel == toplevel->parent &&
                candidate->mapped && !candidate->unmanaged &&
                (leme_view_is_sticky(candidate) ||
                    (leme_ownership_tag(candidate) != NULL &&
                        leme_ownership_tag(candidate)->owner != NULL))) {
            return candidate;
        }
    }
    return NULL;
}

static void
leme_view_handle_map(struct wl_listener *listener, void *data)
{
    struct leme_view *view = wl_container_of(listener, view, map);
    const enum leme_scratchpad_map_result scratchpad_map =
        leme_scratchpad_try_adopt_map(view);
    struct leme_view *parent =
        leme_xdg_parent_view(view->server, view->xdg_toplevel);
    struct leme_sticky_adoption *adoption = NULL;
    struct leme_view_destination destination =
        leme_view_policy_destination(view->server, parent);
    const struct wlr_xdg_toplevel_state *state =
        &view->xdg_toplevel->current;
    const struct leme_view_rules rules = leme_view_rules_match(
        view->server->config, leme_view_identity(view),
        leme_view_title(view));
    bool floating = view->xdg_toplevel->parent != NULL ||
        leme_view_policy_fixed_size(state->min_width, state->min_height,
            state->max_width, state->max_height);

    leme_view_policy_apply_rules(view->server, &rules, &destination,
        &floating, parent != NULL);

    const bool sticky_parent = parent != NULL &&
        leme_view_is_sticky(parent);
    const struct leme_view_map_options options = {
        .tags = sticky_parent ? NULL : destination.tags,
        .tag_id = sticky_parent ? 0 : destination.tag_id,
        .parent = parent,
        .floating = floating,
        .durable_direct = sticky_parent,
    };

    (void)data;
    if (scratchpad_map == LEME_SCRATCHPAD_MAP_ADOPTED) {
        return;
    }
    if (scratchpad_map == LEME_SCRATCHPAD_MAP_FAILED) {
        wlr_log(WLR_ERROR, "%s", "leme: failed to adopt pending XDG scratchpad");
        return;
    }
    if (sticky_parent &&
            !leme_sticky_prepare_transient(parent, view, &adoption)) {
        wlr_log(WLR_ERROR, "%s",
            "leme: failed to prepare sticky transient adoption");
        return;
    }
    if (!leme_view_map(view, &options)) {
        leme_sticky_discard_transient(&adoption);
        wlr_log(WLR_ERROR, "%s", "leme: failed to map XDG view");
        return;
    }
    if (adoption != NULL) {
        leme_view_apply_layout_box(view,
            leme_view_policy_center_box(view->box, parent->box,
                leme_output_usable_box(leme_view_output(parent))));
        leme_sticky_commit_transient(&adoption);
    }
    if (view->xdg_toplevel->requested.fullscreen ||
            ((rules.fields & LEME_WINDOW_RULE_FULLSCREEN) != 0 &&
                rules.fullscreen)) {
        leme_view_set_fullscreen(view, true);
    }
}

static void
leme_view_handle_unmap(struct wl_listener *listener, void *data)
{
    struct leme_view *view = wl_container_of(listener, view, unmap);

    (void)data;
    leme_view_popups_destroy(view);
    leme_view_unmap(view);
}

static void
leme_view_handle_commit(struct wl_listener *listener, void *data)
{
    struct leme_view *view = wl_container_of(listener, view, commit);

    (void)data;
    leme_session_refresh_idle_inhibitors(view->server);
    if (view->mapped) {
        leme_render_view_clip_to_geometry(view);
    }
    if (view->xdg_toplevel->base->initial_commit) {
        wlr_xdg_toplevel_set_size(view->xdg_toplevel, 0, 0);
    }
}

static void
leme_view_handle_set_title(struct wl_listener *listener, void *data)
{
    struct leme_view *view = wl_container_of(listener, view, set_title);

    (void)data;
    leme_render_view_set_activated(view,
        view == view->server->focused_view);
}

static void
leme_view_handle_set_app_id(struct wl_listener *listener, void *data)
{
    struct leme_view *view = wl_container_of(listener, view, set_app_id);

    (void)data;
    leme_scratchpad_handle_identity_change(view);
    leme_render_view_set_activated(view,
        view == view->server->focused_view);
}

static void
leme_view_handle_request_fullscreen(struct wl_listener *listener, void *data)
{
    struct leme_view *view =
        wl_container_of(listener, view, request_fullscreen);

    (void)data;
    if (!view->mapped) {
        return;
    }
    leme_view_set_fullscreen(
        view, view->xdg_toplevel->requested.fullscreen);
}

static void
leme_view_handle_request_move(struct wl_listener *listener, void *data)
{
    struct leme_view *view =
        wl_container_of(listener, view, request_move);
    struct wlr_xdg_toplevel_move_event *event = data;

    if (event->seat == NULL || event->seat->seat != view->server->seat ||
            !leme_input_pointer_grab_start_xdg(
                view, false, event->serial, WLR_EDGE_NONE)) {
        wlr_log(WLR_DEBUG, "%s", "leme: rejected XDG move request");
    }
}

static void
leme_view_handle_request_resize(struct wl_listener *listener, void *data)
{
    struct leme_view *view =
        wl_container_of(listener, view, request_resize);
    struct wlr_xdg_toplevel_resize_event *event = data;

    if (event->seat == NULL || event->seat->seat != view->server->seat ||
            !leme_input_pointer_grab_start_xdg(
                view, true, event->serial, event->edges)) {
        wlr_log(WLR_DEBUG, "%s", "leme: rejected XDG resize request");
    }
}

static void
leme_view_handle_destroy(struct wl_listener *listener, void *data)
{
    struct leme_view *view = wl_container_of(listener, view, destroy);
    struct leme_server *server = view->server;

    (void)data;
    leme_view_popups_destroy(view);
    leme_view_destroy_core(view);
    wl_list_remove(&view->map.link);
    wl_list_remove(&view->unmap.link);
    wl_list_remove(&view->commit.link);
    wl_list_remove(&view->set_title.link);
    wl_list_remove(&view->set_app_id.link);
    wl_list_remove(&view->request_fullscreen.link);
    wl_list_remove(&view->request_move.link);
    wl_list_remove(&view->request_resize.link);
    wl_list_remove(&view->new_popup.link);
    wl_list_remove(&view->destroy.link);
    free(view);
    if (leme_focused_tags(server) != NULL) {
        leme_view_refresh_tag_focus(server);
    }
}

static void
leme_view_handle_new_toplevel(struct wl_listener *listener, void *data)
{
    struct leme_server *server =
        wl_container_of(listener, server, new_xdg_toplevel);
    struct wlr_xdg_toplevel *xdg_toplevel = data;
    struct leme_view *view = calloc(1, sizeof(*view));

    if (view == NULL) {
        wlr_log(WLR_ERROR, "%s", "leme: failed to allocate view state");
        return;
    }
    view->server = server;
    view->kind = LEME_VIEW_XDG;
    view->xdg_toplevel = xdg_toplevel;
    view->render_opacity = 1.0f;
    leme_ownership_init(view);
    wl_list_init(&view->link);
    wl_list_init(&view->tag_link);
    wl_list_init(&view->focus_link);
    wl_list_init(&view->scratchpad_link);
    wl_list_init(&view->popups);

    view->map.notify = leme_view_handle_map;
    wl_signal_add(&xdg_toplevel->base->surface->events.map, &view->map);
    view->unmap.notify = leme_view_handle_unmap;
    wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &view->unmap);
    view->commit.notify = leme_view_handle_commit;
    wl_signal_add(&xdg_toplevel->base->surface->events.commit, &view->commit);
    view->set_title.notify = leme_view_handle_set_title;
    wl_signal_add(&xdg_toplevel->events.set_title, &view->set_title);
    view->set_app_id.notify = leme_view_handle_set_app_id;
    wl_signal_add(&xdg_toplevel->events.set_app_id, &view->set_app_id);
    view->request_fullscreen.notify = leme_view_handle_request_fullscreen;
    wl_signal_add(&xdg_toplevel->events.request_fullscreen,
        &view->request_fullscreen);
    view->request_move.notify = leme_view_handle_request_move;
    wl_signal_add(&xdg_toplevel->events.request_move, &view->request_move);
    view->request_resize.notify = leme_view_handle_request_resize;
    wl_signal_add(&xdg_toplevel->events.request_resize, &view->request_resize);
    view->new_popup.notify = leme_view_handle_new_popup;
    wl_signal_add(&xdg_toplevel->base->events.new_popup, &view->new_popup);
    view->destroy.notify = leme_view_handle_destroy;
    wl_signal_add(&xdg_toplevel->events.destroy, &view->destroy);
    wl_list_insert(&server->views, &view->link);
}

void
leme_view_init(struct leme_server *server)
{
    wl_list_init(&server->views);
    wl_list_init(&server->focus_history);
    server->xdg_shell = wlr_xdg_shell_create(server->display, 3);
    if (server->xdg_shell == NULL) {
        return;
    }
    server->new_xdg_toplevel.notify = leme_view_handle_new_toplevel;
    wl_signal_add(&server->xdg_shell->events.new_toplevel,
        &server->new_xdg_toplevel);
}

void
leme_view_finish(struct leme_server *server)
{
    if (server->new_xdg_toplevel.link.next != NULL) {
        wl_list_remove(&server->new_xdg_toplevel.link);
        server->new_xdg_toplevel.link.next = NULL;
        server->new_xdg_toplevel.link.prev = NULL;
    }
}
