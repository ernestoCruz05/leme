#include "protocols/gamma.h"

#include "core/server.h"
#include "output/output.h"

#include <wayland-server-core.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_output.h>

static void leme_gamma_handle_set(struct wl_listener *listener, void *data) {
  struct leme_server *server = wl_container_of(listener, server, gamma_set);
  const struct wlr_gamma_control_manager_v1_set_gamma_event *event = data;
  struct leme_output *output =
      leme_output_from_wlr_output(server, event->output);

  if (output == NULL) {
    return;
  }
  output->gamma_changed = true;
  wlr_output_schedule_frame(output->wlr_output);
}

bool leme_gamma_init(struct leme_server *server) {
  server->gamma_control_manager =
      wlr_gamma_control_manager_v1_create(server->display);
  if (server->gamma_control_manager == NULL) {
    return false;
  }
  server->gamma_set.notify = leme_gamma_handle_set;
  wl_signal_add(&server->gamma_control_manager->events.set_gamma,
                &server->gamma_set);
  return true;
}

void leme_gamma_finish(struct leme_server *server) {
  if (server->gamma_set.link.next != NULL) {
    wl_list_remove(&server->gamma_set.link);
    server->gamma_set.link.next = NULL;
    server->gamma_set.link.prev = NULL;
  }
  server->gamma_control_manager = NULL;
}

void leme_gamma_apply(struct leme_output *output,
                      struct wlr_output_state *state) {
  struct wlr_gamma_control_v1 *control =
      wlr_gamma_control_manager_v1_get_control(
          output->server->gamma_control_manager, output->wlr_output);
  struct wlr_output_state pending = {0};

  output->gamma_changed = false;
  if (!wlr_output_state_copy(&pending, state)) {
    return;
  }
  if (wlr_gamma_control_v1_apply(control, &pending) &&
      wlr_output_test_state(output->wlr_output, &pending)) {
    (void)wlr_output_state_copy(state, &pending);
  } else {
    wlr_gamma_control_v1_send_failed_and_destroy(control);
  }
  wlr_output_state_finish(&pending);
}
