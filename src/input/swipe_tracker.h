#ifndef LEME_INPUT_SWIPE_TRACKER_H
#define LEME_INPUT_SWIPE_TRACKER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define LEME_SWIPE_TRACKER_CAP 32

struct leme_swipe_sample {
  double delta;
  uint32_t time_msec;
};

struct leme_swipe_tracker {
  struct leme_swipe_sample samples[LEME_SWIPE_TRACKER_CAP];
  size_t count;
  size_t head;
  uint32_t window_ms;
  double deceleration;
};

void leme_swipe_tracker_init(struct leme_swipe_tracker *tracker,
                             uint32_t window_ms, double deceleration);
void leme_swipe_tracker_reset(struct leme_swipe_tracker *tracker);
void leme_swipe_tracker_push(struct leme_swipe_tracker *tracker, double delta,
                             uint32_t time_msec);
double leme_swipe_tracker_velocity(const struct leme_swipe_tracker *tracker,
                                   uint32_t now_msec);
double
leme_swipe_tracker_projected_position(const struct leme_swipe_tracker *tracker,
                                      double current_position,
                                      uint32_t now_msec);
bool leme_swipe_tracker_threshold_reached(double projected, double threshold,
                                          bool forward);

double leme_swipe_continuous_target(double projected, double threshold,
                                    bool forward);
double leme_swipe_position(double raw_position, double center);
double leme_swipe_position_derivative(double raw_position, double center);
double leme_swipe_target(double projected_position, double center,
                         double threshold);

#endif
