#ifndef LEME_DESK_GRID_H
#define LEME_DESK_GRID_H

#include "core/leme.h"
#include "workspace/layout.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LEME_DESK_STRIP_ICON 48

struct leme_desk_grid_metrics {
  int size;
  int spacing;
  int padding;
  int width;
  int gap;
};

struct leme_desk_wish {
  bool pinned;
  uint32_t column;
  uint32_t row;
};

size_t leme_desk_grid_columns(struct leme_box area,
                              const struct leme_desk_grid_metrics *metrics);
size_t leme_desk_grid_rows(struct leme_box area,
                           const struct leme_desk_grid_metrics *metrics,
                           int row_height);
bool leme_desk_grid_assign(size_t columns, size_t rows,
                           const struct leme_desk_wish *wishes, size_t count,
                           size_t *slots, size_t *placed);
size_t leme_desk_grid_nearest_free(size_t columns, size_t rows,
                                   const bool *occupied, size_t column,
                                   size_t row);
bool leme_desk_grid_step(size_t columns, size_t rows, const bool *occupied,
                         size_t slot, enum leme_direction direction,
                         size_t *next);
bool leme_desk_grid_cell(struct leme_box area,
                         const struct leme_desk_grid_metrics *metrics,
                         int row_height, size_t index, struct leme_box *cell);
bool leme_desk_grid_pixels(float scale, int size, int *pixels);

#endif
