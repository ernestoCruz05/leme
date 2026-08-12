#include "input/input.h"
#include "input/internal.h"

#include "core/server.h"

#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_seat.h>

void
leme_input_update_capabilities(struct leme_server *server)
{
    uint32_t capabilities = 0;

    if (!wl_list_empty(&server->keyboards)) {
        capabilities |= WL_SEAT_CAPABILITY_KEYBOARD;
    }
    if (!wl_list_empty(&server->pointers)) {
        capabilities |= WL_SEAT_CAPABILITY_POINTER;
    }
    wlr_seat_set_capabilities(server->seat, capabilities);
}

static void
leme_input_handle_new(struct wl_listener *listener, void *data)
{
    struct leme_server *server =
        wl_container_of(listener, server, new_input);
    struct wlr_input_device *device = data;

    if (device->type == WLR_INPUT_DEVICE_KEYBOARD) {
        leme_input_keyboard_add(server, device);
    } else if (device->type == WLR_INPUT_DEVICE_POINTER) {
        leme_input_pointer_add(server, device);
    }
    leme_input_update_capabilities(server);
}

void
leme_input_init(struct leme_server *server)
{
    wl_list_init(&server->keyboards);
    wl_list_init(&server->pointers);
    server->cursor = wlr_cursor_create();
    if (server->cursor == NULL) {
        return;
    }
    wlr_cursor_attach_output_layout(server->cursor, server->output_layout);
    leme_input_pointer_events_init(server);

    server->new_input.notify = leme_input_handle_new;
    wl_signal_add(&server->backend->events.new_input, &server->new_input);
    leme_input_update_capabilities(server);
}

void
leme_input_finish(struct leme_server *server)
{
    if (server->new_input.link.next != NULL) {
        wl_list_remove(&server->new_input.link);
        server->new_input.link.next = NULL;
        server->new_input.link.prev = NULL;
    }
    if (server->cursor == NULL) {
        return;
    }
    leme_input_keyboards_finish(server);
    leme_input_pointers_finish(server);
    wlr_cursor_destroy(server->cursor);
    server->cursor = NULL;
}
