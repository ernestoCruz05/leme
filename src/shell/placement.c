#include "shell/placement.h"

#include <limits.h>
#include <math.h>

static bool leme_placement_valid_area(struct leme_box area) {
  return area.width > 0 && area.height > 0 && area.x <= INT_MAX - area.width &&
         area.y <= INT_MAX - area.height;
}

struct leme_box leme_placement_translate_box(struct leme_box box,
                                             struct leme_box from,
                                             struct leme_box to) {
  if (!leme_placement_valid_area(box)) {
    return (struct leme_box){0};
  }
  const int64_t x = (int64_t)box.x + to.x - from.x;
  const int64_t y = (int64_t)box.y + to.y - from.y;
  if (x < INT_MIN || y < INT_MIN || x + box.width > INT_MAX ||
      y + box.height > INT_MAX) {
    return (struct leme_box){0};
  }
  box.x = (int)x;
  box.y = (int)y;
  return box;
}

static int leme_placement_round(double value) {
  if (value >= INT_MAX) {
    return INT_MAX;
  }
  if (value <= INT_MIN) {
    return INT_MIN;
  }
  return (int)llround(value);
}

struct leme_box
leme_placement_anchored_box(const struct leme_placement_anchor *anchor) {
  if (anchor == NULL || !leme_placement_valid_area(anchor->original) ||
      !leme_placement_valid_area(anchor->restored) ||
      !leme_placement_valid_area(anchor->frame) || !isfinite(anchor->start_x) ||
      !isfinite(anchor->start_y) || !isfinite(anchor->cursor_x) ||
      !isfinite(anchor->cursor_y)) {
    return (struct leme_box){0};
  }
  const double x = fmin(1.0, fmax(0.0, (anchor->start_x - anchor->original.x) /
                                           anchor->original.width));
  const double y = fmin(1.0, fmax(0.0, (anchor->start_y - anchor->original.y) /
                                           anchor->original.height));
  struct leme_box box = anchor->frame;
  box.x = leme_placement_round(anchor->cursor_x -
                               ((double)anchor->restored.x - box.x) -
                               x * anchor->restored.width);
  box.y = leme_placement_round(anchor->cursor_y -
                               ((double)anchor->restored.y - box.y) -
                               y * anchor->restored.height);
  return box;
}

bool leme_placement_set(struct leme_placement_state *state,
                        enum leme_view_placement kind,
                        struct leme_box normal_content) {
  if (state == NULL || kind < LEME_VIEW_PLACEMENT_NORMAL ||
      kind > LEME_VIEW_PLACEMENT_BOTTOM_RIGHT) {
    return false;
  }
  if (kind == LEME_VIEW_PLACEMENT_NORMAL) {
    leme_placement_forget(state);
    return true;
  }
  if (state->kind == LEME_VIEW_PLACEMENT_NORMAL) {
    if (!leme_placement_valid_area(normal_content)) {
      return false;
    }
    state->restore_box = normal_content;
    state->restore_valid = true;
  }
  state->kind = kind;
  return true;
}

void leme_placement_forget(struct leme_placement_state *state) {
  if (state != NULL) {
    *state = (struct leme_placement_state){0};
  }
}

struct leme_box leme_placement_box(struct leme_box area,
                                   enum leme_view_placement placement) {
  if (!leme_placement_valid_area(area) ||
      placement <= LEME_VIEW_PLACEMENT_NORMAL ||
      placement > LEME_VIEW_PLACEMENT_BOTTOM_RIGHT) {
    return (struct leme_box){0};
  }
  if (placement == LEME_VIEW_PLACEMENT_MAXIMIZED) {
    return area;
  }
  const int left_width = area.width / 2;
  if (placement == LEME_VIEW_PLACEMENT_RIGHT_HALF ||
      placement == LEME_VIEW_PLACEMENT_TOP_RIGHT ||
      placement == LEME_VIEW_PLACEMENT_BOTTOM_RIGHT) {
    area.x += left_width;
    area.width -= left_width;
  } else {
    area.width = left_width;
  }
  const int top_height = area.height / 2;
  if (placement == LEME_VIEW_PLACEMENT_TOP_LEFT ||
      placement == LEME_VIEW_PLACEMENT_TOP_RIGHT) {
    area.height = top_height;
  } else if (placement == LEME_VIEW_PLACEMENT_BOTTOM_LEFT ||
             placement == LEME_VIEW_PLACEMENT_BOTTOM_RIGHT) {
    area.y += top_height;
    area.height -= top_height;
  }
  return area.width > 0 && area.height > 0 ? area : (struct leme_box){0};
}

enum leme_view_placement
leme_placement_at(struct leme_box full, double x, double y,
                  bool (*has_neighbour)(double x, double y, void *data),
                  void *data) {
  if (!leme_placement_valid_area(full) || !isfinite(x) || !isfinite(y)) {
    return LEME_VIEW_PLACEMENT_NORMAL;
  }
  const double right = (double)full.x + full.width;
  const double bottom = (double)full.y + full.height;
  if (x < full.x || x >= right || y < full.y || y >= bottom) {
    return LEME_VIEW_PLACEMENT_NORMAL;
  }
  const bool left =
      x - full.x < 48.0 &&
      (has_neighbour == NULL || !has_neighbour((double)full.x - 1.0, y, data));
  const bool right_side =
      right - x <= 48.0 &&
      (has_neighbour == NULL || !has_neighbour(right + 1.0, y, data));
  const bool top =
      y - full.y < 48.0 &&
      (has_neighbour == NULL || !has_neighbour(x, (double)full.y - 1.0, data));
  const bool bottom_side =
      bottom - y <= 48.0 &&
      (has_neighbour == NULL || !has_neighbour(x, bottom + 1.0, data));
  const bool touch_left = left && x - full.x < 2.0;
  const bool touch_right = right_side && right - x <= 2.0;
  const bool touch_top = top && y - full.y < 2.0;
  const bool touch_bottom = bottom_side && bottom - y <= 2.0;
  if (left && top && (touch_left || touch_top)) {
    return LEME_VIEW_PLACEMENT_TOP_LEFT;
  }
  if (right_side && top && (touch_right || touch_top)) {
    return LEME_VIEW_PLACEMENT_TOP_RIGHT;
  }
  if (left && bottom_side && (touch_left || touch_bottom)) {
    return LEME_VIEW_PLACEMENT_BOTTOM_LEFT;
  }
  if (right_side && bottom_side && (touch_right || touch_bottom)) {
    return LEME_VIEW_PLACEMENT_BOTTOM_RIGHT;
  }
  if (touch_top) {
    return LEME_VIEW_PLACEMENT_MAXIMIZED;
  }
  if (touch_left) {
    return LEME_VIEW_PLACEMENT_LEFT_HALF;
  }
  if (touch_right) {
    return LEME_VIEW_PLACEMENT_RIGHT_HALF;
  }
  return LEME_VIEW_PLACEMENT_NORMAL;
}

enum leme_view_placement leme_placement_snap(enum leme_view_placement current,
                                             enum leme_direction direction) {
  if (current > LEME_VIEW_PLACEMENT_BOTTOM_RIGHT ||
      direction > LEME_DIRECTION_DOWN) {
    return current;
  }
  const enum leme_view_placement n = LEME_VIEW_PLACEMENT_NORMAL;
  const enum leme_view_placement m = LEME_VIEW_PLACEMENT_MAXIMIZED;
  const enum leme_view_placement l = LEME_VIEW_PLACEMENT_LEFT_HALF;
  const enum leme_view_placement r = LEME_VIEW_PLACEMENT_RIGHT_HALF;
  const enum leme_view_placement tl = LEME_VIEW_PLACEMENT_TOP_LEFT;
  const enum leme_view_placement tr = LEME_VIEW_PLACEMENT_TOP_RIGHT;
  const enum leme_view_placement bl = LEME_VIEW_PLACEMENT_BOTTOM_LEFT;
  const enum leme_view_placement br = LEME_VIEW_PLACEMENT_BOTTOM_RIGHT;
  const enum leme_view_placement table[8][4] = {
      [LEME_VIEW_PLACEMENT_NORMAL] = {l, r, m, n},
      [LEME_VIEW_PLACEMENT_MAXIMIZED] = {l, r, m, n},
      [LEME_VIEW_PLACEMENT_LEFT_HALF] = {l, n, tl, bl},
      [LEME_VIEW_PLACEMENT_RIGHT_HALF] = {n, r, tr, br},
      [LEME_VIEW_PLACEMENT_TOP_LEFT] = {tl, tr, m, l},
      [LEME_VIEW_PLACEMENT_TOP_RIGHT] = {tl, tr, m, r},
      [LEME_VIEW_PLACEMENT_BOTTOM_LEFT] = {bl, br, l, bl},
      [LEME_VIEW_PLACEMENT_BOTTOM_RIGHT] = {bl, br, r, br},
  };
  return table[current][direction];
}
