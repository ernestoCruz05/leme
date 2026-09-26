#include "input/swipe_tracker.h"

#include <float.h>
#include <math.h>
#include <stddef.h>

void leme_swipe_tracker_init(struct leme_swipe_tracker *tracker,
                             uint32_t window_ms, double deceleration) {
  if (tracker == NULL) {
    return;
  }
  tracker->count = 0;
  tracker->head = 0;
  tracker->window_ms = window_ms > 0 ? window_ms : 150;
  tracker->deceleration =
      deceleration > 0.0 && deceleration < 1.0 ? deceleration : 0.997;
}

void leme_swipe_tracker_reset(struct leme_swipe_tracker *tracker) {
  if (tracker == NULL) {
    return;
  }
  tracker->count = 0;
  tracker->head = 0;
}

void leme_swipe_tracker_push(struct leme_swipe_tracker *tracker, double delta,
                             uint32_t time_msec) {
  if (tracker == NULL || !isfinite(delta)) {
    return;
  }
  tracker->samples[tracker->head].delta = delta;
  tracker->samples[tracker->head].time_msec = time_msec;
  tracker->head = (tracker->head + 1) % LEME_SWIPE_TRACKER_CAP;
  if (tracker->count < LEME_SWIPE_TRACKER_CAP) {
    tracker->count++;
  }
}

double leme_swipe_tracker_velocity(const struct leme_swipe_tracker *tracker,
                                   uint32_t now_msec) {
  double total_delta = 0.0;
  uint32_t min_age = UINT32_MAX;
  uint32_t max_age = 0;
  uint32_t dt;
  size_t valid_samples = 0;
  size_t i;

  if (tracker == NULL || tracker->count == 0) {
    return 0.0;
  }
  for (i = 0; i < tracker->count; i++) {
    const struct leme_swipe_sample *sample = &tracker->samples[i];
    const uint32_t age = now_msec - sample->time_msec;

    /* Rejeita leituras futuras ou fora de ordem */
    if (age > (uint32_t)INT32_MAX) {
      continue;
    }
    if (age <= tracker->window_ms) {
      total_delta += sample->delta;
      if (age < min_age) {
        min_age = age;
      }
      if (age > max_age) {
        max_age = age;
      }
      valid_samples++;
    }
  }
  if (valid_samples < 2 || max_age <= min_age || !isfinite(total_delta)) {
    return 0.0;
  }
  dt = max_age - min_age;
  if (dt == 0) {
    return 0.0;
  }
  const double v = (total_delta / (double)dt) * 1000.0;
  return isfinite(v) ? v : 0.0;
}

double
leme_swipe_tracker_projected_position(const struct leme_swipe_tracker *tracker,
                                      double current_position,
                                      uint32_t now_msec) {
  double v;
  double log_dec;
  double proj;

  if (tracker == NULL || !isfinite(current_position)) {
    return isfinite(current_position) ? current_position : 0.0;
  }
  v = leme_swipe_tracker_velocity(tracker, now_msec);
  if (!isfinite(v) || v == 0.0) {
    return current_position;
  }
  log_dec = log(tracker->deceleration);
  if (!isfinite(log_dec) || fabs(log_dec) < 1e-9) {
    return current_position;
  }
  proj = current_position - v / (1000.0 * log_dec);
  return isfinite(proj) ? proj : current_position;
}

bool leme_swipe_tracker_threshold_reached(double projected, double threshold,
                                          bool forward) {
  double frac;

  if (!isfinite(projected)) {
    return false;
  }
  frac = projected - floor(projected);
  if (!isfinite(frac)) {
    return false;
  }
  return forward ? frac >= threshold : frac <= (1.0 - threshold);
}

double leme_swipe_continuous_target(double projected, double threshold,
                                    bool forward) {
  /* Beyond this range doubles cannot distinguish adjacent tag coordinates. */
  if (!isfinite(projected) || fabs(projected) > 0x1p52 - 2.0 ||
      !(threshold > 0.0 && threshold < 1.0)) {
    return NAN;
  }
  const double base = floor(projected);
  if (projected == base) {
    return base;
  }
  if (forward) {
    return projected >= base + threshold ? base + 1.0 : base;
  }
  return projected <= base + 1.0 - threshold ? base : base + 1.0;
}

double leme_swipe_position(double raw_position, double center) {
  if (!isfinite(raw_position) || !isfinite(center)) {
    return isfinite(center) ? center : 0.0;
  }

  const double lo = center - 1.0;
  const double hi = center + 1.0;
  if (raw_position >= lo && raw_position <= hi) {
    return raw_position;
  }

  const double edge = raw_position < lo ? lo : hi;
  const double diff = raw_position - edge;
  const double distance = fabs(diff);
  if (!isfinite(distance) || distance >= (DBL_MAX / 10.0)) {
    return edge + copysign(0.05, diff);
  }

  const double denominator = 1.0 + distance * 10.0;
  const double band = 0.05 * (1.0 - 1.0 / denominator);
  return edge + copysign(band, diff);
}

double leme_swipe_position_derivative(double raw_position, double center) {
  if (!isfinite(raw_position) || !isfinite(center)) {
    return 0.0;
  }

  const double lo = center - 1.0;
  const double hi = center + 1.0;
  if (raw_position >= lo && raw_position <= hi) {
    return 1.0;
  }

  const double edge = raw_position < lo ? lo : hi;
  const double distance = fabs(raw_position - edge);
  if (!isfinite(distance) || distance >= (DBL_MAX / 10.0)) {
    return 0.0;
  }

  const double denominator = 1.0 + distance * 10.0;
  const double deriv = (0.5 / denominator) / denominator;
  return isfinite(deriv) ? deriv : 0.0;
}

double leme_swipe_target(double projected_position, double center,
                         double threshold) {
  if (!isfinite(center)) {
    return 0.0;
  }
  if (!isfinite(projected_position)) {
    return center;
  }
  if (!isfinite(threshold) || threshold <= 0.0 || threshold >= 1.0) {
    return center;
  }
  if (projected_position >= center + threshold) {
    return center + 1.0;
  }
  if (projected_position <= center - threshold) {
    return center - 1.0;
  }
  return center;
}
