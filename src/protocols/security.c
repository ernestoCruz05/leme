#include "protocols/security.h"

#include "core/server.h"
#include "shell/xwayland.h"

#include <string.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_security_context_v1.h>

static const char *const leme_security_privileged[] = {
    "ext_data_control_manager_v1",
    "ext_foreign_toplevel_image_capture_source_manager_v1",
    "ext_foreign_toplevel_list_v1",
    "ext_image_copy_capture_manager_v1",
    "ext_output_image_capture_source_manager_v1",
    "ext_session_lock_manager_v1",
    "ext_workspace_manager_v1",
    "leme_decoration_manager_v1",
    "wp_security_context_manager_v1",
    "zwlr_data_control_manager_v1",
    "zwlr_foreign_toplevel_manager_v1",
    "zwlr_gamma_control_manager_v1",
    "zwlr_layer_shell_v1",
    "zwlr_output_manager_v1",
    "zwlr_output_power_manager_v1",
    "zwlr_screencopy_manager_v1",
    "zwlr_virtual_pointer_manager_v1",
    "zwp_virtual_keyboard_manager_v1",
};

static bool leme_security_filter_global(const struct wl_client *client,
                                        const struct wl_global *global,
                                        void *data) {
  const struct leme_server *server = data;

  if (!leme_xwayland_global_allowed(server, client, global)) {
    return false;
  }
  if (wlr_security_context_manager_v1_lookup_client(
          server->security_context_manager, client) == NULL) {
    return true;
  }
  const char *name = wl_global_get_interface(global)->name;
  for (size_t i = 0; i < LEME_ARRAY_LENGTH(leme_security_privileged); i++) {
    if (strcmp(name, leme_security_privileged[i]) == 0) {
      return false;
    }
  }
  return true;
}

bool leme_security_init(struct leme_server *server) {
  server->security_context_manager =
      wlr_security_context_manager_v1_create(server->display);
  if (server->security_context_manager == NULL) {
    return false;
  }
  wl_display_set_global_filter(server->display, leme_security_filter_global,
                               server);
  return true;
}
