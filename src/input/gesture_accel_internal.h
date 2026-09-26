#ifndef LEME_INPUT_GESTURE_ACCEL_INTERNAL_H
#define LEME_INPUT_GESTURE_ACCEL_INTERNAL_H

#include "input/gesture_accel.h"
#include <stdint.h>

struct leme_gesture_accel_ops {
  struct libinput_device *(*ref)(struct libinput_device *);
  struct libinput_device *(*unref)(struct libinput_device *);
  int (*available)(struct libinput_device *);
  uint32_t (*profiles)(struct libinput_device *);
  enum libinput_config_accel_profile (*get_profile)(struct libinput_device *);
  double (*get_speed)(struct libinput_device *);
  enum libinput_config_status (*set_profile)(
      struct libinput_device *, enum libinput_config_accel_profile);
  enum libinput_config_status (*set_speed)(struct libinput_device *, double);
};

bool leme_gesture_accel_begin_with_ops(
    struct leme_gesture_accel *state, struct libinput_device *device,
    const struct leme_gesture_accel_ops *ops);

#endif
