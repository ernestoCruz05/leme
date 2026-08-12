#include "protocols/data.h"

#include "input/input.h"
#include "protocols/input.h"
#include "render/render.h"
#include "core/server.h"
#include "protocols/session.h"

#include <stdlib.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

static void
leme_data_handle_selection(struct wl_listener *listener, void *data)
{
    struct leme_data *exchange =
        wl_container_of(listener, exchange, request_set_selection);
    struct wlr_seat_request_set_selection_event *event = data;

    wlr_seat_set_selection(exchange->server->seat,
        event->source, event->serial);
}

static void
leme_data_handle_primary_selection(struct wl_listener *listener, void *data)
{
    struct leme_data *exchange =
        wl_container_of(listener, exchange, request_set_primary_selection);
    struct wlr_seat_request_set_primary_selection_event *event = data;

    wlr_seat_set_primary_selection(exchange->server->seat,
        event->source, event->serial);
}

static void
leme_data_drag_icon_finish(struct leme_drag_icon *icon)
{
    struct leme_data *exchange = icon->data;

    if (exchange->drag_icon == icon) {
        exchange->drag_icon = NULL;
    }
    leme_render_drag_icon_destroy(icon);
    if (icon->destroy.link.next != NULL) {
        wl_list_remove(&icon->destroy.link);
    }
    free(icon);
}

static void
leme_data_handle_drag_icon_destroy(struct wl_listener *listener, void *data)
{
    struct leme_drag_icon *icon =
        wl_container_of(listener, icon, destroy);

    (void)data;
    leme_data_drag_icon_finish(icon);
}

static void
leme_data_handle_request_start_drag(struct wl_listener *listener, void *data)
{
    struct leme_data *exchange =
        wl_container_of(listener, exchange, request_start_drag);
    struct wlr_seat_request_start_drag_event *event = data;

    if (leme_session_locked(exchange->server)) {
        if (event->drag->source != NULL) {
            wlr_data_source_destroy(event->drag->source);
        } else if (event->drag->seat_client != NULL) {
            wl_client_destroy(event->drag->seat_client->client);
        }
        return;
    }
    if (wlr_seat_validate_pointer_grab_serial(exchange->server->seat,
            event->origin, event->serial)) {
        leme_input_pointer_grab_cancel(exchange->server);
        wlr_seat_start_pointer_drag(exchange->server->seat,
            event->drag, event->serial);
        return;
    }
    wlr_log(WLR_ERROR, "%s", "leme: rejected drag with invalid pointer serial");
    if (event->drag->source != NULL) {
        wlr_data_source_destroy(event->drag->source);
    }
}

static void
leme_data_handle_start_drag(struct wl_listener *listener, void *data)
{
    struct leme_data *exchange =
        wl_container_of(listener, exchange, start_drag);
    struct wlr_drag *drag = data;
    struct leme_drag_icon *icon;

    leme_input_protocols_cancel_constraint(exchange->server);
    if (drag->icon == NULL) {
        return;
    }
    if (exchange->drag_icon != NULL) {
        leme_data_drag_icon_finish(exchange->drag_icon);
    }
    icon = calloc(1, sizeof(*icon));
    if (icon == NULL) {
        wlr_log(WLR_ERROR, "%s", "leme: failed to allocate drag icon");
        return;
    }
    icon->data = exchange;
    icon->wlr_drag_icon = drag->icon;
    icon->destroy.notify = leme_data_handle_drag_icon_destroy;
    wl_signal_add(&drag->icon->events.destroy, &icon->destroy);
    exchange->drag_icon = icon;
    if (!leme_render_drag_icon_create(icon)) {
        wlr_log(WLR_ERROR, "%s", "leme: failed to render drag icon");
        leme_data_drag_icon_finish(icon);
        return;
    }
    leme_data_update_drag_icon(exchange->server);
}

void
leme_data_cancel_drag(struct leme_server *server)
{
    struct wlr_drag *drag;

    if (server == NULL || server->seat == NULL || server->seat->drag == NULL) {
        return;
    }
    drag = server->seat->drag;
    if (drag->source != NULL) {
        wlr_data_source_destroy(drag->source);
    } else if (drag->seat_client != NULL) {
        wl_client_destroy(drag->seat_client->client);
    }
}

void
leme_data_update_drag_icon(struct leme_server *server)
{
    if (server->data != NULL && server->data->drag_icon != NULL &&
            server->cursor != NULL) {
        leme_render_drag_icon_update(server->data->drag_icon,
            server->cursor->x, server->cursor->y);
    }
}

bool
leme_data_init(struct leme_server *server)
{
    struct leme_data *exchange = calloc(1, sizeof(*exchange));

    if (exchange == NULL) {
        return false;
    }
    exchange->server = server;
    exchange->data_device_manager =
        wlr_data_device_manager_create(server->display);
    exchange->primary_selection_manager =
        wlr_primary_selection_v1_device_manager_create(server->display);
    exchange->wlr_data_control_manager =
        wlr_data_control_manager_v1_create(server->display);
    exchange->ext_data_control_manager =
        wlr_ext_data_control_manager_v1_create(server->display, 1);
    if (exchange->data_device_manager == NULL ||
            exchange->primary_selection_manager == NULL ||
            exchange->wlr_data_control_manager == NULL ||
            exchange->ext_data_control_manager == NULL) {
        wlr_log(WLR_ERROR, "%s",
            "leme: failed to create data exchange globals");
        free(exchange);
        return false;
    }

    exchange->request_set_selection.notify = leme_data_handle_selection;
    wl_signal_add(&server->seat->events.request_set_selection,
        &exchange->request_set_selection);
    exchange->request_set_primary_selection.notify =
        leme_data_handle_primary_selection;
    wl_signal_add(&server->seat->events.request_set_primary_selection,
        &exchange->request_set_primary_selection);
    exchange->request_start_drag.notify = leme_data_handle_request_start_drag;
    wl_signal_add(&server->seat->events.request_start_drag,
        &exchange->request_start_drag);
    exchange->start_drag.notify = leme_data_handle_start_drag;
    wl_signal_add(&server->seat->events.start_drag, &exchange->start_drag);
    server->data = exchange;
    return true;
}

void
leme_data_finish(struct leme_server *server)
{
    struct leme_data *exchange = server->data;

    if (exchange == NULL) {
        return;
    }
    if (exchange->drag_icon != NULL) {
        leme_data_drag_icon_finish(exchange->drag_icon);
    }
    if (exchange->request_set_selection.link.next != NULL) {
        wl_list_remove(&exchange->request_set_selection.link);
    }
    if (exchange->request_set_primary_selection.link.next != NULL) {
        wl_list_remove(&exchange->request_set_primary_selection.link);
    }
    if (exchange->request_start_drag.link.next != NULL) {
        wl_list_remove(&exchange->request_start_drag.link);
    }
    if (exchange->start_drag.link.next != NULL) {
        wl_list_remove(&exchange->start_drag.link);
    }
    free(exchange);
    server->data = NULL;
}
