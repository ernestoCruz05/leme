#include "workspace/layout.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct leme_layout_detach {
  struct leme_layout_node *leaf;
  struct leme_layout_node *branch;
  struct leme_layout_node *sibling;
  struct leme_layout_node *grandparent;
  bool branch_was_first;
};

static struct leme_layout_node *leme_layout_leaf(struct leme_view *view) {
  struct leme_layout_node *node = calloc(1, sizeof(*node));

  if (node != NULL) {
    node->is_leaf = true;
    node->data.view = view;
  }
  return node;
}

static struct leme_layout_node *leme_layout_find(struct leme_layout_node *node,
                                                 struct leme_view *view) {
  struct leme_layout_node *found;

  if (node == NULL) {
    return NULL;
  }
  if (node->is_leaf) {
    return node->data.view == view ? node : NULL;
  }
  found = leme_layout_find(node->data.branch.first, view);
  return found != NULL ? found
                       : leme_layout_find(node->data.branch.second, view);
}

static struct leme_layout_node *
leme_layout_first(struct leme_layout_node *node) {
  while (!node->is_leaf) {
    node = node->data.branch.first;
  }
  return node;
}

static unsigned int leme_layout_depth(const struct leme_layout_node *node) {
  unsigned int depth = 0;

  while (node->parent != NULL) {
    depth++;
    node = node->parent;
  }
  return depth;
}

static struct leme_box leme_layout_shape(const struct leme_layout_node *node,
                                         struct leme_box area) {
  const struct leme_layout_node *parent = node->parent;
  struct leme_box box;

  if (node->arranged.width > 0 && node->arranged.height > 0) {
    return node->arranged;
  }
  if (parent == NULL) {
    return area;
  }
  box = leme_layout_shape(parent, area);
  if (parent->data.branch.split == LEME_SPLIT_HORIZONTAL) {
    const int first = (int)lround(box.width * parent->data.branch.ratio);

    if (parent->data.branch.first == node) {
      box.width = first;
    } else {
      box.x += first;
      box.width -= first;
    }
  } else {
    const int first = (int)lround(box.height * parent->data.branch.ratio);

    if (parent->data.branch.first == node) {
      box.height = first;
    } else {
      box.y += first;
      box.height -= first;
    }
  }
  return box;
}

static enum leme_split
leme_layout_split_for(const struct leme_layout_node *leaf,
                      struct leme_box area) {
  const struct leme_box box = leme_layout_shape(leaf, area);

  if (box.width <= 0 || box.height <= 0) {
    return leme_layout_depth(leaf) % 2 == 0 ? LEME_SPLIT_HORIZONTAL
                                            : LEME_SPLIT_VERTICAL;
  }
  return box.width >= box.height ? LEME_SPLIT_HORIZONTAL : LEME_SPLIT_VERTICAL;
}

struct leme_layout_node *leme_layout_insert(struct leme_layout_node *root,
                                            struct leme_view *focused,
                                            struct leme_view *added,
                                            double ratio,
                                            struct leme_box area) {
  struct leme_layout_node *old_leaf;
  struct leme_layout_node *retained_leaf;
  struct leme_layout_node *new_leaf;
  struct leme_view *retained_view;
  enum leme_split split;

  if (root == NULL) {
    return leme_layout_leaf(added);
  }
  old_leaf = leme_layout_find(root, focused);
  if (old_leaf == NULL) {
    old_leaf = leme_layout_first(root);
  }
  retained_view = old_leaf->data.view;
  split = leme_layout_split_for(old_leaf, area);
  retained_leaf = leme_layout_leaf(retained_view);
  new_leaf = leme_layout_leaf(added);
  if (retained_leaf == NULL || new_leaf == NULL) {
    free(retained_leaf);
    free(new_leaf);
    return root;
  }
  old_leaf->is_leaf = false;
  old_leaf->data.branch.first = retained_leaf;
  old_leaf->data.branch.second = new_leaf;
  old_leaf->data.branch.split = split;
  old_leaf->data.branch.ratio = ratio;
  retained_leaf->parent = old_leaf;
  new_leaf->parent = old_leaf;
  return root;
}

static struct leme_layout_node *
leme_layout_remove(struct leme_layout_node *root, struct leme_view *removed) {
  struct leme_layout_node *node = leme_layout_find(root, removed);
  struct leme_layout_node *parent;
  struct leme_layout_node *sibling;

  if (node == NULL) {
    return root;
  }
  if (node == root) {
    free(node);
    return NULL;
  }
  if (node->parent == NULL) {
    return root;
  }
  parent = node->parent;
  sibling = parent->data.branch.first == node ? parent->data.branch.second
                                              : parent->data.branch.first;
  if (parent->parent == NULL) {
    sibling->parent = NULL;
    root = sibling;
  } else {
    sibling->parent = parent->parent;
    if (parent->parent->data.branch.first == parent) {
      parent->parent->data.branch.first = sibling;
    } else {
      parent->parent->data.branch.second = sibling;
    }
  }
  free(node);
  free(parent);
  return root;
}

static void leme_layout_arrange_node(struct leme_layout_node *node,
                                     struct leme_box box, int gap,
                                     leme_layout_apply_fn apply, void *data) {
  struct leme_box first = box;
  struct leme_box second = box;
  int split_gap;
  int available;
  int first_size;

  if (node == NULL) {
    return;
  }
  if (node->is_leaf) {
    node->arranged = box;
    apply(node->data.view, box, data);
    return;
  }
  if (node->data.branch.split == LEME_SPLIT_HORIZONTAL) {
    split_gap = gap <= box.width - 2 ? gap : 0;
    available = box.width - split_gap;
    node->data.branch.axis_length = available;
    node->data.branch.split_gap = split_gap;
    first_size = (int)(available * node->data.branch.ratio);
    if (available < 2) {
      first_size = 1;
    } else if (first_size < 1) {
      first_size = 1;
    } else if (first_size >= available) {
      first_size = available - 1;
    }
    first.width = first_size;
    second.x += available < 2 ? 0 : first.width + split_gap;
    second.width = available < 2 ? 1 : available - first.width;
  } else {
    split_gap = gap <= box.height - 2 ? gap : 0;
    available = box.height - split_gap;
    node->data.branch.axis_length = available;
    node->data.branch.split_gap = split_gap;
    first_size = (int)(available * node->data.branch.ratio);
    if (available < 2) {
      first_size = 1;
    } else if (first_size < 1) {
      first_size = 1;
    } else if (first_size >= available) {
      first_size = available - 1;
    }
    first.height = first_size;
    second.y += available < 2 ? 0 : first.height + split_gap;
    second.height = available < 2 ? 1 : available - first.height;
  }
  leme_layout_arrange_node(node->data.branch.first, first, gap, apply, data);
  leme_layout_arrange_node(node->data.branch.second, second, gap, apply, data);
}

void leme_layout_arrange(struct leme_layout_node *root, struct leme_box area,
                         int gap, leme_layout_apply_fn apply, void *data) {
  leme_layout_arrange_node(root, area, gap, apply, data);
}

static double leme_layout_min_length(const struct leme_layout_node *node,
                                     enum leme_split split,
                                     leme_layout_min_fn min_length) {
  if (node->is_leaf) {
    return min_length(node->data.view, split);
  }
  const double first =
      leme_layout_min_length(node->data.branch.first, split, min_length);
  const double second =
      leme_layout_min_length(node->data.branch.second, split, min_length);

  if (node->data.branch.split != split) {
    return fmax(first, second);
  }
  const double ratio = node->data.branch.ratio;

  return node->data.branch.split_gap +
         ceil(fmax(first / ratio, second / (1.0 - ratio)));
}

static void leme_layout_ratio_bounds(const struct leme_layout_node *branch,
                                     leme_layout_min_fn min_length,
                                     double *lowest, double *highest) {
  const double length = branch->data.branch.axis_length;
  const double ratio = branch->data.branch.ratio;
  const double first = leme_layout_min_length(
      branch->data.branch.first, branch->data.branch.split, min_length);
  const double second = leme_layout_min_length(
      branch->data.branch.second, branch->data.branch.split, min_length);

  *lowest = fmin(fmax(0.10, (first + 0.5) / length), ratio);
  *highest = fmax(fmin(0.90, (length - second + 0.5) / length), ratio);
}

bool leme_layout_resize_focused(struct leme_layout_node *root,
                                struct leme_view *focused,
                                enum leme_resize_edge edge, int delta,
                                leme_layout_min_fn min_length) {
  struct leme_layout_node *node = leme_layout_find(root, focused);
  enum leme_split split;
  bool needs_first;
  double direction;

  if (node == NULL || delta == 0) {
    return false;
  }
  split = edge == LEME_RESIZE_LEFT || edge == LEME_RESIZE_RIGHT
              ? LEME_SPLIT_HORIZONTAL
              : LEME_SPLIT_VERTICAL;
  needs_first = edge == LEME_RESIZE_RIGHT || edge == LEME_RESIZE_DOWN;
  direction = needs_first ? 1.0 : -1.0;
  while (node->parent != NULL) {
    struct leme_layout_node *parent = node->parent;
    bool is_first = parent->data.branch.first == node;

    if (parent->data.branch.split == split && is_first == needs_first &&
        parent->data.branch.axis_length > 0) {
      double lowest;
      double highest;

      leme_layout_ratio_bounds(parent, min_length, &lowest, &highest);
      parent->data.branch.ratio = fmin(
          fmax(parent->data.branch.ratio +
                   direction * (double)delta / parent->data.branch.axis_length,
               lowest),
          highest);
      return true;
    }
    node = parent;
  }
  return false;
}

static bool leme_layout_direction_valid(enum leme_direction direction) {
  return direction >= LEME_DIRECTION_LEFT && direction <= LEME_DIRECTION_DOWN;
}

struct leme_layout_detach *
leme_layout_detach_view(struct leme_layout_node **root,
                        struct leme_view *view) {
  struct leme_layout_detach *detach;
  struct leme_layout_node *leaf;
  struct leme_layout_node *branch;
  struct leme_layout_node *sibling;
  struct leme_layout_node *grandparent;

  if (root == NULL || *root == NULL || view == NULL) {
    return NULL;
  }
  leaf = leme_layout_find(*root, view);
  if (leaf == NULL) {
    return NULL;
  }
  detach = calloc(1, sizeof(*detach));
  if (detach == NULL) {
    return NULL;
  }
  detach->leaf = leaf;
  if (leaf == *root) {
    *root = NULL;
    return detach;
  }
  branch = leaf->parent;
  if (branch == NULL || branch->is_leaf) {
    free(detach);
    return NULL;
  }
  sibling = branch->data.branch.first == leaf ? branch->data.branch.second
                                              : branch->data.branch.first;
  grandparent = branch->parent;
  detach->branch = branch;
  detach->sibling = sibling;
  detach->grandparent = grandparent;
  if (grandparent == NULL) {
    *root = sibling;
  } else if (grandparent->data.branch.first == branch) {
    detach->branch_was_first = true;
    grandparent->data.branch.first = sibling;
  } else if (grandparent->data.branch.second == branch) {
    grandparent->data.branch.second = sibling;
  } else {
    free(detach);
    return NULL;
  }
  sibling->parent = grandparent;
  return detach;
}

bool leme_layout_restore_view(struct leme_layout_node **root,
                              struct leme_layout_detach **detach_ptr) {
  struct leme_layout_detach *detach;

  if (root == NULL || detach_ptr == NULL || *detach_ptr == NULL) {
    return false;
  }
  detach = *detach_ptr;
  if (detach->branch == NULL) {
    if (*root != NULL) {
      return false;
    }
    detach->leaf->parent = NULL;
    *root = detach->leaf;
  } else if (detach->grandparent == NULL) {
    if (*root != detach->sibling) {
      return false;
    }
    *root = detach->branch;
    detach->branch->parent = NULL;
    detach->sibling->parent = detach->branch;
    detach->leaf->parent = detach->branch;
  } else {
    struct leme_layout_node **slot =
        detach->branch_was_first ? &detach->grandparent->data.branch.first
                                 : &detach->grandparent->data.branch.second;

    if (*slot != detach->sibling) {
      return false;
    }
    *slot = detach->branch;
    detach->branch->parent = detach->grandparent;
    detach->sibling->parent = detach->branch;
    detach->leaf->parent = detach->branch;
  }
  free(detach);
  *detach_ptr = NULL;
  return true;
}

bool leme_layout_insert_detached(struct leme_layout_node **root,
                                 struct leme_layout_detach **detach_ptr,
                                 struct leme_view *target,
                                 enum leme_direction direction) {
  struct leme_layout_detach *detach;
  struct leme_layout_node *target_leaf;
  struct leme_layout_node *target_parent;
  struct leme_layout_node *branch;
  bool dragged_first;

  if (root == NULL || detach_ptr == NULL || *detach_ptr == NULL ||
      target == NULL || !leme_layout_direction_valid(direction)) {
    return false;
  }
  detach = *detach_ptr;
  if (*root == NULL) {
    return false;
  }
  target_leaf = leme_layout_find(*root, target);
  if (target_leaf == NULL || target_leaf == detach->leaf) {
    return false;
  }
  target_parent = target_leaf->parent;
  if (target_parent == NULL
          ? *root != target_leaf
          : (target_parent->data.branch.first != target_leaf &&
             target_parent->data.branch.second != target_leaf)) {
    return false;
  }
  branch = detach->branch;
  if (branch == NULL) {
    branch = calloc(1, sizeof(*branch));
    if (branch == NULL) {
      return false;
    }
  }
  if (target_parent == NULL) {
    *root = branch;
  } else if (target_parent->data.branch.first == target_leaf) {
    target_parent->data.branch.first = branch;
  } else {
    target_parent->data.branch.second = branch;
  }
  dragged_first =
      direction == LEME_DIRECTION_LEFT || direction == LEME_DIRECTION_UP;
  branch->parent = target_parent;
  branch->data.branch.first = dragged_first ? detach->leaf : target_leaf;
  branch->data.branch.second = dragged_first ? target_leaf : detach->leaf;
  branch->data.branch.split =
      direction == LEME_DIRECTION_LEFT || direction == LEME_DIRECTION_RIGHT
          ? LEME_SPLIT_HORIZONTAL
          : LEME_SPLIT_VERTICAL;
  branch->data.branch.ratio = 0.5;
  branch->data.branch.axis_length = 0;
  branch->data.branch.first->parent = branch;
  branch->data.branch.second->parent = branch;
  free(detach);
  *detach_ptr = NULL;
  return true;
}

void leme_layout_discard_detached(struct leme_layout_detach **detach_ptr) {
  struct leme_layout_detach *detach;

  if (detach_ptr == NULL || *detach_ptr == NULL) {
    return;
  }
  detach = *detach_ptr;
  free(detach->leaf);
  free(detach->branch);
  free(detach);
  *detach_ptr = NULL;
}

struct leme_view *
leme_layout_nearest_candidate(const struct leme_layout_candidate *candidates,
                              size_t count, double layout_x, double layout_y) {
  struct leme_view *best = NULL;
  long double best_distance = LDBL_MAX;
  size_t index;

  if (candidates == NULL || count == 0) {
    return NULL;
  }
  for (index = 0; index < count; index++) {
    const struct leme_box *box = &candidates[index].box;
    long double right = (long double)box->x + box->width;
    long double bottom = (long double)box->y + box->height;

    if (candidates[index].view != NULL && box->width > 0 && box->height > 0 &&
        layout_x >= box->x && layout_x < right && layout_y >= box->y &&
        layout_y < bottom) {
      return candidates[index].view;
    }
  }
  for (index = 0; index < count; index++) {
    const struct leme_box *box = &candidates[index].box;
    long double center_x;
    long double center_y;
    long double dx;
    long double dy;
    long double distance;

    if (candidates[index].view == NULL || box->width <= 0 || box->height <= 0) {
      continue;
    }
    center_x = (long double)box->x + (long double)box->width / 2.0L;
    center_y = (long double)box->y + (long double)box->height / 2.0L;
    dx = center_x - (long double)layout_x;
    dy = center_y - (long double)layout_y;
    distance = dx * dx + dy * dy;
    if (distance < best_distance) {
      best_distance = distance;
      best = candidates[index].view;
    }
  }
  return best;
}

bool leme_layout_drop_at(enum leme_drop_mode mode, struct leme_box content,
                         double layout_x, double layout_y,
                         struct leme_layout_drop *drop) {
  int first_width;
  int first_height;

  if (drop == NULL || content.width <= 0 || content.height <= 0 ||
      (mode != LEME_DROP_MODE_SIMPLE && mode != LEME_DROP_MODE_EDGES)) {
    return false;
  }
  *drop = (struct leme_layout_drop){.box = content};
  first_width = content.width / 2;
  first_height = content.height / 2;
  if (mode == LEME_DROP_MODE_SIMPLE) {
    if (content.width >= content.height) {
      drop->direction = layout_x < content.x + (double)content.width / 2.0
                            ? LEME_DIRECTION_LEFT
                            : LEME_DIRECTION_RIGHT;
    } else {
      drop->direction = layout_y < content.y + (double)content.height / 2.0
                            ? LEME_DIRECTION_UP
                            : LEME_DIRECTION_DOWN;
    }
  } else {
    double distances[] = {
        fabs(layout_x - content.x),
        fabs(layout_x - ((double)content.x + content.width)),
        fabs(layout_y - content.y),
        fabs(layout_y - ((double)content.y + content.height)),
    };
    size_t best = 0;
    size_t index;

    for (index = 1; index < LEME_ARRAY_LENGTH(distances); index++) {
      if (distances[index] < distances[best]) {
        best = index;
      }
    }
    drop->direction = (enum leme_direction)best;
  }
  switch (drop->direction) {
  case LEME_DIRECTION_LEFT:
    drop->box.width = first_width;
    break;
  case LEME_DIRECTION_RIGHT:
    drop->box.x += first_width;
    drop->box.width = content.width - first_width;
    break;
  case LEME_DIRECTION_UP:
    drop->box.height = first_height;
    break;
  case LEME_DIRECTION_DOWN:
    drop->box.y += first_height;
    drop->box.height = content.height - first_height;
    break;
  }
  return true;
}

bool leme_layout_resize_drag_begin(struct leme_layout_node *root,
                                   struct leme_view *view,
                                   enum leme_grab_edge edges,
                                   leme_layout_min_fn min_length,
                                   struct leme_layout_resize_drag *drag) {
  const enum leme_grab_edge allowed =
      LEME_GRAB_EDGE_TOP | LEME_GRAB_EDGE_BOTTOM | LEME_GRAB_EDGE_LEFT |
      LEME_GRAB_EDGE_RIGHT;
  struct leme_layout_node *node;

  if (drag == NULL) {
    return false;
  }
  memset(drag, 0, sizeof(*drag));
  if (root == NULL || view == NULL || edges == LEME_GRAB_EDGE_NONE ||
      (edges & ~allowed) != 0 ||
      ((edges & LEME_GRAB_EDGE_TOP) != 0 &&
       (edges & LEME_GRAB_EDGE_BOTTOM) != 0) ||
      ((edges & LEME_GRAB_EDGE_LEFT) != 0 &&
       (edges & LEME_GRAB_EDGE_RIGHT) != 0)) {
    return false;
  }
  node = leme_layout_find(root, view);
  if (node == NULL) {
    return false;
  }
  while (node->parent != NULL) {
    struct leme_layout_node *parent = node->parent;
    bool is_first = parent->data.branch.first == node;
    bool horizontal_match = ((edges & LEME_GRAB_EDGE_LEFT) != 0 && !is_first) ||
                            ((edges & LEME_GRAB_EDGE_RIGHT) != 0 && is_first);
    bool vertical_match = ((edges & LEME_GRAB_EDGE_TOP) != 0 && !is_first) ||
                          ((edges & LEME_GRAB_EDGE_BOTTOM) != 0 && is_first);

    if (parent->data.branch.axis_length > 0 && horizontal_match &&
        parent->data.branch.split == LEME_SPLIT_HORIZONTAL &&
        drag->horizontal == NULL) {
      drag->horizontal = parent;
      drag->horizontal_ratio = parent->data.branch.ratio;
      drag->horizontal_length = parent->data.branch.axis_length;
      leme_layout_ratio_bounds(parent, min_length, &drag->horizontal_lowest,
                               &drag->horizontal_highest);
    } else if (parent->data.branch.axis_length > 0 && vertical_match &&
               parent->data.branch.split == LEME_SPLIT_VERTICAL &&
               drag->vertical == NULL) {
      drag->vertical = parent;
      drag->vertical_ratio = parent->data.branch.ratio;
      drag->vertical_length = parent->data.branch.axis_length;
      leme_layout_ratio_bounds(parent, min_length, &drag->vertical_lowest,
                               &drag->vertical_highest);
    }
    node = parent;
  }
  return drag->horizontal != NULL || drag->vertical != NULL;
}

static double leme_layout_clamp_ratio(double ratio) {
  if (ratio < 0.10) {
    return 0.10;
  }
  if (ratio > 0.90) {
    return 0.90;
  }
  return ratio;
}

bool leme_layout_resize_drag_update(struct leme_layout_resize_drag *drag,
                                    int dx, int dy) {
  bool changed = false;

  if (drag == NULL) {
    return false;
  }
  if (drag->horizontal != NULL && drag->horizontal_length > 0) {
    double ratio =
        fmin(fmax(drag->horizontal_ratio + (double)dx / drag->horizontal_length,
                  drag->horizontal_lowest),
             drag->horizontal_highest);

    changed |= drag->horizontal->data.branch.ratio != ratio;
    drag->horizontal->data.branch.ratio = ratio;
  }
  if (drag->vertical != NULL && drag->vertical_length > 0) {
    double ratio =
        fmin(fmax(drag->vertical_ratio + (double)dy / drag->vertical_length,
                  drag->vertical_lowest),
             drag->vertical_highest);

    changed |= drag->vertical->data.branch.ratio != ratio;
    drag->vertical->data.branch.ratio = ratio;
  }
  return changed;
}

struct leme_view *leme_layout_directional_candidate(
    const struct leme_layout_candidate *candidates, size_t count,
    const struct leme_view *origin, enum leme_direction direction) {
  int64_t origin_x = 0;
  int64_t origin_y = 0;
  long double best_distance = LDBL_MAX;
  struct leme_view *best = NULL;
  size_t index;

  if (candidates == NULL || count == 0 ||
      !leme_layout_direction_valid(direction)) {
    return NULL;
  }
  if (origin == NULL) {
    return candidates[0].view;
  }
  for (index = 0; index < count; index++) {
    if (candidates[index].view == origin) {
      origin_x =
          (int64_t)candidates[index].box.x * 2 + candidates[index].box.width;
      origin_y =
          (int64_t)candidates[index].box.y * 2 + candidates[index].box.height;
      break;
    }
  }
  if (index == count) {
    return NULL;
  }
  for (index = 0; index < count; index++) {
    int64_t candidate_x;
    int64_t candidate_y;
    int64_t dx;
    int64_t dy;
    long double distance;
    bool qualifies;

    if (candidates[index].view == origin) {
      continue;
    }
    candidate_x =
        (int64_t)candidates[index].box.x * 2 + candidates[index].box.width;
    candidate_y =
        (int64_t)candidates[index].box.y * 2 + candidates[index].box.height;
    dx = candidate_x - origin_x;
    dy = candidate_y - origin_y;
    switch (direction) {
    case LEME_DIRECTION_LEFT:
      qualifies = dx < 0 && llabs(dy) <= -dx;
      break;
    case LEME_DIRECTION_RIGHT:
      qualifies = dx > 0 && llabs(dy) <= dx;
      break;
    case LEME_DIRECTION_UP:
      qualifies = dy < 0 && llabs(dx) <= -dy;
      break;
    case LEME_DIRECTION_DOWN:
      qualifies = dy > 0 && llabs(dx) <= dy;
      break;
    default:
      qualifies = false;
      break;
    }
    if (!qualifies) {
      continue;
    }
    distance = (long double)dx * dx + (long double)dy * dy;
    if (distance < best_distance) {
      best_distance = distance;
      best = candidates[index].view;
    }
  }
  return best;
}

bool leme_layout_swap_views(struct leme_layout_node *root,
                            struct leme_view *first, struct leme_view *second) {
  struct leme_layout_node *first_leaf;
  struct leme_layout_node *second_leaf;

  if (root == NULL || first == NULL || second == NULL || first == second) {
    return false;
  }
  first_leaf = leme_layout_find(root, first);
  second_leaf = leme_layout_find(root, second);
  if (first_leaf == NULL || second_leaf == NULL) {
    return false;
  }
  first_leaf->data.view = second;
  second_leaf->data.view = first;
  return true;
}

static int leme_layout_saturate_origin(int64_t value, int extent) {
  const int64_t limit = (int64_t)INT_MAX - (extent > 0 ? extent : 0);

  if (value < INT_MIN) {
    return INT_MIN;
  }
  return value > limit ? (int)limit : (int)value;
}

static int leme_layout_saturate_dimension(int64_t value) {
  if (value < 1) {
    return 1;
  }
  if (value > INT_MAX) {
    return INT_MAX;
  }
  return (int)value;
}

struct leme_box leme_layout_saturate_box(struct leme_box box) {
  box.x = leme_layout_saturate_origin(box.x, box.width);
  box.y = leme_layout_saturate_origin(box.y, box.height);
  return box;
}

static void leme_layout_inset_axis(int leading, int trailing, int *origin,
                                   int *length) {
  int64_t before = leading;
  int64_t after = trailing;
  const int64_t budget = *length - 1;
  const int64_t total = before + after;

  if (total > budget) {
    before = before * budget / total;
    after = budget - before;
  }
  *length = leme_layout_saturate_dimension((int64_t)*length - before - after);
  *origin = leme_layout_saturate_origin((int64_t)*origin + before, *length);
}

struct leme_box leme_layout_tiled_area(struct leme_box area,
                                       const struct leme_gaps *outer,
                                       bool smart_gaps, size_t tiled_count) {
  if (area.width <= 0 || area.height <= 0) {
    return area;
  }
  area = leme_layout_saturate_box(area);
  if (outer == NULL || (smart_gaps && tiled_count <= 1)) {
    return area;
  }
  leme_layout_inset_axis(outer->left, outer->right, &area.x, &area.width);
  leme_layout_inset_axis(outer->top, outer->bottom, &area.y, &area.height);
  return area;
}

struct leme_box leme_layout_move_box(struct leme_box box,
                                     enum leme_direction direction,
                                     int amount) {
  if (amount <= 0 || !leme_layout_direction_valid(direction)) {
    return box;
  }
  switch (direction) {
  case LEME_DIRECTION_LEFT:
    box.x = leme_layout_saturate_origin((int64_t)box.x - amount, box.width);
    break;
  case LEME_DIRECTION_RIGHT:
    box.x = leme_layout_saturate_origin((int64_t)box.x + amount, box.width);
    break;
  case LEME_DIRECTION_UP:
    box.y = leme_layout_saturate_origin((int64_t)box.y - amount, box.height);
    break;
  case LEME_DIRECTION_DOWN:
    box.y = leme_layout_saturate_origin((int64_t)box.y + amount, box.height);
    break;
  }
  return box;
}

enum leme_grab_edge leme_layout_resize_edges(struct leme_box box,
                                             double cursor_x, double cursor_y) {
  enum leme_grab_edge edges =
      cursor_x < box.width / 2.0 ? LEME_GRAB_EDGE_LEFT : LEME_GRAB_EDGE_RIGHT;

  edges |=
      cursor_y < box.height / 2.0 ? LEME_GRAB_EDGE_TOP : LEME_GRAB_EDGE_BOTTOM;
  return edges;
}

static int64_t leme_layout_clamp_length(int64_t length, int minimum,
                                        int maximum) {
  const int64_t lowest = minimum > 1 ? minimum : 1;

  if (maximum > 0 && length > maximum) {
    length = maximum;
  }
  return length < lowest ? lowest : length;
}

struct leme_box leme_layout_resize_box(struct leme_box box,
                                       enum leme_grab_edge edges, int dx,
                                       int dy,
                                       struct leme_layout_limits limits) {
  const unsigned int allowed = LEME_GRAB_EDGE_TOP | LEME_GRAB_EDGE_BOTTOM |
                               LEME_GRAB_EDGE_LEFT | LEME_GRAB_EDGE_RIGHT;
  int64_t x = box.x;
  int64_t y = box.y;
  int64_t width = box.width;
  int64_t height = box.height;

  if (edges == LEME_GRAB_EDGE_NONE || ((unsigned int)edges & ~allowed) != 0 ||
      (edges & LEME_GRAB_EDGE_LEFT && edges & LEME_GRAB_EDGE_RIGHT) ||
      (edges & LEME_GRAB_EDGE_TOP && edges & LEME_GRAB_EDGE_BOTTOM)) {
    return box;
  }
  if (edges & LEME_GRAB_EDGE_LEFT) {
    int64_t right = x + width;

    width = leme_layout_clamp_length(right - (x + dx), limits.min_width,
                                     limits.max_width);
    x = right - width;
  } else if (edges & LEME_GRAB_EDGE_RIGHT) {
    width = leme_layout_clamp_length(width + dx, limits.min_width,
                                     limits.max_width);
  }
  if (edges & LEME_GRAB_EDGE_TOP) {
    int64_t bottom = y + height;

    height = leme_layout_clamp_length(bottom - (y + dy), limits.min_height,
                                      limits.max_height);
    y = bottom - height;
  } else if (edges & LEME_GRAB_EDGE_BOTTOM) {
    height = leme_layout_clamp_length(height + dy, limits.min_height,
                                      limits.max_height);
  }
  box.width = leme_layout_saturate_dimension(width);
  box.height = leme_layout_saturate_dimension(height);
  box.x = leme_layout_saturate_origin(x, box.width);
  box.y = leme_layout_saturate_origin(y, box.height);
  return box;
}

void leme_layout_destroy(struct leme_layout_node *root) {
  if (root != NULL && !root->is_leaf) {
    leme_layout_destroy(root->data.branch.first);
    leme_layout_destroy(root->data.branch.second);
  }
  free(root);
}

void leme_layout_init(struct leme_layout *layout, enum leme_layout_kind kind) {
  *layout = (struct leme_layout){
      .kind = kind,
      .root = NULL,
      .split_ratio = 0.5,
      .mfact = 0.5,
      .nmaster = 1,
      .gap = 0,
      .has_gap = false,
  };
}

void leme_layout_finish(struct leme_layout *layout) {
  leme_layout_destroy(layout->root);
  layout->root = NULL;
}

static struct leme_layout_node *
leme_layout_clone(const struct leme_layout_node *source,
                  struct leme_layout_node *parent) {
  if (source == NULL) {
    return NULL;
  }
  struct leme_layout_node *node = calloc(1, sizeof(*node));
  if (node == NULL) {
    return NULL;
  }
  node->parent = parent;
  node->is_leaf = source->is_leaf;
  node->arranged = source->arranged;
  if (node->is_leaf) {
    node->data.view = source->data.view;
    return node;
  }
  node->data.branch.split = source->data.branch.split;
  node->data.branch.ratio = source->data.branch.ratio;
  node->data.branch.axis_length = source->data.branch.axis_length;
  node->data.branch.first = leme_layout_clone(source->data.branch.first, node);
  node->data.branch.second =
      leme_layout_clone(source->data.branch.second, node);
  if (node->data.branch.first == NULL || node->data.branch.second == NULL) {
    leme_layout_destroy(node);
    return NULL;
  }
  return node;
}

bool leme_layout_prepare_insert(const struct leme_layout *current,
                                struct leme_view *target,
                                struct leme_view *added,
                                enum leme_direction direction,
                                struct leme_layout *out) {
  if (current == NULL || out == NULL || target == NULL || added == NULL ||
      target == added || direction < LEME_DIRECTION_LEFT ||
      direction > LEME_DIRECTION_DOWN || current->kind < LEME_LAYOUT_DWINDLE ||
      current->kind > LEME_LAYOUT_MASTER_STACK) {
    return false;
  }
  struct leme_layout prepared = *current;
  prepared.root = NULL;
  if (current->kind == LEME_LAYOUT_DWINDLE) {
    prepared.root = leme_layout_clone(current->root, NULL);
    struct leme_layout_node *leaf = leme_layout_find(prepared.root, target);
    if (leaf == NULL || leme_layout_find(prepared.root, added) != NULL) {
      leme_layout_finish(&prepared);
      return false;
    }
    struct leme_layout_node *retained = leme_layout_leaf(target);
    struct leme_layout_node *inserted = leme_layout_leaf(added);
    if (retained == NULL || inserted == NULL) {
      free(retained);
      free(inserted);
      leme_layout_finish(&prepared);
      return false;
    }
    const bool before =
        direction == LEME_DIRECTION_LEFT || direction == LEME_DIRECTION_UP;
    leaf->is_leaf = false;
    leaf->data.branch.first = before ? inserted : retained;
    leaf->data.branch.second = before ? retained : inserted;
    leaf->data.branch.split =
        direction == LEME_DIRECTION_LEFT || direction == LEME_DIRECTION_RIGHT
            ? LEME_SPLIT_HORIZONTAL
            : LEME_SPLIT_VERTICAL;
    leaf->data.branch.ratio = 0.5;
    retained->parent = leaf;
    inserted->parent = leaf;
  }
  *out = prepared;
  return true;
}

bool leme_layout_prepare(const struct leme_layout *current,
                         enum leme_layout_kind kind,
                         struct leme_view *const *views, size_t count,
                         struct leme_layout *out) {
  if (current == NULL || out == NULL || (count > 0 && views == NULL) ||
      kind < LEME_LAYOUT_DWINDLE || kind > LEME_LAYOUT_DESKTOP) {
    return false;
  }
  struct leme_layout prepared = *current;
  prepared.root = NULL;
  prepared.kind = kind;
  if (kind == LEME_LAYOUT_DWINDLE) {
    for (size_t i = 0; i < count; i++) {
      prepared.root =
          leme_layout_insert(prepared.root, i == 0 ? NULL : views[i - 1],
                             views[i], prepared.split_ratio, prepared.area);
      if (leme_layout_find(prepared.root, views[i]) == NULL) {
        leme_layout_finish(&prepared);
        return false;
      }
    }
  }
  *out = prepared;
  return true;
}

void leme_layout_add_view(struct leme_layout *layout, struct leme_view *focused,
                          struct leme_view *added) {
  if (layout->kind == LEME_LAYOUT_DWINDLE) {
    layout->root = leme_layout_insert(layout->root, focused, added,
                                      layout->split_ratio, layout->area);
  }
}

void leme_layout_remove_view(struct leme_layout *layout,
                             struct leme_view *removed) {
  if (layout->kind == LEME_LAYOUT_DWINDLE) {
    layout->root = leme_layout_remove(layout->root, removed);
  }
}

static void leme_layout_arrange_column(struct leme_view *const *views,
                                       size_t count, struct leme_box area,
                                       int gap, leme_layout_apply_fn apply,
                                       void *data) {
  size_t index;
  int available;
  int offset = 0;

  if (count == 0) {
    return;
  }
  if (count > (size_t)area.height) {
    for (index = 0; index < count; index++) {
      const int row =
          index < (size_t)area.height ? (int)index : area.height - 1;
      const struct leme_box box = {area.x, area.y + row, area.width, 1};
      apply(views[index], box, data);
    }
    return;
  }
  const int slots = (int)count;
  if (slots > 1 && gap > (area.height - slots) / (slots - 1)) {
    gap = 0;
  }
  available = area.height - (slots - 1) * gap;
  for (index = 0; index < count; index++) {
    int height = available / slots;
    struct leme_box box;

    if (index + 1 == count) {
      height = available - offset;
    }
    box = (struct leme_box){
        .x = area.x,
        .y = area.y + offset + (int)index * gap,
        .width = area.width,
        .height = height,
    };
    apply(views[index], box, data);
    offset += height;
  }
}

static void leme_layout_arrange_master_stack(
    struct leme_layout *layout, struct leme_view *const *views, size_t count,
    struct leme_box area, int gap, leme_layout_apply_fn apply, void *data) {
  size_t masters = layout->nmaster < 1 ? 1 : layout->nmaster;
  struct leme_box master_area = area;
  struct leme_box stack_area = area;
  int split;

  if (count == 0) {
    return;
  }
  if (masters > count) {
    masters = count;
  }
  if (masters == count) {
    leme_layout_arrange_column(views, count, area, gap, apply, data);
    return;
  }
  const int split_gap = gap <= area.width - 2 ? gap : 0;
  const int available = area.width - split_gap;
  split = (int)((double)available * layout->mfact);
  if (available < 2 || split < 1) {
    split = 1;
  } else if (split >= available) {
    split = available - 1;
  }
  master_area.width = split;
  stack_area.x += available < 2 ? 0 : split + split_gap;
  stack_area.width = available < 2 ? 1 : available - split;
  leme_layout_arrange_column(views, masters, master_area, gap, apply, data);
  leme_layout_arrange_column(&views[masters], count - masters, stack_area, gap,
                             apply, data);
}

void leme_layout_arrange_subject(struct leme_layout *layout,
                                 struct leme_view *const *views, size_t count,
                                 struct leme_box area, int gap,
                                 leme_layout_apply_fn apply, void *data) {
  if (count == 0 || area.width <= 0 || area.height <= 0) {
    return;
  }
  area = leme_layout_saturate_box(area);
  layout->area = area;
  if (layout->has_gap) {
    gap = layout->gap;
  }
  switch (layout->kind) {
  case LEME_LAYOUT_DESKTOP:
    return;
  case LEME_LAYOUT_DWINDLE:
    leme_layout_arrange(layout->root, area, gap, apply, data);
    return;
  case LEME_LAYOUT_MASTER_STACK:
    leme_layout_arrange_master_stack(layout, views, count, area, gap, apply,
                                     data);
    return;
  }
}

bool leme_layout_resize(struct leme_layout *layout, struct leme_view *focused,
                        enum leme_resize_edge edge, int delta,
                        struct leme_box area, leme_layout_min_fn min_length) {
  double step;

  switch (layout->kind) {
  case LEME_LAYOUT_DESKTOP:
    return false;
  case LEME_LAYOUT_DWINDLE:
    return leme_layout_resize_focused(layout->root, focused, edge, delta,
                                      min_length);
  case LEME_LAYOUT_MASTER_STACK:
    if (edge != LEME_RESIZE_LEFT && edge != LEME_RESIZE_RIGHT) {
      return false;
    }
    if (area.width < 1) {
      return false;
    }
    step = (double)delta / area.width;
    layout->mfact = leme_layout_clamp_ratio(edge == LEME_RESIZE_RIGHT
                                                ? layout->mfact + step
                                                : layout->mfact - step);
    return true;
  }
  return false;
}

bool leme_layout_drag_supported(const struct leme_layout *layout) {
  return layout != NULL && layout->kind == LEME_LAYOUT_DWINDLE;
}
