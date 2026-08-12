#include "protocols/session.h"

#include "protocols/capture.h"

#include "core/command.h"
#include "protocols/data.h"
#include "input/input.h"
#include "protocols/input.h"
#include "protocols/publication.h"
#include "shell/layer.h"
#include "output/output.h"
#include "render/render.h"
#include "core/server.h"
#include "shell/view.h"

#include <stdlib.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/util/log.h>

struct leme_idle_inhibitor {
    struct leme_session *session;
    struct wlr_idle_inhibitor_v1 *inhibitor;
    struct wl_list link;
    struct wl_listener destroy;
};

bool
leme_session_surface_allowed(const struct leme_server *server,
    struct wlr_surface *surface)
{
    struct leme_session *session = server->session_protocols;
    struct leme_lock_surface *wrapper;
    struct wlr_surface *root;

    if (session == NULL || !session->locked) {
        return true;
    }
    if (surface == NULL) {
        return false;
    }
    root = wlr_surface_get_root_surface(surface);
    wl_list_for_each(wrapper, &session->lock_surfaces, link) {
        if (wrapper->configured && wrapper->scene_tree != NULL &&
                wrapper->wlr_lock_surface->surface == root) {
            return true;
        }
    }
    return false;
}

bool
leme_session_command_allowed(const struct leme_server *server,
    const struct leme_command *command)
{
    return !leme_session_locked(server) ||
        command->type == LEME_COMMAND_SWITCH_VT ||
        command->type == LEME_COMMAND_QUIT;
}

void
leme_session_restore_focus(struct leme_server *server)
{
    if (leme_session_locked(server)) {
        return;
    }
    leme_layer_restore_keyboard_focus(server);
    if (server->focused_layer == NULL && server->focused_view != NULL &&
            server->focused_view->mapped) {
        leme_view_set_activated(server->focused_view, true);
        leme_view_focus(server->focused_view);
    }
    leme_input_refresh_pointer_focus(server, 0);
}

static void
leme_session_focus_surface(struct leme_lock_surface *wrapper)
{
    struct leme_server *server = wrapper->session->server;
    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);

    if (keyboard != NULL) {
        wlr_seat_keyboard_notify_enter(server->seat,
            wrapper->wlr_lock_surface->surface,
            keyboard->keycodes, keyboard->num_keycodes,
            &keyboard->modifiers);
    }
    leme_input_protocols_update_keyboard_focus(server);
    leme_input_refresh_pointer_focus(server, 0);
}

static void
leme_session_handle_surface_commit(
    struct wl_listener *listener, void *data)
{
    struct leme_lock_surface *wrapper =
        wl_container_of(listener, wrapper, commit);
    struct leme_session *session = wrapper->session;

    (void)data;
    if (!session->locked || session->lock == NULL ||
            !wrapper->wlr_lock_surface->configured ||
            !wlr_surface_has_buffer(wrapper->wlr_lock_surface->surface) ||
            wrapper->scene_tree == NULL) {
        return;
    }
    wrapper->configured = true;
    leme_render_lock_surface_configure(wrapper);
    leme_session_focus_surface(wrapper);
}

static void
leme_session_handle_surface_destroy(
    struct wl_listener *listener, void *data)
{
    struct leme_lock_surface *wrapper =
        wl_container_of(listener, wrapper, destroy);
    struct leme_server *server = wrapper->session->server;
    struct wlr_surface *surface = wrapper->wlr_lock_surface->surface;

    (void)data;
    if (server->seat->keyboard_state.focused_surface == surface) {
        wlr_seat_keyboard_notify_clear_focus(server->seat);
        leme_input_protocols_update_keyboard_focus(server);
    }
    if (server->seat->pointer_state.focused_surface == surface) {
        wlr_seat_pointer_notify_clear_focus(server->seat);
        leme_input_protocols_update_pointer_focus(server);
    }
    wl_list_remove(&wrapper->commit.link);
    wl_list_remove(&wrapper->destroy.link);
    wl_list_remove(&wrapper->link);
    leme_render_lock_surface_destroy(wrapper);
    free(wrapper);
}

static void
leme_session_handle_new_surface(
    struct wl_listener *listener, void *data)
{
    struct leme_session *session =
        wl_container_of(listener, session, lock_new_surface);
    struct wlr_session_lock_surface_v1 *lock_surface = data;
    struct leme_output *output = leme_output_from_wlr_output(
        session->server, lock_surface->output);
    struct leme_lock_surface *wrapper = calloc(1, sizeof(*wrapper));
    struct leme_box box;

    if (wrapper == NULL) {
        wlr_log(WLR_ERROR, "%s", "leme: failed to track lock surface");
        return;
    }
    wrapper->session = session;
    wrapper->wlr_lock_surface = lock_surface;
    wrapper->commit.notify = leme_session_handle_surface_commit;
    wl_signal_add(&lock_surface->surface->events.commit, &wrapper->commit);
    wrapper->destroy.notify = leme_session_handle_surface_destroy;
    wl_signal_add(&lock_surface->events.destroy, &wrapper->destroy);
    wl_list_insert(&session->lock_surfaces, &wrapper->link);
    if (output == NULL || !leme_render_lock_surface_create(wrapper)) {
        wlr_log(WLR_ERROR, "%s",
            "leme: lock surface does not target a tracked output");
        return;
    }
    box = leme_output_full_box(output);
    wlr_session_lock_surface_v1_configure(
        lock_surface, (uint32_t)box.width, (uint32_t)box.height);
}

void
leme_session_output_changed(struct leme_server *server)
{
    struct leme_session *session = server->session_protocols;
    struct leme_lock_surface *wrapper;

    if (session == NULL) {
        return;
    }
    leme_render_session_set_locked(session, session->locked);
    if (!session->locked) {
        return;
    }
    wl_list_for_each(wrapper, &session->lock_surfaces, link) {
        struct leme_output *output = leme_output_from_wlr_output(
            server, wrapper->wlr_lock_surface->output);
        struct leme_box box;

        if (output == NULL) {
            continue;
        }
        box = leme_output_full_box(output);
        leme_render_lock_surface_configure(wrapper);
        wlr_session_lock_surface_v1_configure(
            wrapper->wlr_lock_surface,
            (uint32_t)box.width, (uint32_t)box.height);
    }
}

bool
leme_session_prepare_output_wake(
    struct leme_server *server, const struct leme_box *box)
{
    struct leme_session *session = server->session_protocols;

    if (session == NULL || !session->locked) {
        return true;
    }
    return box != NULL &&
        leme_render_session_prepare_output_wake(session, *box);
}

void
leme_session_restore_output_wake(struct leme_server *server)
{
    struct leme_session *session = server->session_protocols;

    if (session != NULL && session->locked) {
        leme_render_session_set_locked(session, true);
    }
}

static void
leme_session_handle_unlock(struct wl_listener *listener, void *data)
{
    struct leme_session *session =
        wl_container_of(listener, session, lock_unlock);
    struct leme_server *server = session->server;

    (void)data;
    session->unlocking = true;
    session->locked = false;
    session->abandoned = false;
    wlr_seat_pointer_notify_clear_focus(server->seat);
    wlr_seat_keyboard_notify_clear_focus(server->seat);
    leme_render_session_set_locked(session, false);
    leme_session_restore_focus(server);
    leme_publication_invalidate(server);
}

static void
leme_session_handle_lock_destroy(
    struct wl_listener *listener, void *data)
{
    struct leme_session *session =
        wl_container_of(listener, session, lock_destroy);
    bool unlocked = session->unlocking;

    (void)data;
    session->lock = NULL;
    wl_list_remove(&session->lock_new_surface.link);
    wl_list_remove(&session->lock_unlock.link);
    wl_list_remove(&session->lock_destroy.link);
    session->unlocking = false;
    if (unlocked) {
        return;
    }
    session->locked = true;
    leme_capture_invalidate_all(session->server);
    leme_input_pointer_grab_finish(session->server);
    session->abandoned = true;
    leme_render_session_set_locked(session, true);
    wlr_seat_pointer_notify_clear_focus(session->server->seat);
    wlr_seat_keyboard_notify_clear_focus(session->server->seat);
    leme_input_protocols_update_pointer_focus(session->server);
    leme_input_protocols_update_keyboard_focus(session->server);
}

static void
leme_session_handle_new_lock(struct wl_listener *listener, void *data)
{
    struct leme_session *session =
        wl_container_of(listener, session, new_lock);
    struct leme_server *server = session->server;
    struct wlr_session_lock_v1 *lock = data;

    if (session->lock != NULL && !session->abandoned) {
        wlr_session_lock_v1_destroy(lock);
        return;
    }
    session->lock = lock;
    session->locked = true;
    leme_capture_invalidate_all(server);
    leme_input_pointer_grab_finish(server);
    session->abandoned = false;
    session->unlocking = false;
    leme_render_session_set_locked(session, true);
    leme_data_cancel_drag(server);
    leme_input_protocols_cancel_constraint(server);
    if (server->focused_view != NULL) {
        leme_view_set_activated(server->focused_view, false);
    }
    wlr_seat_pointer_notify_clear_focus(server->seat);
    wlr_seat_keyboard_notify_clear_focus(server->seat);
    leme_input_set_mode(server, "common");
    leme_input_protocols_update_pointer_focus(server);
    leme_input_protocols_update_keyboard_focus(server);
    session->lock_new_surface.notify = leme_session_handle_new_surface;
    wl_signal_add(&lock->events.new_surface, &session->lock_new_surface);
    session->lock_unlock.notify = leme_session_handle_unlock;
    wl_signal_add(&lock->events.unlock, &session->lock_unlock);
    session->lock_destroy.notify = leme_session_handle_lock_destroy;
    wl_signal_add(&lock->events.destroy, &session->lock_destroy);
    wlr_session_lock_v1_send_locked(lock);
}

void
leme_session_refresh_idle_inhibitors(struct leme_server *server)
{
    struct leme_session *session = server->session_protocols;
    struct leme_idle_inhibitor *wrapper;
    bool inhibited = false;

    if (session == NULL) {
        return;
    }
    wl_list_for_each(wrapper, &session->inhibitors, link) {
        if (leme_render_surface_visible(server, wrapper->inhibitor->surface)) {
            inhibited = true;
            break;
        }
    }
    wlr_idle_notifier_v1_set_inhibited(session->idle_notifier, inhibited);
}

static void
leme_session_handle_inhibitor_destroy(
    struct wl_listener *listener, void *data)
{
    struct leme_idle_inhibitor *wrapper =
        wl_container_of(listener, wrapper, destroy);
    struct leme_server *server = wrapper->session->server;

    (void)data;
    wl_list_remove(&wrapper->destroy.link);
    wl_list_remove(&wrapper->link);
    free(wrapper);
    leme_session_refresh_idle_inhibitors(server);
}

static void
leme_session_handle_new_inhibitor(struct wl_listener *listener, void *data)
{
    struct leme_session *session =
        wl_container_of(listener, session, new_inhibitor);
    struct wlr_idle_inhibitor_v1 *inhibitor = data;
    struct leme_idle_inhibitor *wrapper = calloc(1, sizeof(*wrapper));

    if (wrapper == NULL) {
        wlr_log(WLR_ERROR, "%s", "leme: failed to track idle inhibitor");
        return;
    }
    wrapper->session = session;
    wrapper->inhibitor = inhibitor;
    wrapper->destroy.notify = leme_session_handle_inhibitor_destroy;
    wl_signal_add(&inhibitor->events.destroy, &wrapper->destroy);
    wl_list_insert(&session->inhibitors, &wrapper->link);
    leme_session_refresh_idle_inhibitors(session->server);
}

bool
leme_session_init(struct leme_server *server)
{
    struct leme_session *session = calloc(1, sizeof(*session));

    if (session == NULL) {
        wlr_log(WLR_ERROR, "%s",
            "leme: failed to create session and idle protocols");
        return false;
    }
    session->server = server;
    wl_list_init(&session->inhibitors);
    wl_list_init(&session->lock_surfaces);
    session->lock_manager =
        wlr_session_lock_manager_v1_create(server->display);
    session->idle_notifier = wlr_idle_notifier_v1_create(server->display);
    session->idle_inhibit_manager =
        wlr_idle_inhibit_v1_create(server->display);
    if (session->lock_manager == NULL || session->idle_notifier == NULL ||
            session->idle_inhibit_manager == NULL ||
            !leme_render_session_create(session)) {
        wlr_log(WLR_ERROR, "%s",
            "leme: failed to create session and idle protocols");
        free(session);
        return false;
    }
    session->new_lock.notify = leme_session_handle_new_lock;
    wl_signal_add(&session->lock_manager->events.new_lock,
        &session->new_lock);
    session->new_inhibitor.notify = leme_session_handle_new_inhibitor;
    wl_signal_add(&session->idle_inhibit_manager->events.new_inhibitor,
        &session->new_inhibitor);
    server->session_protocols = session;
    return true;
}

void
leme_session_finish(struct leme_server *server)
{
    struct leme_session *session = server->session_protocols;
    struct leme_idle_inhibitor *inhibitor;
    struct leme_idle_inhibitor *temporary;
    struct leme_lock_surface *surface;
    struct leme_lock_surface *surface_temporary;

    if (session == NULL) {
        return;
    }
    if (session->lock != NULL) {
        wl_list_remove(&session->lock_new_surface.link);
        wl_list_remove(&session->lock_unlock.link);
        wl_list_remove(&session->lock_destroy.link);
        session->lock = NULL;
    }
    wl_list_for_each_safe(surface, surface_temporary,
            &session->lock_surfaces, link) {
        wl_list_remove(&surface->commit.link);
        wl_list_remove(&surface->destroy.link);
        wl_list_remove(&surface->link);
        leme_render_lock_surface_destroy(surface);
        free(surface);
    }
    leme_render_session_destroy(session);
    wl_list_for_each_safe(inhibitor, temporary,
            &session->inhibitors, link) {
        wl_list_remove(&inhibitor->destroy.link);
        wl_list_remove(&inhibitor->link);
        free(inhibitor);
    }
    if (session->new_lock.link.next != NULL) {
        wl_list_remove(&session->new_lock.link);
    }
    if (session->new_inhibitor.link.next != NULL) {
        wl_list_remove(&session->new_inhibitor.link);
    }
    free(session);
    server->session_protocols = NULL;
}

void
leme_session_notify_activity(struct leme_server *server)
{
    if (server->session_protocols != NULL) {
        wlr_idle_notifier_v1_notify_activity(
            server->session_protocols->idle_notifier, server->seat);
    }
}

bool
leme_session_locked(const struct leme_server *server)
{
    return server->session_protocols != NULL &&
        server->session_protocols->locked;
}
