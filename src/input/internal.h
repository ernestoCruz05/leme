#ifndef LEME_INPUT_INTERNAL_H
#define LEME_INPUT_INTERNAL_H

struct leme_server;
struct wlr_input_device;
struct wlr_keyboard;

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
};

struct leme_input_setting_val {
  enum leme_input_setting_key key;
  enum libinput_config_accel_profile profile;
  double speed;
  bool boolean;
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
bool leme_input_find_keyboard_layout(const struct leme_server *server,
                                     const char *label, size_t *out_index,
                                     bool *out_ambiguous);

#endif
