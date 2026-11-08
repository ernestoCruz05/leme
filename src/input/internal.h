#ifndef LEME_INPUT_INTERNAL_H
#define LEME_INPUT_INTERNAL_H

struct leme_server;
struct wlr_input_device;
struct wlr_keyboard;
struct wlr_pointer_hold_begin_event;
struct wlr_pointer_hold_end_event;
struct wlr_pointer_pinch_begin_event;
struct wlr_pointer_pinch_end_event;
struct wlr_pointer_pinch_update_event;
struct wlr_pointer_swipe_begin_event;
struct wlr_pointer_swipe_end_event;
struct wlr_pointer_swipe_update_event;

#include "input/input.h"
#include "public/model.h"
#include <libinput.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum leme_input_setting_key {
  LEME_INPUT_SETTING_ACCEL_PROFILE,
  LEME_INPUT_SETTING_ACCEL_SPEED,
  LEME_INPUT_SETTING_NATURAL_SCROLL,
  LEME_INPUT_SETTING_LEFT_HANDED,
  LEME_INPUT_SETTING_TAP,
  LEME_INPUT_SETTING_DISABLE_WHILE_TYPING,
  LEME_INPUT_SETTING_CLICK_METHOD,
  LEME_INPUT_SETTING_SCROLL_METHOD,
  LEME_INPUT_SETTING_SCROLL_BUTTON,
  LEME_INPUT_SETTING_MIDDLE_EMULATION,
  LEME_INPUT_SETTING_TAP_BUTTON_MAP,
  LEME_INPUT_SETTING_TAP_DRAG,
  LEME_INPUT_SETTING_DRAG_LOCK,
  LEME_INPUT_SETTING_DRAG_FINGERS,
  LEME_INPUT_SETTING_SCROLL_FACTOR,
};

struct leme_input_setting_val {
  enum leme_input_setting_key key;
  enum libinput_config_accel_profile profile;
  double speed;
  bool boolean;
  unsigned choice;
  uint32_t fingers;
  double factor;
};

struct leme_input_target_info {
  bool exists;
  bool is_keyboard;
  bool is_libinput;
  bool has_accel;
  bool has_natural_scroll;
  bool has_left_handed;
  bool has_tap;
  bool adaptive;
  bool flat;
  double current_accel_speed;
  enum libinput_config_accel_profile current_accel_profile;
  bool current_natural_scroll;
  bool current_left_handed;
  bool current_tap;
  bool has_disable_while_typing;
  bool has_middle_emulation;
  bool has_scroll_button;
  uint32_t click_methods;
  uint32_t scroll_methods;
  uint32_t drag_finger_count;
  bool current_disable_while_typing;
  bool current_middle_emulation;
  bool current_tap_drag;
  unsigned current_click_method;
  unsigned current_scroll_method;
  bool has_current_scroll_button;
  unsigned current_scroll_button;
  unsigned current_tap_button_map;
  unsigned current_drag_lock;
  uint32_t current_drag_fingers;
  double current_scroll_factor;
};

void leme_input_update_capabilities(struct leme_server *server);
void leme_input_keyboard_add(struct leme_server *server,
                             struct wlr_input_device *device);
void leme_input_virtual_keyboard_add(struct leme_server *server,
                                     struct wlr_keyboard *keyboard);
void leme_input_keyboards_finish(struct leme_server *server);
void leme_input_pointer_add(struct leme_server *server,
                            struct wlr_input_device *device);
void leme_input_pointer_events_init(struct leme_server *server);
void leme_input_pointers_finish(struct leme_server *server);

bool leme_input_pointer_resolve_id(const struct leme_server *server,
                                   struct leme_public_id id,
                                   struct leme_input_target_info *info);
bool leme_input_keyboard_resolve_id(const struct leme_server *server,
                                    struct leme_public_id id,
                                    struct leme_input_target_info *info);
bool leme_input_resolve_target(const struct leme_server *server,
                               struct leme_public_id id,
                               struct leme_input_target_info *info);
enum libinput_config_status
leme_input_apply_pointer_setting(struct leme_server *server,
                                 struct leme_public_id id,
                                 const struct leme_input_setting_val *val);
enum leme_workspace_swipe {
  LEME_WORKSPACE_SWIPE_IGNORED,
  LEME_WORKSPACE_SWIPE_PENDING,
  LEME_WORKSPACE_SWIPE_ENGAGED,
  LEME_WORKSPACE_SWIPE_DECLINED,
};

enum leme_workspace_swipe leme_input_workspace_swipe_begin(
    struct leme_server *server,
    const struct wlr_pointer_swipe_begin_event *event);
enum leme_workspace_swipe leme_input_workspace_swipe_update(
    struct leme_server *server,
    const struct wlr_pointer_swipe_update_event *event);
void leme_input_workspace_swipe_end(
    struct leme_server *server,
    const struct wlr_pointer_swipe_end_event *event);
void leme_input_gesture_swipe_begin(
    struct leme_server *server,
    const struct wlr_pointer_swipe_begin_event *event);
void leme_input_gesture_swipe_update(
    struct leme_server *server,
    const struct wlr_pointer_swipe_update_event *event);
void leme_input_gesture_swipe_end(
    struct leme_server *server,
    const struct wlr_pointer_swipe_end_event *event);
void leme_input_gesture_pinch_begin(
    struct leme_server *server,
    const struct wlr_pointer_pinch_begin_event *event);
void leme_input_gesture_pinch_update(
    struct leme_server *server,
    const struct wlr_pointer_pinch_update_event *event);
void leme_input_gesture_pinch_end(
    struct leme_server *server,
    const struct wlr_pointer_pinch_end_event *event);
void leme_input_gesture_hold_begin(
    struct leme_server *server,
    const struct wlr_pointer_hold_begin_event *event);
void leme_input_gesture_hold_end(
    struct leme_server *server, const struct wlr_pointer_hold_end_event *event);
const struct leme_binding *leme_input_gesture_binding(
    const struct leme_server *server, enum leme_binding_trigger trigger,
    uint32_t fingers, uint32_t modifiers, enum leme_gesture_direction direction,
    enum leme_edge_side edge);
void leme_input_gesture_run(struct leme_server *server,
                            const struct leme_binding *binding);
bool leme_input_pointer_gesture_grab_begin(struct leme_server *server,
                                           bool resize);
void leme_input_pointer_gesture_grab_motion(struct leme_server *server,
                                            uint32_t time_msec, double dx,
                                            double dy);
void leme_input_pointer_gesture_grab_end(struct leme_server *server,
                                         bool cancelled);
struct leme_edge_reader;
enum leme_edge_event {
  LEME_EDGE_EVENT_MOTION,
  LEME_EDGE_EVENT_AXIS,
  LEME_EDGE_EVENT_PRESS,
  LEME_EDGE_EVENT_RELEASE,
  LEME_EDGE_EVENT_GESTURE,
};
struct leme_edge_reader *
leme_input_edge_reader_create(struct leme_server *server,
                              struct wlr_input_device *device);
void leme_input_edge_reader_destroy(struct leme_edge_reader *reader);
bool leme_input_edge_mutes(struct leme_edge_reader *reader,
                           enum leme_edge_event event, uint32_t button);
bool leme_input_find_keyboard_layout(const struct leme_server *server,
                                     const char *label, size_t *out_index,
                                     bool *out_ambiguous);

#endif
