#include "protocols/input.h"

#include "input/input.h"
#include "render/render.h"
#include "core/server.h"

#include <math.h>
#include <stdlib.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_keyboard_shortcuts_inhibit_v1.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>

struct leme_pointer_constraint {
    struct leme_input_protocols *protocols;
    struct wlr_pointer_constraint_v1 *constraint;
    struct wl_list link;
    struct wl_listener set_region;
    struct wl_listener destroy;
};

struct leme_shortcuts_inhibitor {
    struct leme_input_protocols *protocols;
    struct wlr_keyboard_shortcuts_inhibitor_v1 *inhibitor;
    struct wl_list link;
    struct wl_listener destroy;
};

struct leme_input_protocols {
    struct leme_server *server;
    struct wlr_relative_pointer_manager_v1 *relative_pointer_manager;
    struct wlr_pointer_constraints_v1 *pointer_constraints;
    struct wlr_keyboard_shortcuts_inhibit_manager_v1 *shortcuts_inhibit;
    struct wl_list constraints;
    struct wl_list inhibitors;
    struct wlr_pointer_constraint_v1 *active_constraint;
    struct leme_shortcuts_inhibitor *active_inhibitor;
    struct wl_event_source *refresh_idle;
    struct wl_listener new_constraint;
    struct wl_listener new_inhibitor;
};

static bool
leme_input_protocols_constraint_hit(
    struct leme_input_protocols *protocols,
    struct wlr_pointer_constraint_v1 *constraint,
    struct leme_render_hit *hit)
{
    struct leme_server *server = protocols->server;

    if (!leme_render_at(server, server->cursor->x, server->cursor->y, hit) ||
            hit->surface != constraint->surface ||
            server->seat->pointer_state.focused_surface !=
                constraint->surface) {
        return false;
    }
    return pixman_region32_contains_point(&constraint->region,
        (int)floor(hit->surface_x), (int)floor(hit->surface_y), NULL);
}

static bool
leme_input_protocols_cursor_hint(
    struct leme_input_protocols *protocols,
    struct wlr_pointer_constraint_v1 *constraint,
    double *hint_lx, double *hint_ly)
{
    struct leme_render_hit hit;

    if (constraint->type != WLR_POINTER_CONSTRAINT_V1_LOCKED ||
            !constraint->current.cursor_hint.enabled ||
            !leme_render_at(protocols->server,
                protocols->server->cursor->x,
                protocols->server->cursor->y, &hit) ||
            hit.surface != constraint->surface) {
        return false;
    }
    *hint_lx = protocols->server->cursor->x +
        constraint->current.cursor_hint.x - hit.surface_x;
    *hint_ly = protocols->server->cursor->y +
        constraint->current.cursor_hint.y - hit.surface_y;
    return true;
}

static void leme_input_protocols_schedule_refresh(
    struct leme_input_protocols *protocols);

static void
leme_input_protocols_deactivate_constraint(
    struct leme_input_protocols *protocols)
{
    struct wlr_pointer_constraint_v1 *constraint =
        protocols->active_constraint;
    double hint_lx = 0.0;
    double hint_ly = 0.0;
    bool have_hint;
    bool was_locked;

    if (constraint == NULL) {
        return;
    }
    was_locked = constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED;
    have_hint = leme_input_protocols_cursor_hint(
        protocols, constraint, &hint_lx, &hint_ly);
    protocols->active_constraint = NULL;
    wlr_pointer_constraint_v1_send_deactivated(constraint);
    if (have_hint) {
        wlr_cursor_warp(protocols->server->cursor, NULL, hint_lx, hint_ly);
    }
    if (was_locked) {
        leme_input_protocols_schedule_refresh(protocols);
    }
}

static void
leme_input_protocols_activate_constraint(
    struct leme_input_protocols *protocols,
    struct wlr_pointer_constraint_v1 *constraint)
{
    if (protocols->active_constraint == constraint) {
        return;
    }
    leme_input_protocols_deactivate_constraint(protocols);
    protocols->active_constraint = constraint;
    wlr_pointer_constraint_v1_send_activated(constraint);
    if (constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
        wlr_cursor_unset_image(protocols->server->cursor);
    }
}

void
leme_input_protocols_update_pointer_focus(struct leme_server *server)
{
    struct leme_input_protocols *protocols = server->input_protocols;
    struct wlr_pointer_constraint_v1 *constraint = NULL;
    struct wlr_surface *surface;
    struct leme_render_hit hit;

    if (protocols == NULL) {
        return;
    }
    if (leme_input_pointer_grab_active(server)) {
        leme_input_protocols_deactivate_constraint(protocols);
        return;
    }
    surface = server->seat->pointer_state.focused_surface;
    if (surface != NULL && server->seat->drag == NULL) {
        constraint = wlr_pointer_constraints_v1_constraint_for_surface(
            protocols->pointer_constraints, surface, server->seat);
        if (constraint != NULL &&
                !leme_input_protocols_constraint_hit(
                    protocols, constraint, &hit)) {
            constraint = NULL;
        }
    }
    if (constraint == NULL) {
        leme_input_protocols_deactivate_constraint(protocols);
    } else {
        leme_input_protocols_activate_constraint(protocols, constraint);
    }
}

static void
leme_input_protocols_handle_constraint_region(
    struct wl_listener *listener, void *data)
{
    struct leme_pointer_constraint *wrapper =
        wl_container_of(listener, wrapper, set_region);

    (void)data;
    leme_input_protocols_update_pointer_focus(wrapper->protocols->server);
}

static void
leme_input_protocols_refresh_idle(void *data)
{
    struct leme_input_protocols *protocols = data;

    protocols->refresh_idle = NULL;
    leme_input_refresh_pointer_focus(protocols->server, 0);
}

static void
leme_input_protocols_schedule_refresh(
    struct leme_input_protocols *protocols)
{
    if (protocols->refresh_idle == NULL) {
        protocols->refresh_idle = wl_event_loop_add_idle(
            wl_display_get_event_loop(protocols->server->display),
            leme_input_protocols_refresh_idle, protocols);
    }
}

static void
leme_input_protocols_handle_constraint_destroy(
    struct wl_listener *listener, void *data)
{
    struct leme_pointer_constraint *wrapper =
        wl_container_of(listener, wrapper, destroy);
    struct leme_input_protocols *protocols = wrapper->protocols;
    double hint_lx = 0.0;
    double hint_ly = 0.0;
    bool was_active = protocols->active_constraint == wrapper->constraint;
    bool was_locked = was_active &&
        wrapper->constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED;
    bool have_hint = was_active && leme_input_protocols_cursor_hint(
        protocols, wrapper->constraint, &hint_lx, &hint_ly);

    (void)data;
    if (was_active) {
        protocols->active_constraint = NULL;
    }
    wl_list_remove(&wrapper->set_region.link);
    wl_list_remove(&wrapper->destroy.link);
    wl_list_remove(&wrapper->link);
    free(wrapper);
    if (have_hint) {
        wlr_cursor_warp(protocols->server->cursor, NULL, hint_lx, hint_ly);
    }
    if (was_locked) {
        leme_input_protocols_schedule_refresh(protocols);
    }
}

static void
leme_input_protocols_handle_new_constraint(
    struct wl_listener *listener, void *data)
{
    struct leme_input_protocols *protocols =
        wl_container_of(listener, protocols, new_constraint);
    struct wlr_pointer_constraint_v1 *constraint = data;
    struct leme_pointer_constraint *wrapper = calloc(1, sizeof(*wrapper));

    if (wrapper == NULL) {
        wlr_log(WLR_ERROR, "%s", "leme: failed to track pointer constraint");
        return;
    }
    wrapper->protocols = protocols;
    wrapper->constraint = constraint;
    wrapper->set_region.notify =
        leme_input_protocols_handle_constraint_region;
    wl_signal_add(&constraint->events.set_region, &wrapper->set_region);
    wrapper->destroy.notify = leme_input_protocols_handle_constraint_destroy;
    wl_signal_add(&constraint->events.destroy, &wrapper->destroy);
    wl_list_insert(&protocols->constraints, &wrapper->link);
    leme_input_protocols_update_pointer_focus(protocols->server);
}

void
leme_input_protocols_update_keyboard_focus(struct leme_server *server)
{
    struct leme_input_protocols *protocols = server->input_protocols;
    struct leme_shortcuts_inhibitor *wrapper;
    struct leme_shortcuts_inhibitor *target = NULL;
    struct wlr_surface *focused;

    if (protocols == NULL) {
        return;
    }
    focused = server->seat->keyboard_state.focused_surface;
    wl_list_for_each(wrapper, &protocols->inhibitors, link) {
        if (wrapper->inhibitor->seat == server->seat &&
                wrapper->inhibitor->surface == focused) {
            target = wrapper;
            break;
        }
    }
    if (protocols->active_inhibitor == target) {
        return;
    }
    if (protocols->active_inhibitor != NULL) {
        wlr_keyboard_shortcuts_inhibitor_v1_deactivate(
            protocols->active_inhibitor->inhibitor);
    }
    protocols->active_inhibitor = target;
    if (target != NULL) {
        wlr_keyboard_shortcuts_inhibitor_v1_activate(target->inhibitor);
        leme_input_set_mode(server, "common");
    }
}

bool
leme_input_protocols_shortcuts_inhibited(const struct leme_server *server)
{
    const struct leme_input_protocols *protocols = server->input_protocols;

    return protocols != NULL && protocols->active_inhibitor != NULL &&
        protocols->active_inhibitor->inhibitor->active &&
        protocols->active_inhibitor->inhibitor->seat == server->seat &&
        protocols->active_inhibitor->inhibitor->surface ==
            server->seat->keyboard_state.focused_surface;
}

static void
leme_input_protocols_handle_inhibitor_destroy(
    struct wl_listener *listener, void *data)
{
    struct leme_shortcuts_inhibitor *wrapper =
        wl_container_of(listener, wrapper, destroy);
    struct leme_input_protocols *protocols = wrapper->protocols;

    (void)data;
    if (protocols->active_inhibitor == wrapper) {
        protocols->active_inhibitor = NULL;
    }
    wl_list_remove(&wrapper->destroy.link);
    wl_list_remove(&wrapper->link);
    free(wrapper);
    leme_input_protocols_update_keyboard_focus(protocols->server);
}

static void
leme_input_protocols_handle_new_inhibitor(
    struct wl_listener *listener, void *data)
{
    struct leme_input_protocols *protocols =
        wl_container_of(listener, protocols, new_inhibitor);
    struct wlr_keyboard_shortcuts_inhibitor_v1 *inhibitor = data;
    struct leme_shortcuts_inhibitor *wrapper = calloc(1, sizeof(*wrapper));

    if (wrapper == NULL) {
        wlr_log(WLR_ERROR, "%s", "leme: failed to track shortcut inhibitor");
        return;
    }
    wrapper->protocols = protocols;
    wrapper->inhibitor = inhibitor;
    wrapper->destroy.notify = leme_input_protocols_handle_inhibitor_destroy;
    wl_signal_add(&inhibitor->events.destroy, &wrapper->destroy);
    wl_list_insert(&protocols->inhibitors, &wrapper->link);
    leme_input_protocols_update_keyboard_focus(protocols->server);
}

bool
leme_input_protocols_init(struct leme_server *server)
{
    struct leme_input_protocols *protocols = calloc(1, sizeof(*protocols));

    if (protocols == NULL) {
        return false;
    }
    protocols->server = server;
    wl_list_init(&protocols->constraints);
    wl_list_init(&protocols->inhibitors);
    protocols->relative_pointer_manager =
        wlr_relative_pointer_manager_v1_create(server->display);
    protocols->pointer_constraints =
        wlr_pointer_constraints_v1_create(server->display);
    protocols->shortcuts_inhibit =
        wlr_keyboard_shortcuts_inhibit_v1_create(server->display);
    if (protocols->relative_pointer_manager == NULL ||
            protocols->pointer_constraints == NULL ||
            protocols->shortcuts_inhibit == NULL) {
        wlr_log(WLR_ERROR, "%s",
            "leme: failed to create gaming input protocols");
        free(protocols);
        return false;
    }
    protocols->new_constraint.notify =
        leme_input_protocols_handle_new_constraint;
    wl_signal_add(&protocols->pointer_constraints->events.new_constraint,
        &protocols->new_constraint);
    protocols->new_inhibitor.notify =
        leme_input_protocols_handle_new_inhibitor;
    wl_signal_add(&protocols->shortcuts_inhibit->events.new_inhibitor,
        &protocols->new_inhibitor);
    server->input_protocols = protocols;
    return true;
}

void
leme_input_protocols_finish(struct leme_server *server)
{
    struct leme_input_protocols *protocols = server->input_protocols;
    struct leme_pointer_constraint *constraint;
    struct leme_pointer_constraint *constraint_temporary;
    struct leme_shortcuts_inhibitor *inhibitor;
    struct leme_shortcuts_inhibitor *inhibitor_temporary;

    if (protocols == NULL) {
        return;
    }
    protocols->active_constraint = NULL;
    if (protocols->refresh_idle != NULL) {
        wl_event_source_remove(protocols->refresh_idle);
        protocols->refresh_idle = NULL;
    }
    wl_list_for_each_safe(constraint, constraint_temporary,
            &protocols->constraints, link) {
        wl_list_remove(&constraint->set_region.link);
        wl_list_remove(&constraint->destroy.link);
        wl_list_remove(&constraint->link);
        free(constraint);
    }
    if (protocols->active_inhibitor != NULL) {
        wlr_keyboard_shortcuts_inhibitor_v1_deactivate(
            protocols->active_inhibitor->inhibitor);
        protocols->active_inhibitor = NULL;
    }
    wl_list_for_each_safe(inhibitor, inhibitor_temporary,
            &protocols->inhibitors, link) {
        wl_list_remove(&inhibitor->destroy.link);
        wl_list_remove(&inhibitor->link);
        free(inhibitor);
    }
    if (protocols->new_constraint.link.next != NULL) {
        wl_list_remove(&protocols->new_constraint.link);
    }
    if (protocols->new_inhibitor.link.next != NULL) {
        wl_list_remove(&protocols->new_inhibitor.link);
    }
    free(protocols);
    server->input_protocols = NULL;
}

void
leme_input_protocols_send_relative(struct leme_server *server,
    uint32_t time_msec, double dx, double dy,
    double unaccel_dx, double unaccel_dy)
{
    if (server->input_protocols == NULL) {
        return;
    }
    wlr_relative_pointer_manager_v1_send_relative_motion(
        server->input_protocols->relative_pointer_manager,
        server->seat, (uint64_t)time_msec * 1000,
        dx, dy, unaccel_dx, unaccel_dy);
}

bool
leme_input_protocols_pointer_locked(const struct leme_server *server)
{
    const struct leme_input_protocols *protocols = server->input_protocols;

    return protocols != NULL && protocols->active_constraint != NULL &&
        protocols->active_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED;
}

bool
leme_input_protocols_constrain_motion(struct leme_server *server,
    double *dx, double *dy)
{
    struct leme_input_protocols *protocols = server->input_protocols;
    struct wlr_pointer_constraint_v1 *constraint;
    struct leme_render_hit hit;
    double confined_x;
    double confined_y;

    if (protocols == NULL || protocols->active_constraint == NULL) {
        return false;
    }
    if (leme_input_pointer_grab_active(server)) {
        leme_input_protocols_deactivate_constraint(protocols);
        return false;
    }
    constraint = protocols->active_constraint;
    if (server->seat->drag != NULL ||
            !leme_input_protocols_constraint_hit(
                protocols, constraint, &hit)) {
        leme_input_protocols_deactivate_constraint(protocols);
        return false;
    }
    if (constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
        return true;
    }
    if (!wlr_region_confine(&constraint->region,
            hit.surface_x, hit.surface_y,
            hit.surface_x + *dx, hit.surface_y + *dy,
            &confined_x, &confined_y)) {
        leme_input_protocols_deactivate_constraint(protocols);
        return false;
    }
    *dx = confined_x - hit.surface_x;
    *dy = confined_y - hit.surface_y;
    return false;
}

void
leme_input_protocols_cancel_constraint(struct leme_server *server)
{
    if (server->input_protocols != NULL) {
        leme_input_protocols_deactivate_constraint(server->input_protocols);
    }
}
