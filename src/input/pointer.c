#include "input/input.h"
#include "input/internal.h"

#include "config/config.h"
#include "core/server.h"
#include "output/output.h"
#include "protocols/data.h"
#include "protocols/desktop.h"
#include "protocols/input.h"
#include "protocols/session.h"
#include "render/render.h"
#include "shell/layer.h"
#include "shell/policy.h"
#include "shell/view.h"
#include "workspace/tag.h"

#include <libinput.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/backend/libinput.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>

struct leme_pointer {
    struct leme_server *server;
    struct wlr_input_device *device;
    struct wl_listener destroy;
    struct wl_list link;
};

enum leme_pointer_grab_mode {
    LEME_POINTER_GRAB_NONE,
    LEME_POINTER_GRAB_FLOAT_MOVE,
    LEME_POINTER_GRAB_FLOAT_RESIZE,
    LEME_POINTER_GRAB_TILED_MOVE,
    LEME_POINTER_GRAB_TILED_RESIZE,
    LEME_POINTER_GRAB_SINK,
};

struct leme_pointer_grab {
    struct leme_server *server;
    struct wlr_seat_pointer_grab seat_grab;
    enum leme_pointer_grab_mode mode;
    struct leme_view *view;
    uint32_t button;
    double cursor_x;
    double cursor_y;
    struct leme_box saved_box;
    enum leme_grab_edge edges;
    struct leme_layout_detach *detach;
    struct leme_layout_resize_drag resize_drag;
    struct leme_view *drop_target;
    enum leme_direction drop_direction;
    enum leme_drop_mode drop_mode;
    bool preview_failed;
    struct leme_output *destination;
    bool drop_empty;
};

static void leme_input_process_motion(struct leme_server *server,
    uint32_t time_msec, struct leme_render_hit *result,
    bool allow_view_focus);

static bool
leme_input_pointer_button_held(
    const struct wlr_seat *seat, uint32_t button)
{
    size_t index;

    for (index = 0; index < seat->pointer_state.button_count; index++) {
        if (seat->pointer_state.buttons[index].button == button &&
                seat->pointer_state.buttons[index].n_pressed > 0) {
            return true;
        }
    }
    return false;
}

static size_t
leme_input_pointer_button_press_count(const struct wlr_seat *seat)
{
    size_t count = 0;
    size_t index;

    for (index = 0; index < seat->pointer_state.button_count; index++) {
        count += seat->pointer_state.buttons[index].n_pressed;
    }
    return count;
}

static int
leme_input_pointer_round_delta(double value)
{
    if (value >= (double)INT_MAX) {
        return INT_MAX;
    }
    if (value <= (double)INT_MIN) {
        return INT_MIN;
    }
    return (int)llround(value);
}

static int
leme_input_pointer_add_saturated(int value, int delta)
{
    int64_t result = (int64_t)value + delta;

    if (result > INT_MAX) {
        return INT_MAX;
    }
    if (result < INT_MIN) {
        return INT_MIN;
    }
    return (int)result;
}

static bool
leme_input_pointer_edges(uint32_t wlr_edges, enum leme_grab_edge *edges)
{
    const uint32_t allowed = WLR_EDGE_TOP | WLR_EDGE_BOTTOM |
        WLR_EDGE_LEFT | WLR_EDGE_RIGHT;

    if (wlr_edges == WLR_EDGE_NONE || (wlr_edges & ~allowed) != 0 ||
            ((wlr_edges & WLR_EDGE_TOP) != 0 &&
                (wlr_edges & WLR_EDGE_BOTTOM) != 0) ||
            ((wlr_edges & WLR_EDGE_LEFT) != 0 &&
                (wlr_edges & WLR_EDGE_RIGHT) != 0)) {
        return false;
    }
    *edges = LEME_GRAB_EDGE_NONE;
    if ((wlr_edges & WLR_EDGE_TOP) != 0) {
        *edges |= LEME_GRAB_EDGE_TOP;
    }
    if ((wlr_edges & WLR_EDGE_BOTTOM) != 0) {
        *edges |= LEME_GRAB_EDGE_BOTTOM;
    }
    if ((wlr_edges & WLR_EDGE_LEFT) != 0) {
        *edges |= LEME_GRAB_EDGE_LEFT;
    }
    if ((wlr_edges & WLR_EDGE_RIGHT) != 0) {
        *edges |= LEME_GRAB_EDGE_RIGHT;
    }
    return true;
}

static const char *
leme_input_pointer_grab_cursor(enum leme_grab_edge edges)
{
    if (edges == LEME_GRAB_EDGE_NONE) {
        return "grabbing";
    }
    if ((edges & LEME_GRAB_EDGE_TOP) != 0) {
        if ((edges & LEME_GRAB_EDGE_LEFT) != 0) {
            return "nw-resize";
        }
        if ((edges & LEME_GRAB_EDGE_RIGHT) != 0) {
            return "ne-resize";
        }
        return "n-resize";
    }
    if ((edges & LEME_GRAB_EDGE_BOTTOM) != 0) {
        if ((edges & LEME_GRAB_EDGE_LEFT) != 0) {
            return "sw-resize";
        }
        if ((edges & LEME_GRAB_EDGE_RIGHT) != 0) {
            return "se-resize";
        }
        return "s-resize";
    }
    return (edges & LEME_GRAB_EDGE_LEFT) != 0 ?
        "w-resize" : "e-resize";
}

static bool
leme_input_pointer_grab_mode_tiled(enum leme_pointer_grab_mode mode)
{
    return mode == LEME_POINTER_GRAB_TILED_MOVE ||
        mode == LEME_POINTER_GRAB_TILED_RESIZE;
}

static void
leme_input_pointer_grab_hide_preview(struct leme_pointer_grab *grab)
{
    if (grab->drop_target != NULL) {
        leme_render_view_hide_drop_preview(grab->drop_target);
        grab->drop_target = NULL;
    }
}

static void
leme_input_pointer_grab_cleanup(struct leme_pointer_grab *grab)
{
    struct leme_server *server = grab->server;

    leme_input_pointer_grab_hide_preview(grab);
    if (grab->detach != NULL) {
        if (grab->view != NULL) {
            leme_view_restore_tiled_drag(grab->view, &grab->detach);
        } else {
            leme_layout_discard_detached(&grab->detach);
        }
    }
    grab->mode = LEME_POINTER_GRAB_NONE;
    /* O configure final tem de sair, senão o cliente fica na caixa antiga. */
    leme_view_set_configure_deferred(grab->view, false);
    grab->view = NULL;
    grab->button = 0;
    grab->edges = LEME_GRAB_EDGE_NONE;
    memset(&grab->resize_drag, 0, sizeof(grab->resize_drag));
    grab->drop_direction = LEME_DIRECTION_LEFT;
    grab->preview_failed = false;
    grab->destination = NULL;
    grab->drop_empty = false;
    leme_desktop_cursor_restore(server);
    if (server->desktop != NULL && server->seat != NULL &&
            server->cursor != NULL && !leme_session_locked(server)) {
        wlr_seat_pointer_clear_focus(server->seat);
        leme_input_process_motion(server, 0, NULL, false);
    }
}

static void
leme_input_pointer_grab_enter(struct wlr_seat_pointer_grab *seat_grab,
    struct wlr_surface *surface, double sx, double sy)
{
    (void)seat_grab;
    (void)surface;
    (void)sx;
    (void)sy;
}

static void
leme_input_pointer_grab_clear_focus(
    struct wlr_seat_pointer_grab *seat_grab)
{
    (void)seat_grab;
}

static bool
leme_input_pointer_grab_cross_allowed(const struct leme_server *server)
{
    return server->config == NULL ||
        server->config->output_policy.cross_output_drag;
}

static void
leme_input_pointer_grab_update_destination(struct leme_pointer_grab *grab)
{
    struct leme_server *server = grab->server;
    struct leme_output *resolved;

    if (!leme_input_pointer_grab_cross_allowed(server)) {
        grab->destination = leme_view_output(grab->view);
        return;
    }
    resolved = leme_output_at(server, server->cursor->x, server->cursor->y);
    if (resolved == NULL || !resolved->wlr_output->enabled) {
        return;
    }
    grab->destination = resolved;
}

static void
leme_input_pointer_grab_motion(struct wlr_seat_pointer_grab *seat_grab,
    uint32_t time_msec, double sx, double sy)
{
    struct leme_pointer_grab *grab = seat_grab->data;
    struct leme_box box;
    int dx;
    int dy;

    (void)time_msec;
    (void)sx;
    (void)sy;
    if (grab->view == NULL || grab->mode == LEME_POINTER_GRAB_SINK ||
            grab->mode == LEME_POINTER_GRAB_NONE) {
        return;
    }
    dx = leme_input_pointer_round_delta(
        grab->server->cursor->x - grab->cursor_x);
    dy = leme_input_pointer_round_delta(
        grab->server->cursor->y - grab->cursor_y);
    if (grab->mode == LEME_POINTER_GRAB_TILED_RESIZE) {
        if (leme_layout_resize_drag_update(&grab->resize_drag, dx, dy)) {
            leme_view_arrange(grab->server);
        }
        return;
    }
    if (grab->mode == LEME_POINTER_GRAB_FLOAT_MOVE ||
            grab->mode == LEME_POINTER_GRAB_TILED_MOVE) {
        leme_input_pointer_grab_update_destination(grab);
        box = grab->saved_box;
        box.x = leme_input_pointer_add_saturated(box.x, dx);
        box.y = leme_input_pointer_add_saturated(box.y, dy);
        if (grab->mode == LEME_POINTER_GRAB_FLOAT_MOVE &&
                !leme_input_pointer_grab_cross_allowed(grab->server) &&
                grab->destination != NULL) {
            box = leme_view_policy_clamp_box(box,
                leme_output_usable_box(grab->destination));
        }
    } else {
        box = leme_layout_resize_box(grab->saved_box, grab->edges, dx, dy);
    }
    if (!leme_view_apply_interactive_box(grab->view, box,
            grab->mode == LEME_POINTER_GRAB_FLOAT_RESIZE)) {
        leme_input_pointer_grab_cancel(grab->server);
        return;
    }
    if (grab->mode == LEME_POINTER_GRAB_TILED_MOVE) {
        struct leme_tags *tags = leme_output_tags(grab->destination);
        struct leme_view *target = leme_tags_pointer_drop_target(
            tags, grab->view,
            grab->server->cursor->x, grab->server->cursor->y);
        struct leme_layout_drop drop;

        grab->drop_empty = target == NULL && tags != NULL &&
            grab->destination != leme_view_output(grab->view);
        if (target != grab->drop_target) {
            leme_input_pointer_grab_hide_preview(grab);
        }
        if (target == NULL || !leme_layout_drop_at(grab->drop_mode,
                leme_render_view_content_box(target, target->box),
                grab->server->cursor->x, grab->server->cursor->y,
                &drop)) {
            grab->drop_target = NULL;
            return;
        }
        grab->drop_target = target;
        grab->drop_direction = drop.direction;
        if (!leme_render_view_show_drop_preview(target, drop.box) &&
                !grab->preview_failed) {
            wlr_log(WLR_ERROR, "%s",
                "leme: failed to render tiled drop preview");
            grab->preview_failed = true;
        }
    }
}

static uint32_t
leme_input_pointer_grab_button(struct wlr_seat_pointer_grab *seat_grab,
    uint32_t time_msec, uint32_t button, enum wl_pointer_button_state state)
{
    struct leme_pointer_grab *grab = seat_grab->data;

    (void)time_msec;
    if (button == grab->button &&
            state == WL_POINTER_BUTTON_STATE_RELEASED &&
            !leme_input_pointer_button_held(grab->server->seat, button)) {
        struct leme_output *destination = grab->destination;
        bool crossing = destination != NULL && grab->view != NULL &&
            destination != leme_view_output(grab->view);

        if (grab->mode == LEME_POINTER_GRAB_TILED_MOVE &&
                grab->view != NULL && grab->detach != NULL) {
            struct leme_view *target = grab->drop_target;
            enum leme_direction direction = grab->drop_direction;

            leme_input_pointer_grab_hide_preview(grab);
            if (crossing && (target != NULL || grab->drop_empty)) {
                if (!leme_view_drop_to_output(grab->view, &grab->detach,
                        destination, target, direction)) {
                    leme_view_restore_tiled_drag(grab->view, &grab->detach);
                }
            } else if (target == NULL || !leme_view_commit_tiled_drag(
                    grab->view, &grab->detach, target, direction)) {
                leme_view_restore_tiled_drag(grab->view, &grab->detach);
            }
        } else {
            leme_input_pointer_grab_hide_preview(grab);
            if (crossing && grab->mode == LEME_POINTER_GRAB_FLOAT_MOVE) {
                if (leme_view_is_shown_scratchpad(grab->view)) {
                    (void)leme_scratchpad_move_shown(grab->view, destination);
                } else {
                    (void)leme_view_drop_to_output(grab->view, NULL,
                        destination, NULL, LEME_DIRECTION_LEFT);
                }
            }
        }
        wlr_seat_pointer_end_grab(grab->server->seat);
    }
    return 0;
}

static void
leme_input_pointer_grab_axis(struct wlr_seat_pointer_grab *seat_grab,
    uint32_t time_msec, enum wl_pointer_axis orientation, double value,
    int32_t value_discrete, enum wl_pointer_axis_source source,
    enum wl_pointer_axis_relative_direction relative_direction)
{
    (void)seat_grab;
    (void)time_msec;
    (void)orientation;
    (void)value;
    (void)value_discrete;
    (void)source;
    (void)relative_direction;
}

static void
leme_input_pointer_grab_frame(struct wlr_seat_pointer_grab *seat_grab)
{
    (void)seat_grab;
}

static void
leme_input_pointer_grab_cancelled(
    struct wlr_seat_pointer_grab *seat_grab)
{
    struct leme_pointer_grab *grab = seat_grab->data;

    leme_input_pointer_grab_cleanup(grab);
}

static const struct wlr_pointer_grab_interface leme_pointer_grab_interface = {
    .enter = leme_input_pointer_grab_enter,
    .clear_focus = leme_input_pointer_grab_clear_focus,
    .motion = leme_input_pointer_grab_motion,
    .button = leme_input_pointer_grab_button,
    .axis = leme_input_pointer_grab_axis,
    .frame = leme_input_pointer_grab_frame,
    .cancel = leme_input_pointer_grab_cancelled,
};

static bool
leme_input_pointer_grab_start(struct leme_view *view, bool resize,
    uint32_t button, uint32_t wlr_edges, bool compositor_owned)
{
    struct leme_server *server;
    struct leme_pointer_grab *grab;
    enum leme_grab_edge edges = LEME_GRAB_EDGE_NONE;
    const char *cursor_name;
    bool tiled;

    if (view == NULL || view->server == NULL) {
        return false;
    }
    server = view->server;
    grab = server->pointer_grab;
    tiled = !view->floating;
    if (grab == NULL || server->seat == NULL || server->cursor == NULL ||
            wlr_seat_pointer_has_grab(server->seat) ||
            leme_session_locked(server) || server->seat->drag != NULL ||
            !view->mapped || view->unmanaged || view->detached ||
            view->fullscreen || (leme_view_is_scratchpad(view) &&
                !leme_view_is_shown_scratchpad(view)) ||
            (tiled && view->tag == NULL) ||
            (!compositor_owned && tiled) ||
            (resize && !leme_input_pointer_edges(wlr_edges, &edges)) ||
            grab->detach != NULL) {
        return false;
    }
    if (tiled && resize) {
        if (!leme_layout_drag_supported(&view->tag->layout) ||
                !leme_layout_resize_drag_begin(
                    view->tag->layout.root, view, edges,
                    &grab->resize_drag)) {
            return false;
        }
        if (grab->resize_drag.horizontal == NULL) {
            edges = (enum leme_grab_edge)(edges &
                (LEME_GRAB_EDGE_TOP | LEME_GRAB_EDGE_BOTTOM));
        }
        if (grab->resize_drag.vertical == NULL) {
            edges = (enum leme_grab_edge)(edges &
                (LEME_GRAB_EDGE_LEFT | LEME_GRAB_EDGE_RIGHT));
        }
    }
    cursor_name = tiled && !resize ? "grabbing" :
        leme_input_pointer_grab_cursor(edges);
    if (!leme_desktop_cursor_override(server, cursor_name)) {
        memset(&grab->resize_drag, 0, sizeof(grab->resize_drag));
        return false;
    }
    if (tiled && !resize &&
            !leme_view_begin_tiled_drag(view, &grab->detach)) {
        leme_desktop_cursor_restore(server);
        return false;
    }
    if (tiled) {
        grab->mode = resize ? LEME_POINTER_GRAB_TILED_RESIZE :
            LEME_POINTER_GRAB_TILED_MOVE;
    } else {
        grab->mode = resize ? LEME_POINTER_GRAB_FLOAT_RESIZE :
            LEME_POINTER_GRAB_FLOAT_MOVE;
    }
    grab->view = view;
    grab->button = button;
    grab->cursor_x = server->cursor->x;
    grab->cursor_y = server->cursor->y;
    grab->saved_box = view->box;
    grab->edges = edges;
    grab->drop_target = NULL;
    grab->drop_direction = LEME_DIRECTION_LEFT;
    grab->drop_mode = server->config == NULL ? LEME_DROP_MODE_SIMPLE :
        server->config->drop_mode;
    grab->preview_failed = false;
    grab->destination = leme_view_output(view);
    grab->drop_empty = false;
    grab->seat_grab.interface = &leme_pointer_grab_interface;
    grab->seat_grab.data = grab;
    /*
     * Um X11 refaz o desenho a cada ConfigureNotify. Durante o arrasto
     * acumula-se um só por frame; fora dele nada muda.
     */
    if (view->kind == LEME_VIEW_XWAYLAND) {
        leme_view_set_configure_deferred(view, true);
    }
    wlr_seat_pointer_start_grab(server->seat, &grab->seat_grab);
    leme_input_protocols_update_pointer_focus(server);
    return true;
}

bool
leme_input_pointer_grab_active(const struct leme_server *server)
{
    return server != NULL && server->pointer_grab != NULL &&
        server->seat != NULL &&
        server->seat->pointer_state.grab ==
            &server->pointer_grab->seat_grab;
}

bool
leme_input_pointer_grab_start_xdg(struct leme_view *view,
    bool resize, uint32_t serial, uint32_t wlr_edges)
{
    struct leme_server *server;
    uint32_t button;

    if (view == NULL || view->kind != LEME_VIEW_XDG) {
        return false;
    }
    server = view->server;
    button = server->seat->pointer_state.grab_button;
    if (leme_view_from_surface(server,
            server->seat->keyboard_state.focused_surface) != view ||
            leme_view_from_surface(server,
                server->seat->pointer_state.focused_surface) != view ||
            !leme_input_pointer_button_held(server->seat, button) ||
            !wlr_seat_validate_pointer_grab_serial(
                server->seat, NULL, serial)) {
        return false;
    }
    return leme_input_pointer_grab_start(
        view, resize, button, wlr_edges, false);
}

bool
leme_input_pointer_grab_start_xwayland(struct leme_view *view,
    bool resize, uint32_t wlr_edges)
{
    struct leme_server *server;
    uint32_t button;

    if (view == NULL || view->kind != LEME_VIEW_XWAYLAND) {
        return false;
    }
    server = view->server;
    if (server->focused_view != view ||
            leme_view_from_surface(server,
                server->seat->pointer_state.focused_surface) != view ||
            leme_input_pointer_button_press_count(server->seat) != 1) {
        return false;
    }
    button = server->seat->pointer_state.buttons[0].button;
    return leme_input_pointer_grab_start(
        view, resize, button, wlr_edges, false);
}

static void
leme_input_pointer_grab_sink(struct leme_pointer_grab *grab)
{
    grab->mode = LEME_POINTER_GRAB_SINK;
    leme_view_set_configure_deferred(grab->view, false);
    grab->view = NULL;
    memset(&grab->resize_drag, 0, sizeof(grab->resize_drag));
    leme_desktop_cursor_restore(grab->server);
    if (!leme_input_pointer_button_held(
            grab->server->seat, grab->button)) {
        wlr_seat_pointer_end_grab(grab->server->seat);
    }
}

void
leme_input_pointer_grab_cancel(struct leme_server *server)
{
    struct leme_pointer_grab *grab;

    if (!leme_input_pointer_grab_active(server)) {
        return;
    }
    grab = server->pointer_grab;
    leme_input_pointer_grab_hide_preview(grab);
    if (grab->mode == LEME_POINTER_GRAB_TILED_MOVE &&
            grab->view != NULL && grab->detach != NULL) {
        leme_view_restore_tiled_drag(grab->view, &grab->detach);
    }
    leme_input_pointer_grab_sink(grab);
}

void
leme_input_pointer_grab_cancel_tiled(struct leme_server *server)
{
    if (leme_input_pointer_grab_active(server) &&
            leme_input_pointer_grab_mode_tiled(
                server->pointer_grab->mode)) {
        leme_input_pointer_grab_cancel(server);
    }
}

void
leme_input_pointer_grab_cancel_view(struct leme_view *view)
{
    struct leme_pointer_grab *grab;

    if (view == NULL || !leme_input_pointer_grab_active(view->server)) {
        return;
    }
    grab = view->server->pointer_grab;
    if (grab->mode == LEME_POINTER_GRAB_TILED_MOVE &&
            grab->view == view) {
        leme_input_pointer_grab_hide_preview(grab);
        leme_view_discard_tiled_drag(view, &grab->detach);
        leme_input_pointer_grab_sink(grab);
        return;
    }
    if (leme_input_pointer_grab_mode_tiled(grab->mode)) {
        if (grab->view == view || (grab->view != NULL &&
                view->tag != NULL && view->tag == grab->view->tag &&
                !view->floating && !view->unmanaged)) {
            leme_input_pointer_grab_cancel(view->server);
        }
        return;
    }
    if (grab->view == view) {
        leme_input_pointer_grab_cancel(view->server);
    }
}

void
leme_input_pointer_grab_finish(struct leme_server *server)
{
    if (leme_input_pointer_grab_active(server)) {
        wlr_seat_pointer_end_grab(server->seat);
    }
}

static struct leme_pointer_settings
leme_input_pointer_settings(const struct leme_config *config,
    const char *device_name)
{
    struct leme_pointer_settings settings = config->pointer_defaults;
    size_t index;

    for (index = 0; device_name != NULL &&
            index < config->pointer_rule_count; index++) {
        const struct leme_pointer_rule *rule = &config->pointer_rules[index];
        const struct leme_pointer_settings *override = &rule->settings;

        if (strcmp(rule->name, device_name) != 0) {
            continue;
        }
        if ((override->fields & LEME_POINTER_PROFILE) != 0) {
            settings.profile = override->profile;
        }
        if ((override->fields & LEME_POINTER_SPEED) != 0) {
            settings.speed = override->speed;
        }
        if ((override->fields & LEME_POINTER_NATURAL_SCROLL) != 0) {
            settings.natural_scroll = override->natural_scroll;
        }
        if ((override->fields & LEME_POINTER_LEFT_HANDED) != 0) {
            settings.left_handed = override->left_handed;
        }
        if ((override->fields & LEME_POINTER_TAP) != 0) {
            settings.tap = override->tap;
        }
        break;
    }
    return settings;
}

static void
leme_input_pointer_status(const struct wlr_input_device *device,
    const char *property, enum libinput_config_status status)
{
    if (status != LIBINPUT_CONFIG_STATUS_SUCCESS) {
        wlr_log(WLR_ERROR, "leme: pointer %s failed to set %s: %s",
            device->name == NULL ? "(unnamed)" : device->name,
            property, libinput_config_status_to_str(status));
    }
}

static void
leme_input_configure_pointer(struct wlr_input_device *device,
    const struct leme_config *config)
{
    struct leme_pointer_settings settings;
    struct libinput_device *libinput_device;
    enum libinput_config_accel_profile profile;
    uint32_t profiles;

    if (config == NULL || !wlr_input_device_is_libinput(device)) {
        return;
    }
    libinput_device = wlr_libinput_get_device_handle(device);
    if (libinput_device == NULL) {
        return;
    }
    settings = leme_input_pointer_settings(config, device->name);
    profile = settings.profile == LEME_POINTER_ACCEL_FLAT ?
        LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT :
        LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE;
    profiles = libinput_device_config_accel_get_profiles(libinput_device);
    if ((profiles & profile) != 0) {
        leme_input_pointer_status(device, "accel_profile",
            libinput_device_config_accel_set_profile(
                libinput_device, profile));
    }
    if (libinput_device_config_accel_is_available(libinput_device)) {
        leme_input_pointer_status(device, "accel_speed",
            libinput_device_config_accel_set_speed(
                libinput_device, settings.speed));
    }
    if (libinput_device_config_scroll_has_natural_scroll(libinput_device)) {
        leme_input_pointer_status(device, "natural_scroll",
            libinput_device_config_scroll_set_natural_scroll_enabled(
                libinput_device, settings.natural_scroll));
    }
    if (libinput_device_config_left_handed_is_available(libinput_device)) {
        leme_input_pointer_status(device, "left_handed",
            libinput_device_config_left_handed_set(
                libinput_device, settings.left_handed));
    }
    if (libinput_device_config_tap_get_finger_count(libinput_device) > 0) {
        leme_input_pointer_status(device, "tap",
            libinput_device_config_tap_set_enabled(libinput_device,
                settings.tap ? LIBINPUT_CONFIG_TAP_ENABLED :
                LIBINPUT_CONFIG_TAP_DISABLED));
    }
    wlr_log(WLR_INFO, "leme: configured pointer %s",
        device->name == NULL ? "(unnamed)" : device->name);
}

void
leme_input_apply_pointer_config(struct leme_server *server,
    const struct leme_config *config)
{
    struct leme_pointer *pointer;

    if (server == NULL || config == NULL || server->cursor == NULL) {
        return;
    }
    wl_list_for_each(pointer, &server->pointers, link) {
        leme_input_configure_pointer(pointer->device, config);
    }
}

static void
leme_input_handle_pointer_destroy(struct wl_listener *listener, void *data)
{
    struct leme_pointer *pointer =
        wl_container_of(listener, pointer, destroy);
    struct leme_server *server = pointer->server;

    (void)data;
    wlr_cursor_detach_input_device(server->cursor, pointer->device);
    wl_list_remove(&pointer->destroy.link);
    wl_list_remove(&pointer->link);
    free(pointer);
    leme_input_update_capabilities(server);
}

void
leme_input_pointer_add(struct leme_server *server,
    struct wlr_input_device *device)
{
    struct leme_pointer *pointer = calloc(1, sizeof(*pointer));

    if (pointer == NULL) {
        return;
    }
    pointer->server = server;
    pointer->device = device;
    leme_input_configure_pointer(device, server->config);
    pointer->destroy.notify = leme_input_handle_pointer_destroy;
    wl_signal_add(&device->events.destroy, &pointer->destroy);
    wl_list_insert(&server->pointers, &pointer->link);
    wlr_cursor_attach_input_device(server->cursor, device);
}

static void
leme_input_follow_pointer_output(struct leme_server *server)
{
    struct leme_output *output = leme_output_at(
        server, server->cursor->x, server->cursor->y);

    if (output == NULL || output == leme_output_focused(server)) {
        return;
    }
    leme_output_set_focused(server, output, false);
}

static void
leme_input_process_motion(struct leme_server *server, uint32_t time_msec,
    struct leme_render_hit *result, bool allow_view_focus)
{
    struct leme_render_hit local;
    struct leme_render_hit *hit = result == NULL ? &local : result;

    leme_render_at(server, server->cursor->x, server->cursor->y, hit);
    if (!leme_session_surface_allowed(server, hit->surface)) {
        *hit = (struct leme_render_hit){0};
    }
    if (allow_view_focus && !leme_session_locked(server)) {
        leme_input_follow_pointer_output(server);
    }
    if (allow_view_focus && !leme_session_locked(server) &&
            leme_view_policy_hover_focus(hit->view) &&
            leme_tags_layout_kind(leme_focused_tags(server)) !=
                LEME_LAYOUT_ACCORDION) {
        leme_view_focus(hit->view);
    }
    if (hit->surface == NULL) {
        wlr_seat_pointer_notify_clear_focus(server->seat);
        leme_input_protocols_update_pointer_focus(server);
        return;
    }
    wlr_seat_pointer_notify_enter(server->seat,
        hit->surface, hit->surface_x, hit->surface_y);
    wlr_seat_pointer_notify_motion(server->seat,
        time_msec, hit->surface_x, hit->surface_y);
    leme_input_protocols_update_pointer_focus(server);
}

void
leme_input_refresh_pointer_focus(struct leme_server *server,
    uint32_t time_msec)
{
    wlr_seat_pointer_notify_clear_focus(server->seat);
    leme_input_process_motion(server, time_msec, NULL, true);
}

static void
leme_input_handle_motion(struct wl_listener *listener, void *data)
{
    struct leme_server *server =
        wl_container_of(listener, server, cursor_motion);
    struct wlr_pointer_motion_event *event = data;

    double dx = event->delta_x;
    double dy = event->delta_y;

    leme_session_notify_activity(server);
    if (leme_input_pointer_grab_active(server)) {
        wlr_cursor_move(server->cursor, &event->pointer->base, dx, dy);
        wlr_seat_pointer_notify_motion(server->seat,
            event->time_msec, server->cursor->x, server->cursor->y);
        return;
    }
    leme_input_protocols_send_relative(server, event->time_msec,
        dx, dy, event->unaccel_dx, event->unaccel_dy);
    if (leme_input_protocols_constrain_motion(server, &dx, &dy)) {
        return;
    }
    wlr_cursor_move(server->cursor, &event->pointer->base, dx, dy);
    leme_data_update_drag_icon(server);
    leme_input_process_motion(server, event->time_msec, NULL, true);
}

static void
leme_input_handle_motion_absolute(struct wl_listener *listener, void *data)
{
    struct leme_server *server =
        wl_container_of(listener, server, cursor_motion_absolute);
    struct wlr_pointer_motion_absolute_event *event = data;
    double layout_x;
    double layout_y;
    double dx;
    double dy;

    leme_session_notify_activity(server);
    wlr_cursor_absolute_to_layout_coords(server->cursor,
        &event->pointer->base, event->x, event->y, &layout_x, &layout_y);
    dx = layout_x - server->cursor->x;
    dy = layout_y - server->cursor->y;
    if (leme_input_pointer_grab_active(server)) {
        wlr_cursor_move(server->cursor, &event->pointer->base, dx, dy);
        wlr_seat_pointer_notify_motion(server->seat,
            event->time_msec, server->cursor->x, server->cursor->y);
        return;
    }
    if (leme_input_protocols_constrain_motion(server, &dx, &dy)) {
        return;
    }
    wlr_cursor_move(server->cursor, &event->pointer->base, dx, dy);
    leme_data_update_drag_icon(server);
    leme_input_process_motion(server, event->time_msec, NULL, true);
}

static uint32_t
leme_input_pointer_wlr_edges(enum leme_grab_edge edges)
{
    uint32_t result = WLR_EDGE_NONE;

    if ((edges & LEME_GRAB_EDGE_TOP) != 0) {
        result |= WLR_EDGE_TOP;
    }
    if ((edges & LEME_GRAB_EDGE_BOTTOM) != 0) {
        result |= WLR_EDGE_BOTTOM;
    }
    if ((edges & LEME_GRAB_EDGE_LEFT) != 0) {
        result |= WLR_EDGE_LEFT;
    }
    if ((edges & LEME_GRAB_EDGE_RIGHT) != 0) {
        result |= WLR_EDGE_RIGHT;
    }
    return result;
}

static bool
leme_input_pointer_try_compositor_grab(struct leme_server *server,
    const struct leme_render_hit *hit,
    const struct wlr_pointer_button_event *event, bool inhibited)
{
    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
    uint32_t modifiers;
    uint32_t edges = WLR_EDGE_NONE;

    if (event->state != WL_POINTER_BUTTON_STATE_PRESSED ||
            keyboard == NULL || inhibited || leme_session_locked(server) ||
            server->seat->drag != NULL || hit->view == NULL ||
            (event->button != BTN_LEFT && event->button != BTN_RIGHT)) {
        return false;
    }
    modifiers = wlr_keyboard_get_modifiers(keyboard) &
        (WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL |
        WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO);
    if (modifiers != WLR_MODIFIER_LOGO) {
        return false;
    }
    if (event->button == BTN_RIGHT) {
        int surface_x = leme_input_pointer_round_delta(
            server->cursor->x - hit->view->box.x);
        int surface_y = leme_input_pointer_round_delta(
            server->cursor->y - hit->view->box.y);

        edges = leme_input_pointer_wlr_edges(leme_layout_resize_edges(
            hit->view->box, surface_x, surface_y));
    }
    return leme_input_pointer_grab_start(hit->view,
        event->button == BTN_RIGHT, event->button, edges, true);
}

static void
leme_input_handle_button(struct wl_listener *listener, void *data)
{
    struct leme_server *server =
        wl_container_of(listener, server, cursor_button);
    struct wlr_pointer_button_event *event = data;
    struct leme_render_hit hit;
    bool inhibited;

    leme_session_notify_activity(server);
    if (leme_input_pointer_grab_active(server)) {
        wlr_seat_pointer_notify_button(server->seat,
            event->time_msec, event->button, event->state);
        return;
    }
    inhibited = leme_input_protocols_shortcuts_inhibited(server);
    leme_input_process_motion(server, event->time_msec, &hit, true);
    if (leme_input_pointer_try_compositor_grab(
            server, &hit, event, inhibited)) {
        wlr_seat_pointer_notify_button(server->seat,
            event->time_msec, event->button, event->state);
        return;
    }
    if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
        leme_layer_focus_on_button(hit.layer);
    }
    wlr_seat_pointer_notify_button(server->seat,
        event->time_msec, event->button, event->state);
}

static void
leme_input_handle_axis(struct wl_listener *listener, void *data)
{
    struct leme_server *server =
        wl_container_of(listener, server, cursor_axis);
    struct wlr_pointer_axis_event *event = data;

    leme_session_notify_activity(server);
    wlr_seat_pointer_notify_axis(server->seat, event->time_msec,
        event->orientation, event->delta, event->delta_discrete,
        event->source, event->relative_direction);
}

static void
leme_input_handle_frame(struct wl_listener *listener, void *data)
{
    struct leme_server *server =
        wl_container_of(listener, server, cursor_frame);

    (void)data;
    wlr_seat_pointer_notify_frame(server->seat);
}

static void
leme_input_handle_set_cursor(struct wl_listener *listener, void *data)
{
    struct leme_server *server =
        wl_container_of(listener, server, request_set_cursor);
    struct wlr_seat_pointer_request_set_cursor_event *event = data;

    if (!leme_input_pointer_grab_active(server) &&
            !leme_input_protocols_pointer_locked(server) &&
            leme_session_surface_allowed(server,
                server->seat->pointer_state.focused_surface) &&
            server->seat->pointer_state.focused_client == event->seat_client) {
        leme_desktop_cursor_surface_set(server);
        wlr_cursor_set_surface(server->cursor, event->surface,
            event->hotspot_x, event->hotspot_y);
    }
}


void
leme_input_pointer_events_init(struct leme_server *server)
{
    struct leme_pointer_grab *grab = calloc(1, sizeof(*grab));

    if (grab == NULL) {
        wlr_log(WLR_ERROR, "%s", "leme: failed to allocate pointer grab");
    } else {
        grab->server = server;
        grab->seat_grab.interface = &leme_pointer_grab_interface;
        grab->seat_grab.data = grab;
        server->pointer_grab = grab;
    }
    server->cursor_motion.notify = leme_input_handle_motion;
    wl_signal_add(&server->cursor->events.motion, &server->cursor_motion);
    server->cursor_motion_absolute.notify = leme_input_handle_motion_absolute;
    wl_signal_add(&server->cursor->events.motion_absolute,
        &server->cursor_motion_absolute);
    server->cursor_button.notify = leme_input_handle_button;
    wl_signal_add(&server->cursor->events.button, &server->cursor_button);
    server->cursor_axis.notify = leme_input_handle_axis;
    wl_signal_add(&server->cursor->events.axis, &server->cursor_axis);
    server->cursor_frame.notify = leme_input_handle_frame;
    wl_signal_add(&server->cursor->events.frame, &server->cursor_frame);
    server->request_set_cursor.notify = leme_input_handle_set_cursor;
    wl_signal_add(&server->seat->events.request_set_cursor,
        &server->request_set_cursor);
}

void
leme_input_pointers_finish(struct leme_server *server)
{
    struct leme_pointer *pointer;
    struct leme_pointer *temporary;

    leme_input_pointer_grab_finish(server);
    free(server->pointer_grab);
    server->pointer_grab = NULL;
    wl_list_for_each_safe(pointer, temporary, &server->pointers, link) {
        wlr_cursor_detach_input_device(server->cursor, pointer->device);
        wl_list_remove(&pointer->destroy.link);
        wl_list_remove(&pointer->link);
        free(pointer);
    }
    wl_list_remove(&server->cursor_motion.link);
    wl_list_remove(&server->cursor_motion_absolute.link);
    wl_list_remove(&server->cursor_button.link);
    wl_list_remove(&server->cursor_axis.link);
    wl_list_remove(&server->cursor_frame.link);
    wl_list_remove(&server->request_set_cursor.link);
}
