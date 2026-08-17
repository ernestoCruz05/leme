#include "protocols/toplevel.h"

#include "config/config.h"
#include "core/server.h"
#include "output/output.h"
#include "protocols/publication.h"
#include "protocols/session.h"
#include "protocols/workspace.h"
#include "shell/sticky.h"
#include "shell/view.h"
#include "workspace/tag.h"

#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/xwayland/xwayland.h>

struct leme_publication_toplevel {
    struct leme_view *view;
    struct wlr_ext_foreign_toplevel_handle_v1 *ext_handle;
    struct wlr_foreign_toplevel_handle_v1 *wlr_handle;
    char *title;
    char *app_id;
    struct wlr_output *output;
    struct wl_listener set_title;
    struct wl_listener set_app_id;
    struct wl_listener request_close;
    struct wl_listener request_fullscreen;
    struct wl_listener request_maximize;
    struct wl_listener request_minimize;
    struct wl_listener request_activate;
};

static const char *
leme_toplevel_title(const struct leme_view *view)
{
    if (view->kind == LEME_VIEW_XDG) {
        return view->xdg_toplevel == NULL ? NULL : view->xdg_toplevel->title;
    }
    return view->xwayland_surface == NULL ? NULL :
        view->xwayland_surface->title;
}

static const char *
leme_toplevel_app_id(const struct leme_view *view)
{
    if (view->kind == LEME_VIEW_XDG) {
        return view->xdg_toplevel == NULL ? NULL : view->xdg_toplevel->app_id;
    }
    return view->xwayland_surface == NULL ? NULL :
        view->xwayland_surface->class;
}

static bool
leme_toplevel_eligible(const struct leme_view *view)
{
    return leme_ownership_publication_eligible(view);
}

static bool
leme_toplevel_replace(char **stored, const char *value)
{
    char *copy;

    if (value == NULL) {
        if (*stored == NULL) {
            return false;
        }
        free(*stored);
        *stored = NULL;
        return true;
    }
    if (*stored != NULL && strcmp(*stored, value) == 0) {
        return false;
    }
    copy = strdup(value);
    if (copy == NULL) {
        return false;
    }
    free(*stored);
    *stored = copy;
    return true;
}

static void
leme_toplevel_handle_set_title(struct wl_listener *listener, void *data)
{
    struct leme_publication_toplevel *toplevel =
        wl_container_of(listener, toplevel, set_title);

    (void)data;
    leme_publication_invalidate(toplevel->view->server);
}

static void
leme_toplevel_handle_set_app_id(struct wl_listener *listener, void *data)
{
    struct leme_publication_toplevel *toplevel =
        wl_container_of(listener, toplevel, set_app_id);

    (void)data;
    leme_publication_invalidate(toplevel->view->server);
}

static void
leme_toplevel_handle_request_close(struct wl_listener *listener, void *data)
{
    struct leme_publication_toplevel *toplevel =
        wl_container_of(listener, toplevel, request_close);

    (void)data;
    if (leme_session_locked(toplevel->view->server) ||
            !leme_toplevel_eligible(toplevel->view)) {
        return;
    }
    leme_view_protocol_close(toplevel->view);
}

static void
leme_toplevel_handle_request_fullscreen(
    struct wl_listener *listener, void *data)
{
    struct leme_publication_toplevel *toplevel =
        wl_container_of(listener, toplevel, request_fullscreen);
    struct wlr_foreign_toplevel_handle_v1_fullscreen_event *event = data;
    struct leme_view *view = toplevel->view;

    if (event == NULL || !leme_toplevel_eligible(view) ||
            leme_session_locked(view->server)) {
        return;
    }
    if (leme_view_set_fullscreen(view, event->fullscreen)) {
        leme_publication_invalidate(view->server);
    }
}

static void
leme_toplevel_handle_request_ignored(struct wl_listener *listener, void *data)
{
    (void)listener;
    (void)data;
}

void
leme_toplevel_activate_view(struct leme_view *view)
{
    struct leme_server *server;
    struct leme_output *output;
    enum leme_activation_policy policy = LEME_ACTIVATION_FOLLOW;
    struct leme_tags *tags;
    bool same_tag;

    if (view == NULL || view->server == NULL) {
        return;
    }
    server = view->server;
    output = leme_view_output(view);
    if (leme_session_locked(server) || !leme_toplevel_eligible(view) ||
            output == NULL) {
        return;
    }
    if (leme_view_is_sticky(view)) {
        leme_output_set_focused(server, output, false);
        leme_view_focus(view);
        return;
    }
    if (leme_view_is_shown_scratchpad(view)) {
        leme_view_focus(view);
        return;
    }
    if (server->config != NULL) {
        policy = server->config->publication.activation;
    }
    tags = leme_output_tags(output);
    if (tags == NULL) {
        return;
    }
    same_tag = output == leme_output_focused(server) &&
        tags->focused_id == leme_ownership_tag(view)->id;
    if (same_tag) {
        leme_view_focus(view);
        return;
    }
    if (policy == LEME_ACTIVATION_IGNORE) {
        return;
    }
    if (policy == LEME_ACTIVATION_URGENT) {
        leme_workspace_mark_urgent(server, output, leme_ownership_tag(view)->id);
        leme_publication_invalidate(server);
        return;
    }
    if (output != leme_output_focused(server)) {
        leme_output_set_focused(server, output, true);
    }
    leme_tags_focus_id(tags, leme_ownership_tag(view)->id);
    leme_view_refresh_tag_focus(server);
    leme_view_focus(view);
    leme_publication_invalidate(server);
}

static void
leme_toplevel_handle_request_activate(
    struct wl_listener *listener, void *data)
{
    struct leme_publication_toplevel *toplevel =
        wl_container_of(listener, toplevel, request_activate);

    (void)data;
    leme_toplevel_activate_view(toplevel->view);
}

static void
leme_toplevel_destroy(struct leme_publication_toplevel *toplevel)
{
    wl_list_remove(&toplevel->request_activate.link);
    wl_list_remove(&toplevel->request_close.link);
    wl_list_remove(&toplevel->request_fullscreen.link);
    wl_list_remove(&toplevel->request_maximize.link);
    wl_list_remove(&toplevel->request_minimize.link);
    wl_list_remove(&toplevel->set_title.link);
    wl_list_remove(&toplevel->set_app_id.link);
    if (toplevel->ext_handle != NULL) {
        wlr_ext_foreign_toplevel_handle_v1_destroy(toplevel->ext_handle);
    }
    if (toplevel->wlr_handle != NULL) {
        wlr_foreign_toplevel_handle_v1_destroy(toplevel->wlr_handle);
    }
    free(toplevel->title);
    free(toplevel->app_id);
    toplevel->view->publication = NULL;
    free(toplevel);
}

static struct leme_publication_toplevel *
leme_toplevel_create(struct leme_server *server, struct leme_view *view)
{
    struct leme_publication_toplevel *toplevel = calloc(1, sizeof(*toplevel));
    struct wlr_ext_foreign_toplevel_handle_v1_state state = {0};

    if (toplevel == NULL) {
        return NULL;
    }
    toplevel->view = view;
    state.title = leme_toplevel_title(view);
    state.app_id = leme_toplevel_app_id(view);
    toplevel->ext_handle = wlr_ext_foreign_toplevel_handle_v1_create(
        server->foreign_toplevel_list, &state);
    if (toplevel->ext_handle == NULL) {
        free(toplevel);
        return NULL;
    }
    toplevel->wlr_handle = wlr_foreign_toplevel_handle_v1_create(
        server->foreign_toplevel_manager);
    if (toplevel->wlr_handle == NULL) {
        wlr_ext_foreign_toplevel_handle_v1_destroy(toplevel->ext_handle);
        free(toplevel);
        return NULL;
    }
    toplevel->ext_handle->data = toplevel;
    toplevel->wlr_handle->data = toplevel;

    toplevel->set_title.notify = leme_toplevel_handle_set_title;
    toplevel->set_app_id.notify = leme_toplevel_handle_set_app_id;
    if (view->kind == LEME_VIEW_XDG) {
        wl_signal_add(&view->xdg_toplevel->events.set_title,
            &toplevel->set_title);
        wl_signal_add(&view->xdg_toplevel->events.set_app_id,
            &toplevel->set_app_id);
    } else {
        wl_signal_add(&view->xwayland_surface->events.set_title,
            &toplevel->set_title);
        wl_signal_add(&view->xwayland_surface->events.set_class,
            &toplevel->set_app_id);
    }

    toplevel->request_close.notify = leme_toplevel_handle_request_close;
    wl_signal_add(&toplevel->wlr_handle->events.request_close,
        &toplevel->request_close);
    toplevel->request_fullscreen.notify =
        leme_toplevel_handle_request_fullscreen;
    wl_signal_add(&toplevel->wlr_handle->events.request_fullscreen,
        &toplevel->request_fullscreen);
    toplevel->request_maximize.notify = leme_toplevel_handle_request_ignored;
    wl_signal_add(&toplevel->wlr_handle->events.request_maximize,
        &toplevel->request_maximize);
    toplevel->request_minimize.notify = leme_toplevel_handle_request_ignored;
    wl_signal_add(&toplevel->wlr_handle->events.request_minimize,
        &toplevel->request_minimize);
    toplevel->request_activate.notify = leme_toplevel_handle_request_activate;
    wl_signal_add(&toplevel->wlr_handle->events.request_activate,
        &toplevel->request_activate);

    view->publication = toplevel;
    return toplevel;
}

static void
leme_toplevel_sync(struct leme_server *server, struct leme_view *view)
{
    struct leme_publication_toplevel *toplevel = view->publication;
    struct wlr_ext_foreign_toplevel_handle_v1_state state = {0};
    struct leme_output *output = leme_view_output(view);
    struct wlr_output *wlr_output =
        output == NULL ? NULL : output->wlr_output;
    const char *title = leme_toplevel_title(view);
    const char *app_id = leme_toplevel_app_id(view);
    bool changed;

    if (toplevel == NULL) {
        toplevel = leme_toplevel_create(server, view);
        if (toplevel == NULL) {
            return;
        }
        changed = leme_toplevel_replace(&toplevel->title, title);
        changed = leme_toplevel_replace(&toplevel->app_id, app_id) || changed;
    } else {
        changed = leme_toplevel_replace(&toplevel->title, title);
        changed = leme_toplevel_replace(&toplevel->app_id, app_id) || changed;
        if (changed) {
            state.title = toplevel->title;
            state.app_id = toplevel->app_id;
            wlr_ext_foreign_toplevel_handle_v1_update_state(
                toplevel->ext_handle, &state);
        }
    }
    if (changed) {
        wlr_foreign_toplevel_handle_v1_set_title(
            toplevel->wlr_handle,
            toplevel->title == NULL ? "" : toplevel->title);
        wlr_foreign_toplevel_handle_v1_set_app_id(
            toplevel->wlr_handle,
            toplevel->app_id == NULL ? "" : toplevel->app_id);
    }
    if (toplevel->output != wlr_output) {
        if (toplevel->output != NULL) {
            wlr_foreign_toplevel_handle_v1_output_leave(
                toplevel->wlr_handle, toplevel->output);
        }
        if (wlr_output != NULL) {
            wlr_foreign_toplevel_handle_v1_output_enter(
                toplevel->wlr_handle, wlr_output);
        }
        toplevel->output = wlr_output;
    }
    wlr_foreign_toplevel_handle_v1_set_activated(
        toplevel->wlr_handle, server->focused_view == view);
    wlr_foreign_toplevel_handle_v1_set_fullscreen(
        toplevel->wlr_handle, view->fullscreen);
}

void
leme_toplevel_reconcile(struct leme_server *server)
{
    struct leme_view *view;

    if (server->foreign_toplevel_list == NULL) {
        return;
    }
    wl_list_for_each(view, &server->views, link) {
        if (leme_toplevel_eligible(view)) {
            leme_toplevel_sync(server, view);
        } else if (view->publication != NULL) {
            leme_toplevel_destroy(view->publication);
        }
    }
}

struct leme_view *
leme_toplevel_view_from_handle(
    struct wlr_ext_foreign_toplevel_handle_v1 *handle)
{
    const struct leme_publication_toplevel *toplevel;

    if (handle == NULL || handle->data == NULL) {
        return NULL;
    }
    toplevel = handle->data;
    return toplevel->view;
}

void
leme_toplevel_untrack(struct leme_view *view)
{
    if (view != NULL && view->publication != NULL) {
        leme_toplevel_destroy(view->publication);
    }
}


struct wlr_foreign_toplevel_handle_v1 *
leme_toplevel_handle(const struct leme_view *view)
{
    return view == NULL || view->publication == NULL ?
        NULL : view->publication->wlr_handle;
}

bool
leme_toplevel_init(struct leme_server *server)
{
    server->foreign_toplevel_list =
        wlr_ext_foreign_toplevel_list_v1_create(server->display, 1);
    server->foreign_toplevel_manager =
        wlr_foreign_toplevel_manager_v1_create(server->display);
    return server->foreign_toplevel_list != NULL &&
        server->foreign_toplevel_manager != NULL;
}

void
leme_toplevel_finish(struct leme_server *server)
{
    struct leme_view *view;

    if (server->foreign_toplevel_list == NULL) {
        return;
    }
    wl_list_for_each(view, &server->views, link) {
        leme_toplevel_untrack(view);
    }
    server->foreign_toplevel_list = NULL;
    server->foreign_toplevel_manager = NULL;
}
