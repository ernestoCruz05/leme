#ifndef LEME_DESK_OWNER_H
#define LEME_DESK_OWNER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon.h>

struct leme_server;
struct wlr_pointer_axis_event;
struct wlr_pointer_button_event;

struct leme_desk_owner {
  bool modal;
  void (*motion)(struct leme_server *server);
  bool (*button)(struct leme_server *server,
                 const struct wlr_pointer_button_event *event);
  void (*axis)(struct leme_server *server,
               const struct wlr_pointer_axis_event *event);
  bool (*key)(struct leme_server *server, xkb_keysym_t keysym,
              uint32_t modifiers, const char *utf8, size_t length);
  void (*cancel)(struct leme_server *server);
};

struct leme_desk_owner_state {
  const struct leme_desk_owner *owner;
  uint32_t buttons;
  struct wl_listener pointer_grab;
  struct wl_listener keyboard_grab;
};

void leme_desk_owner_finish(struct leme_server *server);
void leme_desk_owner_claim(struct leme_server *server,
                           const struct leme_desk_owner *owner,
                           uint32_t button);
void leme_desk_owner_swallow(struct leme_server *server, uint32_t button);
void leme_desk_owner_release(struct leme_server *server,
                             const struct leme_desk_owner *owner);
void leme_desk_owner_handoff(struct leme_server *server,
                             const struct leme_desk_owner *owner,
                             uint32_t button);
bool leme_desk_owner_modal(const struct leme_server *server);
void leme_desk_owner_cancel(struct leme_server *server);
bool leme_desk_owner_handle_motion(struct leme_server *server);
bool leme_desk_owner_handle_button(
    struct leme_server *server, const struct wlr_pointer_button_event *event);
bool leme_desk_owner_handle_axis(struct leme_server *server,
                                 const struct wlr_pointer_axis_event *event);
bool leme_desk_owner_handle_key(struct leme_server *server, xkb_keysym_t keysym,
                                uint32_t modifiers, const char *utf8,
                                size_t length, bool reserved);

#endif
