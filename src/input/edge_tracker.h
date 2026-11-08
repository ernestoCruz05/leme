#ifndef LEME_INPUT_EDGE_TRACKER_H
#define LEME_INPUT_EDGE_TRACKER_H

#include <stdint.h>

#define LEME_EDGE_DECIDE_MM 3.0
#define LEME_EDGE_DECIDE_MSEC 300u

enum leme_edge_side {
  LEME_EDGE_NONE,
  LEME_EDGE_LEFT,
  LEME_EDGE_RIGHT,
  LEME_EDGE_TOP,
  LEME_EDGE_BOTTOM,
};

enum leme_edge_state {
  LEME_EDGE_IDLE,
  LEME_EDGE_PENDING,
  LEME_EDGE_CLAIMED,
  LEME_EDGE_PASSED,
};

struct leme_edge_touch {
  uint32_t touches;
  double x;
  double y;
  uint32_t msec;
};

struct leme_edge_tracker {
  double width;
  double height;
  enum leme_edge_state state;
  uint32_t sides;
  enum leme_edge_side side;
  double start_x;
  double start_y;
  double anchor;
  uint32_t start_msec;
};

void leme_edge_tracker_init(struct leme_edge_tracker *tracker, double width,
                            double height);
int leme_edge_tracker_update(struct leme_edge_tracker *tracker,
                             const struct leme_edge_touch *touch,
                             uint32_t bound_sides, double size, double step);

#endif
