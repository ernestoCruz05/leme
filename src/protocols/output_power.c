#include "protocols/output_power.h"

#include "core/server.h"
#include "output/output.h"

#include <wayland-server-core.h>
#include <wlr/types/wlr_output_power_management_v1.h>

static void
leme_output_power_handle_set_mode(struct wl_listener *listener, void *data)
{
    struct leme_server *server =
        wl_container_of(listener, server, output_power_set_mode);
    const struct wlr_output_power_v1_set_mode_event *event = data;
    struct leme_output *output = leme_output_from_wlr_output(
        server, event->output);
    bool on;

    if (output == NULL) {
        return;
    }
    switch (event->mode) {
    case ZWLR_OUTPUT_POWER_V1_MODE_ON:
        on = true;
        break;
    case ZWLR_OUTPUT_POWER_V1_MODE_OFF:
        on = false;
        break;
    default:
        return;
    }
    (void)leme_output_set_power(output, on);
}

bool
leme_output_power_init(struct leme_server *server)
{
    server->output_power_manager =
        wlr_output_power_manager_v1_create(server->display);
    if (server->output_power_manager == NULL) {
        return false;
    }
    server->output_power_set_mode.notify =
        leme_output_power_handle_set_mode;
    wl_signal_add(&server->output_power_manager->events.set_mode,
        &server->output_power_set_mode);
    return true;
}

void
leme_output_power_finish(struct leme_server *server)
{
    if (server->output_power_set_mode.link.next != NULL) {
        wl_list_remove(&server->output_power_set_mode.link);
        server->output_power_set_mode.link.next = NULL;
        server->output_power_set_mode.link.prev = NULL;
    }
    server->output_power_manager = NULL;
}
