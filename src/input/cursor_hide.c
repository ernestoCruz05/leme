#include "config/config.h"
#include "core/server.h"
#include "desktop/owner.h"
#include "input/input.h"
#include "input/internal.h"
#include "input/pointer_internal.h"
#include "protocols/desktop.h"
#include "protocols/input.h"

#include <wayland-server-core.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_seat.h>

void leme_input_cursor_hide(struct leme_server *server) {
  if (server->cursor_hidden || server->seat->pointer_state.button_count > 0 ||
      server->seat->drag != NULL || wlr_seat_pointer_has_grab(server->seat) ||
      leme_input_pointer_grab_active(server) ||
      leme_input_protocols_constraint_active(server) ||
      leme_desk_owner_modal(server)) {
    return;
  }
  server->cursor_hidden = true;
  leme_desktop_cursor_default(server);
  wlr_cursor_unset_image(server->cursor);
  wlr_seat_pointer_notify_clear_focus(server->seat);
}

static int leme_input_cursor_handle_hide_timer(void *data) {
  struct leme_server *server = data;

  if (server->config->cursor.hide_after > 0) {
    leme_input_cursor_hide(server);
  }
  return 0;
}

void leme_input_cursor_activity(struct leme_server *server,
                                uint32_t time_msec) {
  const int hide_after =
      server->config == NULL ? 0 : server->config->cursor.hide_after;

  if (hide_after > 0 && server->cursor_hide_timer == NULL) {
    server->cursor_hide_timer =
        wl_event_loop_add_timer(wl_display_get_event_loop(server->display),
                                leme_input_cursor_handle_hide_timer, server);
  }
  if (server->cursor_hide_timer != NULL) {
    wl_event_source_timer_update(server->cursor_hide_timer, hide_after);
  }
  if (!server->cursor_hidden) {
    return;
  }
  server->cursor_hidden = false;
  if (leme_input_pointer_grab_active(server)) {
    return;
  }
  leme_desktop_cursor_default(server);
  leme_input_process_motion(server, time_msec, NULL, false);
}
