#ifndef LEME_H
#define LEME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define LEME_ARRAY_LENGTH(array) (sizeof(array) / sizeof((array)[0]))

struct leme_gaps {
  int top;
  int right;
  int bottom;
  int left;
};

struct leme_box {
  int x;
  int y;
  int width;
  int height;
};

static inline bool leme_box_equal(struct leme_box a, struct leme_box b) {
  return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

static inline bool leme_box_contains(struct leme_box box, double x, double y) {
  return x >= box.x && y >= box.y && x < (double)box.x + box.width &&
         y < (double)box.y + box.height;
}

#endif
