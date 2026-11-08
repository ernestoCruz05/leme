#include "input/edge_tracker.h"

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>

void leme_edge_tracker_init(struct leme_edge_tracker *tracker, double width,
                            double height) {
  assert(tracker != NULL && width > 0.0 && height > 0.0);
  *tracker = (struct leme_edge_tracker){.width = width, .height = height};
}

int leme_edge_tracker_update(struct leme_edge_tracker *tracker,
                             const struct leme_edge_touch *touch,
                             uint32_t bound_sides, double size, double step) {
  assert(tracker != NULL && touch != NULL && size > 0.0 && step > 0.0);
  if (touch->touches == 0) {
    tracker->state = LEME_EDGE_IDLE;
    return 0;
  }
  if (tracker->state == LEME_EDGE_IDLE) {
    uint32_t sides = 0;
    if (touch->touches == 1) {
      sides |= touch->x < size ? 1u << LEME_EDGE_LEFT : 0;
      sides |= touch->x > tracker->width - size ? 1u << LEME_EDGE_RIGHT : 0;
      sides |= touch->y < size ? 1u << LEME_EDGE_TOP : 0;
      sides |= touch->y > tracker->height - size ? 1u << LEME_EDGE_BOTTOM : 0;
    }
    tracker->sides = sides & bound_sides;
    tracker->state = tracker->sides != 0 ? LEME_EDGE_PENDING : LEME_EDGE_PASSED;
    tracker->side = LEME_EDGE_NONE;
    tracker->start_x = touch->x;
    tracker->start_y = touch->y;
    tracker->start_msec = touch->msec;
    tracker->anchor = 0.0;
    return 0;
  }
  if (tracker->state == LEME_EDGE_PENDING) {
    const double dx = touch->x - tracker->start_x;
    const double dy = touch->y - tracker->start_y;

    tracker->sides &= bound_sides;
    if (tracker->sides == 0 || touch->touches > 1 ||
        touch->msec - tracker->start_msec > LEME_EDGE_DECIDE_MSEC) {
      tracker->state = LEME_EDGE_PASSED;
      return 0;
    }
    if (hypot(dx, dy) < LEME_EDGE_DECIDE_MM) {
      return 0;
    }
    const uint32_t along =
        fabs(dy) >= fabs(dx) ? (1u << LEME_EDGE_LEFT) | (1u << LEME_EDGE_RIGHT)
                             : (1u << LEME_EDGE_TOP) | (1u << LEME_EDGE_BOTTOM);
    const uint32_t sides = tracker->sides & along;
    if (sides == 0) {
      tracker->state = LEME_EDGE_PASSED;
      return 0;
    }
    tracker->side = (sides & (1u << LEME_EDGE_LEFT)) != 0    ? LEME_EDGE_LEFT
                    : (sides & (1u << LEME_EDGE_RIGHT)) != 0 ? LEME_EDGE_RIGHT
                    : (sides & (1u << LEME_EDGE_TOP)) != 0   ? LEME_EDGE_TOP
                                                             : LEME_EDGE_BOTTOM;
    tracker->state = LEME_EDGE_CLAIMED;
  }
  if (tracker->state != LEME_EDGE_CLAIMED) {
    return 0;
  }
  const bool vertical =
      tracker->side == LEME_EDGE_LEFT || tracker->side == LEME_EDGE_RIGHT;
  const double offset =
      vertical ? touch->y - tracker->start_y : touch->x - tracker->start_x;
  const double steps = trunc((offset - tracker->anchor) / step);
  tracker->anchor += steps * step;
  return (int)steps;
}
