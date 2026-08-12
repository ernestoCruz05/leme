#include "shell/layer.h"

#include "protocols/input.h"
#include "shell/layer_layout.h"
#include "output/output.h"
#include "render/render.h"
#include "core/server.h"
#include "protocols/session.h"
#include "workspace/tag.h"
#include "shell/view.h"

#include <stdlib.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

static const char *
leme_layer_namespace(const struct wlr_layer_surface_v1 *surface)
{
    return surface->namespace == NULL ? "(unnamed)" : surface->namespace;
}

bool
leme_layer_keyboard_is_exclusive(const struct leme_server *server)
{
    return server->focused_layer != NULL &&
        server->focused_layer->mapped &&
        server->focused_layer->output != NULL &&
        server->focused_layer->output->wlr_output->enabled &&
        server->focused_layer->wlr_layer_surface->current.keyboard_interactive ==
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE;
}

static void
leme_layer_focus(struct leme_layer_surface *layer)
{
    struct leme_server *server;
    struct wlr_keyboard *keyboard;

    if (layer == NULL || !layer->mapped || layer->output == NULL ||
            !layer->output->wlr_output->enabled ||
            layer->wlr_layer_surface->current.keyboard_interactive ==
            ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
        return;
    }
    server = layer->server;
    if (leme_session_locked(server)) {
        return;
    }
    if (server->focused_layer == layer) {
        return;
    }
    leme_view_clear_focus(server);
    server->focused_layer = layer;
    wlr_log(WLR_INFO, "leme: focused layer %s",
        leme_layer_namespace(layer->wlr_layer_surface));
    keyboard = wlr_seat_get_keyboard(server->seat);
    if (keyboard != NULL) {
        wlr_seat_keyboard_notify_enter(server->seat,
            layer->wlr_layer_surface->surface, keyboard->keycodes,
            keyboard->num_keycodes, &keyboard->modifiers);
    }
    leme_input_protocols_update_keyboard_focus(server);
}

void
leme_layer_focus_on_button(struct leme_layer_surface *layer)
{
    if (layer != NULL &&
            layer->wlr_layer_surface->current.keyboard_interactive !=
            ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
        leme_layer_focus(layer);
    }
}

void
leme_layer_release_for_view(struct leme_server *server)
{
    if (!leme_layer_keyboard_is_exclusive(server)) {
        server->focused_layer = NULL;
    }
}

void
leme_layer_restore_keyboard_focus(struct leme_server *server)
{
    static const uint32_t priorities[] = {
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
        ZWLR_LAYER_SHELL_V1_LAYER_TOP,
        ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM,
        ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
    };
    struct leme_layer_surface *layer;
    size_t index;

    if (leme_session_locked(server)) {
        return;
    }
    server->focused_layer = NULL;
    for (index = 0; index < LEME_ARRAY_LENGTH(priorities); index++) {
        wl_list_for_each(layer, &server->layer_surfaces, link) {
            if (layer->mapped && layer->output != NULL &&
                    layer->output->wlr_output->enabled &&
                    layer->wlr_layer_surface->current.layer == priorities[index] &&
                    layer->wlr_layer_surface->current.keyboard_interactive ==
                    ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE) {
                leme_layer_focus(layer);
                return;
            }
        }
    }
    if (leme_focused_tags(server) != NULL) {
        leme_tags_refresh_visibility(leme_focused_tags(server));
    } else {
        leme_view_clear_focus(server);
    }
    leme_input_protocols_update_keyboard_focus(server);
}

static void
leme_layer_handle_commit(struct wl_listener *listener, void *data)
{
    struct leme_layer_surface *layer =
        wl_container_of(listener, layer, commit);

    bool was_focused = layer->server->focused_layer == layer;

    (void)data;
    leme_render_layer_update_tree(layer);
    leme_session_refresh_idle_inhibitors(layer->server);
    leme_layer_arrange(layer->server);
    if (layer->mapped &&
            layer->wlr_layer_surface->current.keyboard_interactive ==
            ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE) {
        leme_layer_focus(layer);
    } else if (was_focused &&
            layer->wlr_layer_surface->current.keyboard_interactive ==
            ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
        leme_layer_restore_keyboard_focus(layer->server);
    }
}

static void
leme_layer_handle_map(struct wl_listener *listener, void *data)
{
    struct leme_layer_surface *layer = wl_container_of(listener, layer, map);

    (void)data;
    layer->mapped = true;
    leme_layer_arrange(layer->server);
    if (layer->wlr_layer_surface->current.keyboard_interactive ==
            ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE) {
        leme_layer_focus(layer);
    }
}

static void
leme_layer_handle_unmap(struct wl_listener *listener, void *data)
{
    struct leme_layer_surface *layer = wl_container_of(listener, layer, unmap);

    bool was_focused = layer->server->focused_layer == layer;

    (void)data;
    layer->mapped = false;
    if (was_focused) {
        leme_layer_restore_keyboard_focus(layer->server);
    }
    leme_layer_arrange(layer->server);
}

static struct leme_box
leme_layer_full_box(const struct leme_layer_surface *layer)
{
    return leme_output_full_box(layer->output);
}

static void
leme_layer_popup_handle_commit(struct wl_listener *listener, void *data)
{
    struct leme_layer_popup *popup =
        wl_container_of(listener, popup, commit);

    (void)data;
    leme_session_refresh_idle_inhibitors(popup->layer->server);
    leme_render_layer_popup_unconstrain(
        popup, leme_layer_full_box(popup->layer));
    leme_render_layer_popup_update(popup);
}

static void
leme_layer_popup_handle_destroy(struct wl_listener *listener, void *data)
{
    struct leme_layer_popup *popup =
        wl_container_of(listener, popup, destroy);

    (void)data;
    leme_render_layer_popup_destroy(popup);
    wl_list_remove(&popup->commit.link);
    wl_list_remove(&popup->destroy.link);
    wl_list_remove(&popup->link);
    free(popup);
}

static bool
leme_layer_popup_create(struct leme_layer_surface *layer,
    struct wlr_xdg_popup *wlr_popup)
{
    struct leme_layer_popup *popup = calloc(1, sizeof(*popup));

    if (popup == NULL) {
        return false;
    }
    popup->layer = layer;
    popup->wlr_popup = wlr_popup;
    popup->commit.notify = leme_layer_popup_handle_commit;
    wl_signal_add(&wlr_popup->base->surface->events.commit, &popup->commit);
    popup->destroy.notify = leme_layer_popup_handle_destroy;
    wl_signal_add(&wlr_popup->events.destroy, &popup->destroy);
    wl_list_insert(&layer->popups, &popup->link);
    if (!leme_render_layer_popup_create(popup)) {
        wlr_xdg_popup_destroy(wlr_popup);
        return false;
    }
    leme_render_layer_popup_unconstrain(popup, leme_layer_full_box(layer));
    leme_render_layer_popup_update(popup);
    return true;
}

static void
leme_layer_handle_new_popup(struct wl_listener *listener, void *data)
{
    struct leme_layer_surface *layer =
        wl_container_of(listener, layer, new_popup);
    struct wlr_xdg_popup *popup = data;

    if (!leme_layer_popup_create(layer, popup)) {
        wlr_log(WLR_ERROR, "leme: failed to create popup for layer %s",
            leme_layer_namespace(layer->wlr_layer_surface));
    }
}

static void
leme_layer_handle_destroy(struct wl_listener *listener, void *data)
{
    struct leme_layer_surface *layer =
        wl_container_of(listener, layer, destroy);
    struct leme_server *server = layer->server;
    struct leme_layer_popup *popup;
    struct leme_layer_popup *temporary;
    bool was_focused = server->focused_layer == layer;

    (void)data;
    layer->wlr_layer_surface->data = NULL;
    wl_list_for_each_safe(popup, temporary, &layer->popups, link) {
        leme_render_layer_popup_destroy(popup);
        wl_list_remove(&popup->commit.link);
        wl_list_remove(&popup->destroy.link);
        wl_list_remove(&popup->link);
        free(popup);
    }
    leme_render_layer_destroy(layer);
    wl_list_remove(&layer->commit.link);
    wl_list_remove(&layer->map.link);
    wl_list_remove(&layer->unmap.link);
    wl_list_remove(&layer->new_popup.link);
    wl_list_remove(&layer->destroy.link);
    wl_list_remove(&layer->link);
    free(layer);
    leme_layer_arrange(server);
    if (was_focused) {
        leme_layer_restore_keyboard_focus(server);
    }
}

static void
leme_layer_handle_new_surface(struct wl_listener *listener, void *data)
{
    struct leme_server *server =
        wl_container_of(listener, server, new_layer_surface);
    struct wlr_layer_surface_v1 *wlr_layer_surface = data;
    struct leme_output *output = leme_output_focused(server);
    struct leme_layer_surface *layer;

    if (wlr_layer_surface->output != NULL) {
        output = leme_output_from_wlr_output(
            server, wlr_layer_surface->output);
    }
    if (output == NULL || !output->wlr_output->enabled) {
        wlr_log(WLR_ERROR, "leme: rejecting layer surface %s without output",
            leme_layer_namespace(wlr_layer_surface));
        wlr_layer_surface_v1_destroy(wlr_layer_surface);
        return;
    }
    wlr_layer_surface->output = output->wlr_output;

    layer = calloc(1, sizeof(*layer));
    if (layer == NULL) {
        wlr_log(WLR_ERROR, "leme: failed to allocate layer surface %s",
            leme_layer_namespace(wlr_layer_surface));
        wlr_layer_surface_v1_destroy(wlr_layer_surface);
        return;
    }
    layer->server = server;
    layer->output = output;
    layer->wlr_layer_surface = wlr_layer_surface;
    wl_list_init(&layer->popups);
    layer->commit.notify = leme_layer_handle_commit;
    wl_signal_add(&wlr_layer_surface->surface->events.commit, &layer->commit);
    layer->map.notify = leme_layer_handle_map;
    wl_signal_add(&wlr_layer_surface->surface->events.map, &layer->map);
    layer->unmap.notify = leme_layer_handle_unmap;
    wl_signal_add(&wlr_layer_surface->surface->events.unmap, &layer->unmap);
    layer->new_popup.notify = leme_layer_handle_new_popup;
    wl_signal_add(&wlr_layer_surface->events.new_popup, &layer->new_popup);
    layer->destroy.notify = leme_layer_handle_destroy;
    wl_signal_add(&wlr_layer_surface->events.destroy, &layer->destroy);
    wl_list_insert(&server->layer_surfaces, &layer->link);
    wlr_layer_surface->data = layer;

    if (!leme_render_layer_create(layer)) {
        wlr_log(WLR_ERROR, "leme: failed to render layer surface %s",
            leme_layer_namespace(wlr_layer_surface));
        wlr_layer_surface_v1_destroy(wlr_layer_surface);
    }
}

bool
leme_layer_init(struct leme_server *server)
{
    wl_list_init(&server->layer_surfaces);
    server->layer_shell = wlr_layer_shell_v1_create(server->display, 5);
    if (server->layer_shell == NULL) {
        return false;
    }
    server->new_layer_surface.notify = leme_layer_handle_new_surface;
    wl_signal_add(&server->layer_shell->events.new_surface,
        &server->new_layer_surface);
    return true;
}

void
leme_layer_finish(struct leme_server *server)
{
    struct leme_layer_surface *layer;
    struct leme_layer_surface *temporary;

    if (server->new_layer_surface.link.next != NULL) {
        wl_list_remove(&server->new_layer_surface.link);
        server->new_layer_surface.link.next = NULL;
        server->new_layer_surface.link.prev = NULL;
    }
    wl_list_for_each_safe(layer, temporary, &server->layer_surfaces, link) {
        wlr_layer_surface_v1_destroy(layer->wlr_layer_surface);
    }
    server->layer_shell = NULL;
    server->focused_layer = NULL;
}

void
leme_layer_handle_output_destroy(struct leme_server *server,
    struct wlr_output *output)
{
    struct leme_layer_surface *layer;
    struct leme_layer_surface *temporary;

    wl_list_for_each_safe(layer, temporary, &server->layer_surfaces, link) {
        if (layer->wlr_layer_surface->output == output) {
            wlr_layer_surface_v1_destroy(layer->wlr_layer_surface);
        }
    }
}
