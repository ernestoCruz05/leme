#include "protocols/desktop.h"

#include "config/config.h"
#include "core/server.h"
#include "input/input.h"
#include "output/output.h"
#include "protocols/input.h"
#include "protocols/session.h"
#include "shell/layer.h"
#include "shell/sticky.h"
#include "shell/view.h"
#include "shell/xwayland.h"
#include "workspace/tag.h"

#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_dialog_v1.h>
#include <wlr/util/log.h>

struct leme_xdg_decoration {
    struct wlr_xdg_toplevel_decoration_v1 *decoration;
    struct wl_listener request_mode;
    struct wl_listener destroy;
    struct wl_list link;
};

struct leme_server_decoration {
    struct wlr_server_decoration *decoration;
    struct wl_listener mode;
    struct wl_listener destroy;
    struct wl_list link;
};

struct leme_desktop {
    struct leme_server *server;
    struct wlr_xdg_activation_v1 *activation;
    struct wlr_xdg_decoration_manager_v1 *xdg_decoration_manager;
    struct wlr_server_decoration_manager *server_decoration_manager;
    struct wlr_cursor_shape_manager_v1 *cursor_shape;
    struct wlr_xcursor_manager *xcursor;
    const char *cursor_name;
    char *cursor_override_name;
    float cursor_scale;
    bool cursor_themed;
    bool cursor_overridden;
    struct wl_list xdg_decorations;
    struct wl_list server_decorations;
    struct wl_listener request_activate;
    struct wl_listener new_xdg_decoration;
    struct wl_listener new_server_decoration;
    struct wl_listener request_set_shape;
};

static bool
leme_desktop_same_root(struct wlr_surface *left, struct wlr_surface *right)
{
    return left != NULL && right != NULL &&
        wlr_surface_get_root_surface(left) ==
        wlr_surface_get_root_surface(right);
}

static bool
leme_desktop_activation_source_valid(struct leme_server *server,
    const struct wlr_xdg_activation_token_v1 *token)
{
    struct wl_client *client;
    struct wlr_seat_client *seat_client;
    struct wlr_surface *source;

    if (token->seat != server->seat || token->surface == NULL) {
        return false;
    }
    source = wlr_surface_get_root_surface(token->surface);
    client = wl_resource_get_client(token->surface->resource);
    seat_client = wlr_seat_client_for_wl_client(server->seat, client);
    return seat_client != NULL &&
        wlr_seat_client_validate_event_serial(seat_client, token->serial) &&
        (leme_desktop_same_root(source,
            server->seat->keyboard_state.focused_surface) ||
        leme_desktop_same_root(source,
            server->seat->pointer_state.focused_surface));
}

bool
leme_desktop_activation_target_eligible(const struct leme_server *server,
    const struct leme_view *view)
{
    const struct leme_tags *tags;

    if (server == NULL || !leme_ownership_activation_eligible(view)) {
        return false;
    }
    if (leme_view_is_sticky(view)) {
        return leme_view_output(view) != NULL;
    }
    if (leme_view_is_shown_scratchpad(view)) {
        return leme_view_output(view) == leme_output_focused(server);
    }
    tags = leme_focused_tags(server);
    return tags != NULL && !tags->focused_is_candidate &&
        leme_ownership_tag(view) != NULL && leme_ownership_tag(view)->id == tags->focused_id;
}

static void
leme_desktop_handle_activate(struct wl_listener *listener, void *data)
{
    struct leme_desktop *desktop =
        wl_container_of(listener, desktop, request_activate);
    struct leme_server *server = desktop->server;
    struct wlr_xdg_activation_v1_request_activate_event *event = data;
    struct leme_view *view;

    if (leme_session_locked(server) ||
            leme_layer_keyboard_is_exclusive(server) ||
            !leme_desktop_activation_source_valid(server, event->token)) {
        return;
    }
    view = leme_view_from_surface(server, event->surface);
    if (leme_desktop_activation_target_eligible(server, view)) {
        if (leme_view_is_sticky(view)) {
            leme_output_set_focused(server, leme_view_output(view), false);
        }
        leme_view_focus(view);
    }
}

static void
leme_desktop_xdg_decoration_finish(struct leme_xdg_decoration *wrapper)
{
    wl_list_remove(&wrapper->request_mode.link);
    wl_list_remove(&wrapper->destroy.link);
    wl_list_remove(&wrapper->link);
    free(wrapper);
}

static void
leme_desktop_set_xdg_decoration_mode(
    struct wlr_xdg_toplevel_decoration_v1 *decoration)
{
    if (decoration->toplevel->base->initialized) {
        wlr_xdg_toplevel_decoration_v1_set_mode(decoration,
            WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    } else {
        decoration->scheduled_mode =
            WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
    }
}

static void
leme_desktop_handle_xdg_decoration_mode(
    struct wl_listener *listener, void *data)
{
    struct leme_xdg_decoration *wrapper =
        wl_container_of(listener, wrapper, request_mode);

    (void)data;
    leme_desktop_set_xdg_decoration_mode(wrapper->decoration);
}

static void
leme_desktop_handle_xdg_decoration_destroy(
    struct wl_listener *listener, void *data)
{
    struct leme_xdg_decoration *wrapper =
        wl_container_of(listener, wrapper, destroy);

    (void)data;
    leme_desktop_xdg_decoration_finish(wrapper);
}

static void
leme_desktop_handle_new_xdg_decoration(
    struct wl_listener *listener, void *data)
{
    struct leme_desktop *desktop =
        wl_container_of(listener, desktop, new_xdg_decoration);
    struct wlr_xdg_toplevel_decoration_v1 *decoration = data;
    struct leme_xdg_decoration *wrapper = calloc(1, sizeof(*wrapper));

    leme_desktop_set_xdg_decoration_mode(decoration);
    if (wrapper == NULL) {
        wlr_log(WLR_ERROR, "%s",
            "leme: failed to track XDG decoration");
        return;
    }
    wrapper->decoration = decoration;
    wrapper->request_mode.notify = leme_desktop_handle_xdg_decoration_mode;
    wl_signal_add(&decoration->events.request_mode, &wrapper->request_mode);
    wrapper->destroy.notify = leme_desktop_handle_xdg_decoration_destroy;
    wl_signal_add(&decoration->events.destroy, &wrapper->destroy);
    wl_list_insert(&desktop->xdg_decorations, &wrapper->link);
}

static void
leme_desktop_server_decoration_finish(
    struct leme_server_decoration *wrapper)
{
    wl_list_remove(&wrapper->mode.link);
    wl_list_remove(&wrapper->destroy.link);
    wl_list_remove(&wrapper->link);
    free(wrapper);
}

static void
leme_desktop_handle_server_decoration_mode(
    struct wl_listener *listener, void *data)
{
    struct leme_server_decoration *wrapper =
        wl_container_of(listener, wrapper, mode);

    (void)data;
    wrapper->decoration->mode = WLR_SERVER_DECORATION_MANAGER_MODE_SERVER;
}

static void
leme_desktop_handle_server_decoration_destroy(
    struct wl_listener *listener, void *data)
{
    struct leme_server_decoration *wrapper =
        wl_container_of(listener, wrapper, destroy);

    (void)data;
    leme_desktop_server_decoration_finish(wrapper);
}

static void
leme_desktop_handle_new_server_decoration(
    struct wl_listener *listener, void *data)
{
    struct leme_desktop *desktop =
        wl_container_of(listener, desktop, new_server_decoration);
    struct wlr_server_decoration *decoration = data;
    struct leme_server_decoration *wrapper = calloc(1, sizeof(*wrapper));

    decoration->mode = WLR_SERVER_DECORATION_MANAGER_MODE_SERVER;
    if (wrapper == NULL) {
        wlr_log(WLR_ERROR, "%s",
            "leme: failed to track server decoration");
        return;
    }
    wrapper->decoration = decoration;
    wrapper->mode.notify = leme_desktop_handle_server_decoration_mode;
    wl_signal_add(&decoration->events.mode, &wrapper->mode);
    wrapper->destroy.notify = leme_desktop_handle_server_decoration_destroy;
    wl_signal_add(&decoration->events.destroy, &wrapper->destroy);
    wl_list_insert(&desktop->server_decorations, &wrapper->link);
}

static void
leme_desktop_handle_cursor_shape(struct wl_listener *listener, void *data)
{
    struct leme_desktop *desktop =
        wl_container_of(listener, desktop, request_set_shape);
    struct leme_server *server = desktop->server;
    struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;
    struct wlr_surface *focused =
        server->seat->pointer_state.focused_surface;
    const char *name;

    if (event->device_type !=
            WLR_CURSOR_SHAPE_MANAGER_V1_DEVICE_TYPE_POINTER ||
            event->seat_client !=
                server->seat->pointer_state.focused_client ||
            !wlr_seat_client_validate_event_serial(
                event->seat_client, event->serial) ||
            !leme_session_surface_allowed(server, focused) ||
            leme_input_protocols_pointer_locked(server) ||
            leme_input_pointer_grab_active(server)) {
        return;
    }
    name = wlr_cursor_shape_v1_name(event->shape);
    if (name == NULL || wlr_xcursor_manager_get_xcursor(
            desktop->xcursor, name, desktop->cursor_scale) == NULL) {
        return;
    }
    wlr_cursor_set_xcursor(server->cursor, desktop->xcursor, name);
    desktop->cursor_name = name;
    desktop->cursor_themed = true;
}

void
leme_desktop_cursor_surface_set(struct leme_server *server)
{
    if (server != NULL && server->desktop != NULL &&
            !leme_input_pointer_grab_active(server)) {
        server->desktop->cursor_themed = false;
    }
}

bool
leme_desktop_cursor_override(struct leme_server *server, const char *name)
{
    struct leme_desktop *desktop;
    char *copy;

    if (server == NULL || server->desktop == NULL || name == NULL) {
        return false;
    }
    desktop = server->desktop;
    if (wlr_xcursor_manager_get_xcursor(
            desktop->xcursor, name, desktop->cursor_scale) == NULL) {
        return false;
    }
    copy = strdup(name);
    if (copy == NULL) {
        return false;
    }
    free(desktop->cursor_override_name);
    desktop->cursor_override_name = copy;
    desktop->cursor_overridden = true;
    wlr_cursor_set_xcursor(server->cursor, desktop->xcursor, name);
    return true;
}

void
leme_desktop_cursor_restore(struct leme_server *server)
{
    struct leme_desktop *desktop;
    const char *name = "left_ptr";

    if (server == NULL || server->desktop == NULL) {
        return;
    }
    desktop = server->desktop;
    desktop->cursor_overridden = false;
    free(desktop->cursor_override_name);
    desktop->cursor_override_name = NULL;
    if (desktop->cursor_themed && desktop->cursor_name != NULL &&
            wlr_xcursor_manager_get_xcursor(desktop->xcursor,
                desktop->cursor_name, desktop->cursor_scale) != NULL) {
        name = desktop->cursor_name;
    }
    wlr_cursor_set_xcursor(server->cursor, desktop->xcursor, name);
}

static bool
leme_desktop_load_cursor_scales(struct leme_server *server)
{
    struct leme_desktop *desktop = server->desktop;
    struct leme_output *output;
    bool loaded = true;

    if (server->outputs.next == NULL) {
        return wlr_xcursor_manager_load(desktop->xcursor, 1.0f);
    }
    wl_list_for_each(output, &server->outputs, link) {
        if (!output->wlr_output->enabled) {
            continue;
        }
        if (!wlr_xcursor_manager_load(
                desktop->xcursor, output->wlr_output->scale)) {
            wlr_log(WLR_ERROR,
                "leme: failed to load cursor theme at scale %.2f",
                (double)output->wlr_output->scale);
            loaded = false;
        }
    }
    return loaded;
}

void
leme_desktop_output_changed(struct leme_server *server)
{
    struct leme_desktop *desktop;
    float scale;

    if (server == NULL || server->desktop == NULL ||
            server->desktop->xcursor == NULL) {
        return;
    }
    desktop = server->desktop;
    if (!leme_desktop_load_cursor_scales(server)) {
        return;
    }
    scale = leme_output_focused(server) == NULL ? 1.0f :
        leme_output_focused(server)->wlr_output->scale;
    if (scale == desktop->cursor_scale) {
        return;
    }
    desktop->cursor_scale = scale;
    if (desktop->cursor_overridden) {
        const char *name = desktop->cursor_override_name;

        if (name == NULL || wlr_xcursor_manager_get_xcursor(
                desktop->xcursor, name, scale) == NULL) {
            name = "left_ptr";
        }
        wlr_cursor_set_xcursor(server->cursor, desktop->xcursor, name);
    } else if (desktop->cursor_themed && desktop->cursor_name != NULL &&
            wlr_xcursor_manager_get_xcursor(desktop->xcursor,
                desktop->cursor_name, scale) != NULL) {
        wlr_cursor_set_xcursor(server->cursor,
            desktop->xcursor, desktop->cursor_name);
    }
}

bool
leme_desktop_init(struct leme_server *server)
{
    struct leme_desktop *desktop = calloc(1, sizeof(*desktop));

    if (desktop == NULL) {
        wlr_log(WLR_ERROR, "%s",
            "leme: failed to initialize desktop protocols");
        return false;
    }
    desktop->server = server;
    wl_list_init(&desktop->xdg_decorations);
    wl_list_init(&desktop->server_decorations);
    server->desktop = desktop;
    desktop->activation = wlr_xdg_activation_v1_create(server->display);
    server->xdg_dialog_manager =
        wlr_xdg_wm_dialog_v1_create(server->display, 1);
    desktop->xdg_decoration_manager =
        wlr_xdg_decoration_manager_v1_create(server->display);
    desktop->server_decoration_manager =
        wlr_server_decoration_manager_create(server->display);
    desktop->cursor_shape =
        wlr_cursor_shape_manager_v1_create(server->display, 1);
    desktop->xcursor = wlr_xcursor_manager_create(
        server->config == NULL ? NULL : server->config->cursor.theme,
        server->config == NULL ? LEME_CURSOR_SIZE_DEFAULT :
            (uint32_t)server->config->cursor.size);
    if (desktop->activation == NULL ||
            server->xdg_dialog_manager == NULL ||
            desktop->xdg_decoration_manager == NULL ||
            desktop->server_decoration_manager == NULL ||
            desktop->cursor_shape == NULL || desktop->xcursor == NULL ||
            !wlr_xcursor_manager_load(desktop->xcursor, 1.0f) ||
            wlr_xcursor_manager_get_xcursor(
                desktop->xcursor, "left_ptr", 1.0f) == NULL) {
        wlr_log(WLR_ERROR, "%s",
            "leme: failed to initialize desktop protocols");
        leme_desktop_finish(server);
        return false;
    }
    wlr_server_decoration_manager_set_default_mode(
        desktop->server_decoration_manager,
        WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
    desktop->request_activate.notify = leme_desktop_handle_activate;
    wl_signal_add(&desktop->activation->events.request_activate,
        &desktop->request_activate);
    desktop->new_xdg_decoration.notify =
        leme_desktop_handle_new_xdg_decoration;
    wl_signal_add(&desktop->xdg_decoration_manager->events.new_toplevel_decoration,
        &desktop->new_xdg_decoration);
    desktop->new_server_decoration.notify =
        leme_desktop_handle_new_server_decoration;
    wl_signal_add(&desktop->server_decoration_manager->events.new_decoration,
        &desktop->new_server_decoration);
    desktop->request_set_shape.notify = leme_desktop_handle_cursor_shape;
    wl_signal_add(&desktop->cursor_shape->events.request_set_shape,
        &desktop->request_set_shape);
    desktop->cursor_name = "left_ptr";
    desktop->cursor_scale = 1.0f;
    desktop->cursor_themed = true;
    wlr_cursor_set_xcursor(server->cursor, desktop->xcursor,
        desktop->cursor_name);
    leme_xwayland_set_root_cursor(server, desktop->xcursor,
        desktop->cursor_name, desktop->cursor_scale);
    return true;
}

bool
leme_desktop_apply_cursor_config(struct leme_server *server,
    const struct leme_config *config)
{
    struct leme_desktop *desktop =
        server == NULL ? NULL : server->desktop;
    struct wlr_xcursor_manager *replacement;
    struct wlr_xcursor_manager *previous;

    if (desktop == NULL || desktop->xcursor == NULL || config == NULL) {
        return false;
    }
    replacement = wlr_xcursor_manager_create(config->cursor.theme,
        (uint32_t)config->cursor.size);
    if (replacement == NULL ||
            !wlr_xcursor_manager_load(replacement, desktop->cursor_scale) ||
            wlr_xcursor_manager_get_xcursor(replacement, "left_ptr",
                desktop->cursor_scale) == NULL) {
        wlr_xcursor_manager_destroy(replacement);
        return false;
    }
    previous = desktop->xcursor;
    desktop->xcursor = replacement;
    (void)leme_desktop_load_cursor_scales(server);
    if (desktop->cursor_themed && desktop->cursor_name != NULL) {
        wlr_cursor_set_xcursor(server->cursor, replacement,
            desktop->cursor_name);
    }
    leme_xwayland_set_root_cursor(server, replacement,
        desktop->cursor_name, desktop->cursor_scale);
    wlr_xcursor_manager_destroy(previous);
    return true;
}

void
leme_desktop_finish(struct leme_server *server)
{
    struct leme_desktop *desktop = server->desktop;

    if (desktop == NULL) {
        return;
    }
    struct leme_xdg_decoration *xdg;
    struct leme_xdg_decoration *xdg_next;

    wl_list_for_each_safe(xdg, xdg_next,
            &desktop->xdg_decorations, link) {
        leme_desktop_xdg_decoration_finish(xdg);
    }
    struct leme_server_decoration *server_decoration;
    struct leme_server_decoration *server_decoration_next;

    wl_list_for_each_safe(server_decoration, server_decoration_next,
            &desktop->server_decorations, link) {
        leme_desktop_server_decoration_finish(server_decoration);
    }
    if (desktop->request_activate.link.next != NULL) {
        wl_list_remove(&desktop->request_activate.link);
    }
    if (desktop->new_xdg_decoration.link.next != NULL) {
        wl_list_remove(&desktop->new_xdg_decoration.link);
    }
    if (desktop->new_server_decoration.link.next != NULL) {
        wl_list_remove(&desktop->new_server_decoration.link);
    }
    if (desktop->request_set_shape.link.next != NULL) {
        wl_list_remove(&desktop->request_set_shape.link);
    }
    free(desktop->cursor_override_name);
    desktop->cursor_override_name = NULL;
    if (desktop->xcursor != NULL) {
        wlr_xcursor_manager_destroy(desktop->xcursor);
        desktop->xcursor = NULL;
    }
    free(desktop);
    server->desktop = NULL;
    server->xdg_dialog_manager = NULL;
}
