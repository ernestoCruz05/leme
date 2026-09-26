#include "input/gesture_accel_internal.h"

#include <stdio.h>

static const struct leme_gesture_accel_ops prod_ops = {
    .ref = libinput_device_ref,
    .unref = libinput_device_unref,
    .available = libinput_device_config_accel_is_available,
    .profiles = libinput_device_config_accel_get_profiles,
    .get_profile = libinput_device_config_accel_get_profile,
    .get_speed = libinput_device_config_accel_get_speed,
    .set_profile = libinput_device_config_accel_set_profile,
    .set_speed = libinput_device_config_accel_set_speed,
};

bool leme_gesture_accel_restore(struct leme_gesture_accel *state) {
  if (state == NULL || state->device == NULL) {
    return true;
  }
  const struct leme_gesture_accel saved = *state;
  *state = (struct leme_gesture_accel){0};
  const enum libinput_config_status speed_status =
      saved.ops->set_speed(saved.device, saved.saved_speed);
  const enum libinput_config_status profile_status =
      saved.ops->set_profile(saved.device, saved.saved_profile);
  (void)saved.ops->unref(saved.device);
  return speed_status == LIBINPUT_CONFIG_STATUS_SUCCESS &&
         profile_status == LIBINPUT_CONFIG_STATUS_SUCCESS;
}

bool leme_gesture_accel_begin_with_ops(
    struct leme_gesture_accel *state, struct libinput_device *device,
    const struct leme_gesture_accel_ops *ops) {
  if (state == NULL || device == NULL || ops == NULL) {
    return false;
  }
  if (state->device != NULL) {
    return false;
  }
  if (ops->available == NULL || ops->profiles == NULL ||
      ops->get_profile == NULL || ops->get_speed == NULL ||
      ops->set_profile == NULL || ops->set_speed == NULL || ops->ref == NULL ||
      ops->unref == NULL) {
    return false;
  }
  if (!ops->available(device)) {
    return false;
  }
  const uint32_t supported = ops->profiles(device);
  if ((supported & (uint32_t)LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT) == 0) {
    return false;
  }

  const enum libinput_config_accel_profile saved_profile =
      ops->get_profile(device);
  if (saved_profile != LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE &&
      saved_profile != LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT) {
    return false;
  }
  const double saved_speed = ops->get_speed(device);

  struct libinput_device *ref = ops->ref(device);
  if (ref == NULL) {
    return false;
  }

  state->device = ref;
  state->saved_profile = saved_profile;
  state->saved_speed = saved_speed;
  state->ops = ops;

  if (ops->set_profile(ref, LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT) !=
      LIBINPUT_CONFIG_STATUS_SUCCESS) {
    if (!leme_gesture_accel_restore(state)) {
      fputs(
          "leme: failed to restore gesture acceleration after override failure\n",
          stderr);
    }
    return false;
  }

  if (ops->set_speed(ref, 0.0) != LIBINPUT_CONFIG_STATUS_SUCCESS) {
    if (!leme_gesture_accel_restore(state)) {
      fputs(
          "leme: failed to restore gesture acceleration after override failure\n",
          stderr);
    }
    return false;
  }

  return true;
}

bool leme_gesture_accel_begin(struct leme_gesture_accel *state,
                              struct libinput_device *device) {
  return leme_gesture_accel_begin_with_ops(state, device, &prod_ops);
}
