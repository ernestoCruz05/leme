#include "shell/container-policy.h"

#include <math.h>

bool leme_container_drop_at(struct leme_box content, double lx, double ly,
                            struct leme_layout_drop *drop) {
  const struct leme_box area =
      leme_placement_box(content, LEME_VIEW_PLACEMENT_MAXIMIZED);
  if (drop == NULL || area.width == 0 || !isfinite(lx) || !isfinite(ly)) {
    return false;
  }
  const double right = (double)content.x + content.width;
  const double bottom = (double)content.y + content.height;
  if (lx < content.x || lx >= right || ly < content.y || ly >= bottom) {
    return false;
  }
  const bool edge = lx - content.x < LEME_CONTAINER_DROP_EDGE_PX ||
                    right - lx < LEME_CONTAINER_DROP_EDGE_PX ||
                    ly - content.y < LEME_CONTAINER_DROP_EDGE_PX ||
                    bottom - ly < LEME_CONTAINER_DROP_EDGE_PX;
  const enum leme_drop_mode mode =
      edge ? LEME_DROP_MODE_EDGES : LEME_DROP_MODE_SIMPLE;
  struct leme_layout_drop candidate = {0};
  if (!leme_layout_drop_at(mode, content, lx, ly, &candidate)) {
    return false;
  }
  const bool horizontal = candidate.direction == LEME_DIRECTION_LEFT ||
                          candidate.direction == LEME_DIRECTION_RIGHT;
  if ((horizontal && content.width < 2) ||
      (!horizontal && content.height < 2)) {
    return false;
  }
  *drop = candidate;
  return true;
}

enum leme_container_resolution leme_container_resolve_count(size_t count) {
  if (count == 0) {
    return LEME_CONTAINER_DESTROY;
  }
  return count == 1 ? LEME_CONTAINER_UNWRAP : LEME_CONTAINER_KEEP;
}

size_t leme_container_next_focus(const bool *eligible, size_t count,
                                 size_t removed) {
  if (eligible == NULL || removed >= count) {
    return SIZE_MAX;
  }
  size_t index = removed;
  for (size_t searched = 1; searched < count; searched++) {
    index = index == count - 1 ? 0 : index + 1;
    if (eligible[index]) {
      return index;
    }
  }
  return SIZE_MAX;
}
