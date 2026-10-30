#include "desktop/grid.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

size_t leme_desk_grid_columns(struct leme_box area,
                              const struct leme_desk_grid_metrics *metrics) {
  return metrics->spacing < 1 || metrics->padding < 0 ||
                 area.width < 2 * metrics->padding
             ? 0
             : (size_t)((area.width - 2 * metrics->padding) / metrics->spacing);
}

size_t leme_desk_grid_rows(struct leme_box area,
                           const struct leme_desk_grid_metrics *metrics,
                           int row_height) {
  return metrics->spacing < 1 || metrics->padding < 0 ||
                 row_height < metrics->spacing ||
                 area.height < 2 * metrics->padding
             ? 0
             : (size_t)((area.height - 2 * metrics->padding) / row_height);
}

size_t leme_desk_grid_capacity(struct leme_box area,
                               const struct leme_desk_grid_metrics *metrics,
                               int row_height) {
  const size_t columns = leme_desk_grid_columns(area, metrics);
  const size_t rows = leme_desk_grid_rows(area, metrics, row_height);
  return rows == 0 || columns > SIZE_MAX / rows ? 0 : columns * rows;
}

bool leme_desk_grid_assign(size_t columns, size_t rows,
                           const struct leme_desk_wish *wishes, size_t count,
                           size_t *slots, size_t *placed) {
  *placed = 0;
  for (size_t i = 0; i < count; i++) {
    slots[i] = SIZE_MAX;
  }
  if (columns == 0 || rows == 0 || count == 0) {
    return true;
  }
  if (columns > SIZE_MAX / rows) {
    return false;
  }
  const size_t capacity = columns * rows;
  bool *occupied = calloc(capacity, sizeof(*occupied));
  if (occupied == NULL) {
    return false;
  }
  for (size_t i = 0; i < count; i++) {
    if (!wishes[i].pinned || wishes[i].column >= columns ||
        wishes[i].row >= rows) {
      continue;
    }
    const size_t slot = (size_t)wishes[i].row * columns + wishes[i].column;
    if (!occupied[slot]) {
      occupied[slot] = true;
      slots[i] = slot;
      (*placed)++;
    }
  }
  size_t free_slot = 0;
  for (size_t i = 0; i < count && *placed < capacity; i++) {
    if (slots[i] != SIZE_MAX) {
      continue;
    }
    while (occupied[free_slot]) {
      free_slot++;
    }
    occupied[free_slot] = true;
    slots[i] = free_slot;
    (*placed)++;
  }
  free(occupied);
  return true;
}

static uint64_t leme_desk_grid_distance(size_t from, size_t to) {
  const uint64_t delta = from > to ? from - to : to - from;
  return delta * delta;
}

size_t leme_desk_grid_nearest_free(size_t columns, size_t rows,
                                   const bool *occupied, size_t column,
                                   size_t row) {
  size_t best = SIZE_MAX;
  uint64_t best_distance = UINT64_MAX;
  for (size_t r = 0; r < rows; r++) {
    for (size_t c = 0; c < columns; c++) {
      const uint64_t distance =
          leme_desk_grid_distance(c, column) + leme_desk_grid_distance(r, row);
      if (!occupied[r * columns + c] && distance < best_distance) {
        best = r * columns + c;
        best_distance = distance;
      }
    }
  }
  return best;
}

bool leme_desk_grid_step(size_t columns, size_t rows, const bool *occupied,
                         size_t slot, enum leme_direction direction,
                         size_t *next) {
  if (columns == 0 || rows > SIZE_MAX / columns || slot >= columns * rows) {
    return false;
  }
  const size_t column = slot % columns;
  const size_t row = slot / columns;
  const bool horizontal =
      direction == LEME_DIRECTION_LEFT || direction == LEME_DIRECTION_RIGHT;
  bool found = false;
  bool best_aligned = false;
  uint64_t best_distance = UINT64_MAX;
  for (size_t r = 0; r < rows; r++) {
    for (size_t c = 0; c < columns; c++) {
      const bool ahead = (direction == LEME_DIRECTION_LEFT && c < column) ||
                         (direction == LEME_DIRECTION_RIGHT && c > column) ||
                         (direction == LEME_DIRECTION_UP && r < row) ||
                         (direction == LEME_DIRECTION_DOWN && r > row);
      if (!ahead || !occupied[r * columns + c]) {
        continue;
      }
      const bool aligned = horizontal ? r == row : c == column;
      const uint64_t distance =
          leme_desk_grid_distance(c, column) + leme_desk_grid_distance(r, row);
      if ((aligned && !best_aligned) ||
          (aligned == best_aligned && distance < best_distance)) {
        *next = r * columns + c;
        best_aligned = aligned;
        best_distance = distance;
        found = true;
      }
    }
  }
  return found;
}

bool leme_desk_grid_cell(struct leme_box area,
                         const struct leme_desk_grid_metrics *metrics,
                         int row_height, size_t index, struct leme_box *cell) {
  if (cell == NULL ||
      index >= leme_desk_grid_capacity(area, metrics, row_height)) {
    return false;
  }
  const size_t columns = leme_desk_grid_columns(area, metrics);
  const int64_t x = (int64_t)area.x + metrics->padding +
                    (int64_t)(index % columns) * metrics->spacing;
  const int64_t y = (int64_t)area.y + metrics->padding +
                    (int64_t)(index / columns) * row_height;
  if (x < INT_MIN || x > INT_MAX - metrics->spacing || y < INT_MIN ||
      y > INT_MAX - row_height) {
    return false;
  }
  *cell = (struct leme_box){
      .x = (int)x,
      .y = (int)y,
      .width = metrics->spacing,
      .height = row_height,
  };
  return true;
}

bool leme_desk_grid_pixels(float scale, int size, int *pixels) {
  if (pixels == NULL || !isfinite(scale) || scale < 0.5f || scale > 4.0f ||
      size < 1 || size > 4096) {
    return false;
  }
  *pixels = (int)ceil((double)scale * size);
  return true;
}
