#include "core/server.h"

#include "protocols/capture.h"
#include "config/config.h"
#include "protocols/data.h"
#include "protocols/desktop.h"
#include "ipc/ipc.h"
#include "protocols/publication.h"
#include "render/graphics.h"
#include "shell/layer.h"
#include "output/output.h"
#include "protocols/output_power.h"
#include "render/render.h"
#include "protocols/session.h"
#include "core/session_environment.h"
#include "workspace/tag.h"
#include "protocols/tearing.h"
#include "input/input.h"
#include "protocols/input.h"
#include "shell/view.h"
#include "shell/xwayland.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server-protocol.h>
#include <wlr/backend/session.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/util/log.h>

static void
leme_server_handle_session_active(
    struct wl_listener *listener, void *data)
{
    struct leme_server *server =
        wl_container_of(listener, server, session_active);

    (void)data;
    if (server->session != NULL) {
        leme_render_handle_session_active(
            server, server->session->active);
    }
}

static int
leme_server_handle_sigint(int signal_number, void *data)
{
    struct leme_server *server = data;

    wlr_log(WLR_INFO, "leme: received signal %d", signal_number);
    wl_display_terminate(server->display);
    return 0;
}

static void
leme_server_report_diagnostics(const struct leme_server *server)
{
    const struct leme_config *config = server->config;
    size_t index;

    if (config == NULL || config->diagnostics.count == 0) {
        return;
    }
    wlr_log(WLR_ERROR, "leme: %zu configuration problems in %s",
        config->diagnostics.count,
        config->path == NULL ? "the configuration" : config->path);
    for (index = 0; index < config->diagnostics.count; index++) {
        const struct leme_diagnostic *entry = &config->diagnostics.entries[index];

        wlr_log(WLR_ERROR, "leme: %s:%d: %s",
            config->path == NULL ? "config" : config->path,
            entry->line, entry->message);
    }
    if (config->diagnostics.truncated) {
        wlr_log(WLR_ERROR, "%s",
            "leme: further configuration problems were not recorded");
    }
}

static bool
leme_server_load_config(struct leme_server *server)
{
    struct leme_config *defaults = leme_config_defaults();
    struct leme_config *loaded = NULL;
    const char *path = leme_config_path();
    char *error = NULL;

    if (defaults == NULL) {
        wlr_log(WLR_ERROR, "%s", "leme: failed to allocate safe defaults");
        return false;
    }
    if (path != NULL && access(path, R_OK) == 0) {
        loaded = leme_config_load(path, &error);
        if (loaded != NULL && leme_config_apply(server, loaded, &error)) {
            wlr_log(WLR_INFO, "leme: loaded %s", path);
            leme_server_report_diagnostics(server);
            leme_config_destroy(defaults);
            free(error);
            return true;
        }
        wlr_log(WLR_ERROR, "leme: initial config failed: %s",
            error == NULL ? "unknown error" : error);
        leme_config_destroy(loaded);
        free(error);
        error = NULL;
    } else {
        wlr_log(WLR_INFO, "%s", "leme: using safe configuration defaults");
    }
    if (!leme_config_apply(server, defaults, &error)) {
        wlr_log(WLR_ERROR, "leme: safe defaults failed: %s",
            error == NULL ? "unknown error" : error);
        leme_config_destroy(defaults);
        free(error);
        return false;
    }
    free(error);
    return true;
}

bool
leme_server_init(struct leme_server *server)
{
    memset(server, 0, sizeof(*server));
    wl_list_init(&server->new_output.link);
    wl_list_init(&server->session_active.link);
    wl_list_init(&server->layer_surfaces);
    leme_animation_manager_init(&server->animations);

    server->display = wl_display_create();
    if (server->display == NULL) {
        wlr_log(WLR_ERROR, "%s", "leme: failed to create Wayland display");
        return false;
    }

    server->backend = wlr_backend_autocreate(
        wl_display_get_event_loop(server->display), &server->session);
    if (server->backend == NULL) {
        wlr_log(WLR_ERROR, "%s", "leme: failed to create backend");
        return false;
    }
    if (server->session != NULL) {
        server->session_active.notify = leme_server_handle_session_active;
        wl_signal_add(&server->session->events.active,
            &server->session_active);
    }

    server->renderer = wlr_renderer_autocreate(server->backend);
    if (server->renderer == NULL ||
            !wlr_renderer_init_wl_shm(server->renderer, server->display)) {
        wlr_log(WLR_ERROR, "%s", "leme: failed to create renderer");
        return false;
    }

    server->allocator = wlr_allocator_autocreate(
        server->backend, server->renderer);
    if (server->allocator == NULL) {
        wlr_log(WLR_ERROR, "%s", "leme: failed to create allocator");
        return false;
    }

    server->compositor =
        wlr_compositor_create(server->display, 5, server->renderer);
    if (server->compositor == NULL ||
            wlr_subcompositor_create(server->display) == NULL) {
        wlr_log(WLR_ERROR, "%s", "leme: failed to create Wayland globals");
        return false;
    }
    server->viewporter = wlr_viewporter_create(server->display);
    server->fractional_scale_manager =
        wlr_fractional_scale_manager_v1_create(server->display, 1);
    if (server->viewporter == NULL || server->fractional_scale_manager == NULL) {
        wlr_log(WLR_ERROR, "%s", "leme: failed to create surface globals");
        return false;
    }

    server->output_layout = wlr_output_layout_create(server->display);
    if (server->output_layout == NULL) {
        wlr_log(WLR_ERROR, "%s", "leme: failed to create output layout");
        return false;
    }

    leme_render_init(server);
    if (server->scene == NULL || server->scene_output_layout == NULL) {
        wlr_log(WLR_ERROR, "%s", "leme: failed to create scene");
        return false;
    }
    if (!leme_graphics_init(server)) {
        return false;
    }
    if (!leme_capture_init(server) || !leme_tearing_init(server)) {
        return false;
    }

    server->seat = wlr_seat_create(server->display, "leme-seat");
    if (server->seat == NULL) {
        wlr_log(WLR_ERROR, "%s", "leme: failed to create seat");
        return false;
    }
    if (!leme_data_init(server)) {
        return false;
    }
    if (!leme_session_init(server)) {
        return false;
    }

    if (!leme_server_load_config(server)) {
        return false;
    }

    leme_input_init(server);
    if (server->cursor == NULL) {
        wlr_log(WLR_ERROR, "%s", "leme: failed to create cursor");
        return false;
    }
    if (!leme_input_protocols_init(server)) {
        return false;
    }
    if (!leme_scratchpad_init(server)) {
        return false;
    }
    leme_xwayland_init(server);
    leme_view_init(server);
    if (server->xdg_shell == NULL) {
        wlr_log(WLR_ERROR, "%s", "leme: failed to create xdg shell");
        return false;
    }
    if (!leme_layer_init(server)) {
        wlr_log(WLR_ERROR, "%s", "leme: failed to create layer shell");
        return false;
    }
    if (!leme_desktop_init(server)) {
        return false;
    }

    if (!leme_output_init(server)) {
        wlr_log(WLR_ERROR, "%s", "leme: failed to initialize output management");
        return false;
    }
    if (!leme_output_power_init(server)) {
        wlr_log(WLR_ERROR, "%s",
            "leme: failed to initialize output power management");
        return false;
    }

    if (!leme_publication_init(server)) {
        return false;
    }

    server->sigint_source = wl_event_loop_add_signal(
        wl_display_get_event_loop(server->display), SIGINT,
        leme_server_handle_sigint, server);
    if (server->sigint_source == NULL) {
        wlr_log(WLR_ERROR, "%s", "leme: failed to register SIGINT handler");
        return false;
    }

    server->socket = wl_display_add_socket_auto(server->display);
    if (server->socket == NULL) {
        wlr_log(WLR_ERROR, "%s", "leme: failed to create Wayland socket");
        return false;
    }

    if (!leme_ipc_init(server)) {
        return false;
    }

    return true;
}

int
leme_server_run(struct leme_server *server)
{
    if (!wlr_backend_start(server->backend)) {
        wlr_log(WLR_ERROR, "%s", "leme: failed to start backend");
        return EXIT_FAILURE;
    }

    server->started = true;
    wlr_log(WLR_INFO, "leme: WAYLAND_DISPLAY=%s", server->socket);
    leme_session_environment_publish(server);
    leme_config_launch_startup(server);
    wl_display_run(server->display);
    return EXIT_SUCCESS;
}

void
leme_server_finish(struct leme_server *server)
{
    if (server == NULL || server->display == NULL) {
        return;
    }
    leme_xwayland_finish(server);
    if (server->display != NULL) {
        wl_display_destroy_clients(server->display);
    }
    leme_scratchpad_finish(server);

    leme_desktop_finish(server);
    leme_tearing_finish(server);
    leme_capture_finish(server);
    leme_session_finish(server);
    leme_data_finish(server);
    leme_input_protocols_finish(server);
    leme_input_finish(server);
    leme_view_finish(server);
    leme_layer_finish(server);
    leme_ipc_finish(server);
    leme_publication_finish(server);
    leme_output_power_finish(server);
    leme_output_finish(server);

    if (server->sigint_source != NULL) {
        wl_event_source_remove(server->sigint_source);
        server->sigint_source = NULL;
    }

    if (server->session_active.link.next != NULL) {
        wl_list_remove(&server->session_active.link);
        server->session_active.link.next = NULL;
        server->session_active.link.prev = NULL;
    }
    if (server->backend != NULL) {
        wlr_backend_destroy(server->backend);
        server->backend = NULL;
    }

    leme_config_destroy(server->config);
    server->config = NULL;

    leme_animation_manager_finish(&server->animations);
    leme_render_finish(server);
    leme_graphics_finish(server);

    if (server->output_layout != NULL) {
        wlr_output_layout_destroy(server->output_layout);
        server->output_layout = NULL;
    }

    if (server->seat != NULL) {
        wlr_seat_destroy(server->seat);
        server->seat = NULL;
    }

    if (server->allocator != NULL) {
        wlr_allocator_destroy(server->allocator);
        server->allocator = NULL;
    }

    if (server->renderer != NULL) {
        wlr_renderer_destroy(server->renderer);
        server->renderer = NULL;
    }

    if (server->display != NULL) {
        wl_display_destroy(server->display);
        server->display = NULL;
    }
    server->compositor = NULL;
    server->viewporter = NULL;
    server->fractional_scale_manager = NULL;
}
