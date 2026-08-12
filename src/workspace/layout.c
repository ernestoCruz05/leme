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

static struct leme_layout_node *
leme_layout_leaf(struct leme_view *view)
{
    struct leme_layout_node *node = calloc(1, sizeof(*node));

    if (node != NULL) {
        node->is_leaf = true;
        node->data.view = view;
    }
    return node;
}

static struct leme_layout_node *
leme_layout_find(struct leme_layout_node *node, struct leme_view *view)
{
    struct leme_layout_node *found;

    if (node == NULL) {
        return NULL;
    }
    if (node->is_leaf) {
        return node->data.view == view ? node : NULL;
    }
    found = leme_layout_find(node->data.branch.first, view);
    return found != NULL ? found : leme_layout_find(node->data.branch.second, view);
}

static struct leme_layout_node *
leme_layout_first(struct leme_layout_node *node)
{
    while (!node->is_leaf) {
        node = node->data.branch.first;
    }
    return node;
}

static unsigned int
leme_layout_depth(const struct leme_layout_node *node)
{
    unsigned int depth = 0;

    while (node->parent != NULL) {
        depth++;
        node = node->parent;
    }
    return depth;
}

struct leme_layout_node *
leme_layout_insert(struct leme_layout_node *root, struct leme_view *focused,
    struct leme_view *added)
{
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
    split = leme_layout_depth(old_leaf) % 2 == 0 ?
        LEME_SPLIT_HORIZONTAL : LEME_SPLIT_VERTICAL;
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
    old_leaf->data.branch.ratio = 0.5;
    retained_leaf->parent = old_leaf;
    new_leaf->parent = old_leaf;
    return root;
}

struct leme_layout_node *
leme_layout_remove(struct leme_layout_node *root, struct leme_view *removed)
{
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
    sibling = parent->data.branch.first == node ?
        parent->data.branch.second : parent->data.branch.first;
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

static void
leme_layout_arrange_node(struct leme_layout_node *node, struct leme_box box,
    int gap, leme_layout_apply_fn apply, void *data)
{
    struct leme_box first = box;
    struct leme_box second = box;
    int split_gap;
    int available;
    int first_size;

    if (node == NULL) {
        return;
    }
    if (node->is_leaf) {
        apply(node->data.view, box, data);
        return;
    }
    if (node->data.branch.split == LEME_SPLIT_HORIZONTAL) {
        split_gap = gap <= box.width - 2 ? gap : 0;
        available = box.width - split_gap;
        node->data.branch.axis_length = available;
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

void
leme_layout_arrange(struct leme_layout_node *root, struct leme_box area,
    int gap, leme_layout_apply_fn apply, void *data)
{
    leme_layout_arrange_node(root, area, gap, apply, data);
}

bool
leme_layout_resize_focused(struct leme_layout_node *root,
    struct leme_view *focused, enum leme_resize_edge edge, int delta)
{
    struct leme_layout_node *node = leme_layout_find(root, focused);
    enum leme_split split;
    bool needs_first;
    double direction;

    if (node == NULL || delta <= 0) {
        return false;
    }
    split = edge == LEME_RESIZE_LEFT || edge == LEME_RESIZE_RIGHT ?
        LEME_SPLIT_HORIZONTAL : LEME_SPLIT_VERTICAL;
    needs_first = edge == LEME_RESIZE_RIGHT || edge == LEME_RESIZE_DOWN;
    direction = needs_first ? 1.0 : -1.0;
    while (node->parent != NULL) {
        struct leme_layout_node *parent = node->parent;
        bool is_first = parent->data.branch.first == node;

        if (parent->data.branch.split == split && is_first == needs_first &&
                parent->data.branch.axis_length > 0) {
            parent->data.branch.ratio += direction *
                (double)delta / parent->data.branch.axis_length;
            if (parent->data.branch.ratio < 0.1) {
                parent->data.branch.ratio = 0.1;
            } else if (parent->data.branch.ratio > 0.9) {
                parent->data.branch.ratio = 0.9;
            }
            return true;
        }
        node = parent;
    }
    return false;
}

static struct leme_view *
leme_layout_find_neighbor(const struct leme_layout_node *node,
    const struct leme_view *view, bool *found)
{
    struct leme_view *neighbor;

    if (node->is_leaf) {
        if (*found) {
            return node->data.view;
        }
        *found = node->data.view == view;
        return NULL;
    }

    neighbor = leme_layout_find_neighbor(node->data.branch.first, view, found);
    if (neighbor != NULL) {
        return neighbor;
    }
    return leme_layout_find_neighbor(node->data.branch.second, view, found);
}

struct leme_view *
leme_layout_neighbor(const struct leme_layout_node *root,
    const struct leme_view *view)
{
    bool found = false;
    struct leme_view *neighbor;
    const struct leme_layout_node *first;

    if (root == NULL || view == NULL) {
        return NULL;
    }

    neighbor = leme_layout_find_neighbor(root, view, &found);
    if (neighbor != NULL || !found) {
        return neighbor;
    }

    first = root;
    while (!first->is_leaf) {
        first = first->data.branch.first;
    }
    return first->data.view;
}

static bool
leme_layout_direction_valid(enum leme_direction direction)
{
    return direction >= LEME_DIRECTION_LEFT &&
        direction <= LEME_DIRECTION_DOWN;
}

struct leme_layout_detach *
leme_layout_detach_view(
    struct leme_layout_node **root, struct leme_view *view)
{
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
    sibling = branch->data.branch.first == leaf ?
        branch->data.branch.second : branch->data.branch.first;
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

bool
leme_layout_restore_view(struct leme_layout_node **root,
    struct leme_layout_detach **detach_ptr)
{
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
        struct leme_layout_node **slot = detach->branch_was_first ?
            &detach->grandparent->data.branch.first :
            &detach->grandparent->data.branch.second;

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

bool
leme_layout_insert_detached(struct leme_layout_node **root,
    struct leme_layout_detach **detach_ptr, struct leme_view *target,
    enum leme_direction direction)
{
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
    if (target_parent == NULL ? *root != target_leaf :
            (target_parent->data.branch.first != target_leaf &&
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
    dragged_first = direction == LEME_DIRECTION_LEFT ||
        direction == LEME_DIRECTION_UP;
    branch->parent = target_parent;
    branch->data.branch.first = dragged_first ?
        detach->leaf : target_leaf;
    branch->data.branch.second = dragged_first ?
        target_leaf : detach->leaf;
    branch->data.branch.split =
        direction == LEME_DIRECTION_LEFT || direction == LEME_DIRECTION_RIGHT ?
        LEME_SPLIT_HORIZONTAL : LEME_SPLIT_VERTICAL;
    branch->data.branch.ratio = 0.5;
    branch->data.branch.axis_length = 0;
    branch->data.branch.first->parent = branch;
    branch->data.branch.second->parent = branch;
    free(detach);
    *detach_ptr = NULL;
    return true;
}

void
leme_layout_discard_detached(struct leme_layout_detach **detach_ptr)
{
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
leme_layout_nearest_candidate(
    const struct leme_layout_candidate *candidates, size_t count,
    double layout_x, double layout_y)
{
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

        if (candidates[index].view != NULL && box->width > 0 &&
                box->height > 0 && layout_x >= box->x &&
                layout_x < right && layout_y >= box->y &&
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

        if (candidates[index].view == NULL ||
                box->width <= 0 || box->height <= 0) {
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

bool
leme_layout_drop_at(enum leme_drop_mode mode,
    struct leme_box content, double layout_x, double layout_y,
    struct leme_layout_drop *drop)
{
    int first_width;
    int first_height;

    if (drop == NULL || content.width <= 0 || content.height <= 0 ||
            (mode != LEME_DROP_MODE_SIMPLE &&
                mode != LEME_DROP_MODE_EDGES)) {
        return false;
    }
    *drop = (struct leme_layout_drop){.box = content};
    first_width = content.width / 2;
    first_height = content.height / 2;
    if (mode == LEME_DROP_MODE_SIMPLE) {
        if (content.width >= content.height) {
            drop->direction = layout_x <
                content.x + (double)content.width / 2.0 ?
                LEME_DIRECTION_LEFT : LEME_DIRECTION_RIGHT;
        } else {
            drop->direction = layout_y <
                content.y + (double)content.height / 2.0 ?
                LEME_DIRECTION_UP : LEME_DIRECTION_DOWN;
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

bool
leme_layout_resize_drag_begin(struct leme_layout_node *root,
    struct leme_view *view, enum leme_grab_edge edges,
    struct leme_layout_resize_drag *drag)
{
    const enum leme_grab_edge allowed = LEME_GRAB_EDGE_TOP |
        LEME_GRAB_EDGE_BOTTOM | LEME_GRAB_EDGE_LEFT |
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
        bool horizontal_match =
            ((edges & LEME_GRAB_EDGE_LEFT) != 0 && !is_first) ||
            ((edges & LEME_GRAB_EDGE_RIGHT) != 0 && is_first);
        bool vertical_match =
            ((edges & LEME_GRAB_EDGE_TOP) != 0 && !is_first) ||
            ((edges & LEME_GRAB_EDGE_BOTTOM) != 0 && is_first);

        if (parent->data.branch.axis_length > 0 && horizontal_match &&
                parent->data.branch.split == LEME_SPLIT_HORIZONTAL &&
                drag->horizontal == NULL) {
            drag->horizontal = parent;
            drag->horizontal_ratio = parent->data.branch.ratio;
            drag->horizontal_length = parent->data.branch.axis_length;
        } else if (parent->data.branch.axis_length > 0 && vertical_match &&
                parent->data.branch.split == LEME_SPLIT_VERTICAL &&
                drag->vertical == NULL) {
            drag->vertical = parent;
            drag->vertical_ratio = parent->data.branch.ratio;
            drag->vertical_length = parent->data.branch.axis_length;
        }
        node = parent;
    }
    return drag->horizontal != NULL || drag->vertical != NULL;
}

static double
leme_layout_clamp_ratio(double ratio)
{
    if (ratio < 0.10) {
        return 0.10;
    }
    if (ratio > 0.90) {
        return 0.90;
    }
    return ratio;
}

bool
leme_layout_resize_drag_update(
    struct leme_layout_resize_drag *drag, int dx, int dy)
{
    bool changed = false;

    if (drag == NULL) {
        return false;
    }
    if (drag->horizontal != NULL && drag->horizontal_length > 0) {
        double ratio = leme_layout_clamp_ratio(drag->horizontal_ratio +
            (double)dx / drag->horizontal_length);

        changed |= drag->horizontal->data.branch.ratio != ratio;
        drag->horizontal->data.branch.ratio = ratio;
    }
    if (drag->vertical != NULL && drag->vertical_length > 0) {
        double ratio = leme_layout_clamp_ratio(drag->vertical_ratio +
            (double)dy / drag->vertical_length);

        changed |= drag->vertical->data.branch.ratio != ratio;
        drag->vertical->data.branch.ratio = ratio;
    }
    return changed;
}

struct leme_view *
leme_layout_directional_candidate(
    const struct leme_layout_candidate *candidates, size_t count,
    const struct leme_view *origin, enum leme_direction direction)
{
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
            origin_x = (int64_t)candidates[index].box.x * 2 +
                candidates[index].box.width;
            origin_y = (int64_t)candidates[index].box.y * 2 +
                candidates[index].box.height;
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
        candidate_x = (int64_t)candidates[index].box.x * 2 +
            candidates[index].box.width;
        candidate_y = (int64_t)candidates[index].box.y * 2 +
            candidates[index].box.height;
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

bool
leme_layout_swap_views(struct leme_layout_node *root,
    struct leme_view *first, struct leme_view *second)
{
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

static int
leme_layout_saturate_int(int64_t value)
{
    if (value < INT_MIN) {
        return INT_MIN;
    }
    if (value > INT_MAX) {
        return INT_MAX;
    }
    return (int)value;
}

static int
leme_layout_saturate_dimension(int64_t value)
{
    if (value < 1) {
        return 1;
    }
    if (value > INT_MAX) {
        return INT_MAX;
    }
    return (int)value;
}

struct leme_box
leme_layout_move_box(struct leme_box box,
    enum leme_direction direction, int amount)
{
    if (amount <= 0 || !leme_layout_direction_valid(direction)) {
        return box;
    }
    switch (direction) {
    case LEME_DIRECTION_LEFT:
        box.x = leme_layout_saturate_int((int64_t)box.x - amount);
        break;
    case LEME_DIRECTION_RIGHT:
        box.x = leme_layout_saturate_int((int64_t)box.x + amount);
        break;
    case LEME_DIRECTION_UP:
        box.y = leme_layout_saturate_int((int64_t)box.y - amount);
        break;
    case LEME_DIRECTION_DOWN:
        box.y = leme_layout_saturate_int((int64_t)box.y + amount);
        break;
    }
    return box;
}

enum leme_grab_edge
leme_layout_resize_edges(struct leme_box box,
    double cursor_x, double cursor_y)
{
    enum leme_grab_edge edges = cursor_x < box.width / 2.0 ?
        LEME_GRAB_EDGE_LEFT : LEME_GRAB_EDGE_RIGHT;

    edges |= cursor_y < box.height / 2.0 ?
        LEME_GRAB_EDGE_TOP : LEME_GRAB_EDGE_BOTTOM;
    return edges;
}

struct leme_box
leme_layout_resize_box(struct leme_box box,
    enum leme_grab_edge edges, int dx, int dy)
{
    const unsigned int allowed = LEME_GRAB_EDGE_TOP |
        LEME_GRAB_EDGE_BOTTOM | LEME_GRAB_EDGE_LEFT |
        LEME_GRAB_EDGE_RIGHT;
    int64_t x = box.x;
    int64_t y = box.y;
    int64_t width = box.width;
    int64_t height = box.height;

    if (edges == LEME_GRAB_EDGE_NONE ||
            ((unsigned int)edges & ~allowed) != 0 ||
            (edges & LEME_GRAB_EDGE_LEFT &&
                edges & LEME_GRAB_EDGE_RIGHT) ||
            (edges & LEME_GRAB_EDGE_TOP &&
                edges & LEME_GRAB_EDGE_BOTTOM)) {
        return box;
    }
    if (edges & LEME_GRAB_EDGE_LEFT) {
        int64_t right = x + width;

        x += dx;
        width = right - x;
        if (width < 1) {
            width = 1;
            x = right - 1;
        }
    } else if (edges & LEME_GRAB_EDGE_RIGHT) {
        width += dx;
        if (width < 1) {
            width = 1;
        }
    }
    if (edges & LEME_GRAB_EDGE_TOP) {
        int64_t bottom = y + height;

        y += dy;
        height = bottom - y;
        if (height < 1) {
            height = 1;
            y = bottom - 1;
        }
    } else if (edges & LEME_GRAB_EDGE_BOTTOM) {
        height += dy;
        if (height < 1) {
            height = 1;
        }
    }
    box.x = leme_layout_saturate_int(x);
    box.y = leme_layout_saturate_int(y);
    box.width = leme_layout_saturate_dimension(width);
    box.height = leme_layout_saturate_dimension(height);
    return box;
}

void
leme_layout_destroy(struct leme_layout_node *root)
{
    if (root != NULL && !root->is_leaf) {
        leme_layout_destroy(root->data.branch.first);
        leme_layout_destroy(root->data.branch.second);
    }
    free(root);
}

void
leme_layout_init(struct leme_layout *layout, enum leme_layout_kind kind)
{
    *layout = (struct leme_layout){
        .kind = kind,
        .root = NULL,
        .split_ratio = 0.5,
        .mfact = 0.5,
        .nmaster = 1,
        .collapse_width = 46,
        .gap = 0,
        .has_gap = false,
    };
}

void
leme_layout_finish(struct leme_layout *layout)
{
    leme_layout_destroy(layout->root);
    layout->root = NULL;
}

void
leme_layout_add_view(struct leme_layout *layout,
    struct leme_view *focused, struct leme_view *added)
{
    if (layout->kind == LEME_LAYOUT_DWINDLE) {
        layout->root = leme_layout_insert(layout->root, focused, added);
    }
}

void
leme_layout_remove_view(struct leme_layout *layout, struct leme_view *removed)
{
    if (layout->kind == LEME_LAYOUT_DWINDLE) {
        layout->root = leme_layout_remove(layout->root, removed);
    }
}

static void
leme_layout_arrange_column(struct leme_view *const *views, size_t count,
    struct leme_box area, int gap, leme_layout_apply_fn apply, void *data)
{
    size_t index;
    int available;
    int offset = 0;

    if (count == 0) {
        return;
    }
    available = area.height - (int)(count - 1) * gap;
    if (available < (int)count) {
        available = (int)count;
    }
    for (index = 0; index < count; index++) {
        int height = available / (int)count;
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

static void
leme_layout_arrange_master_stack(struct leme_layout *layout,
    struct leme_view *const *views, size_t count, struct leme_box area,
    int gap, leme_layout_apply_fn apply, void *data)
{
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
    split = (int)((double)(area.width - gap) * layout->mfact);
    if (split < 1) {
        split = 1;
    } else if (split > area.width - gap - 1) {
        split = area.width - gap - 1;
    }
    master_area.width = split;
    stack_area.x = area.x + split + gap;
    stack_area.width = area.width - split - gap;
    leme_layout_arrange_column(views, masters, master_area, gap, apply, data);
    leme_layout_arrange_column(&views[masters], count - masters, stack_area,
        gap, apply, data);
}

static void
leme_layout_arrange_accordion(struct leme_layout *layout,
    struct leme_view *const *views, size_t count,
    const struct leme_view *focused, struct leme_box area, int gap,
    leme_layout_apply_fn apply, void *data)
{
    size_t expanded = 0;
    size_t index;
    int collapse = layout->collapse_width;
    int total_gap;
    int expanded_width;
    int offset = 0;

    if (count == 0) {
        return;
    }
    for (index = 0; index < count; index++) {
        if (views[index] == focused) {
            expanded = index;
            break;
        }
    }
    if (collapse < 1) {
        collapse = 1;
    }
    total_gap = (int)(count - 1) * gap;
    while (count > 1 && collapse > 1 &&
            (int)(count - 1) * collapse + total_gap + 1 > area.width) {
        collapse--;
    }
    expanded_width = area.width - (int)(count - 1) * collapse - total_gap;
    if (expanded_width < 1) {
        expanded_width = 1;
    }
    for (index = 0; index < count; index++) {
        int width = index == expanded ? expanded_width : collapse;
        struct leme_box box = {
            .x = area.x + offset + (int)index * gap,
            .y = area.y,
            .width = width,
            .height = area.height < 1 ? 1 : area.height,
        };

        apply(views[index], box, data);
        offset += width;
    }
}

void
leme_layout_arrange_subject(struct leme_layout *layout,
    struct leme_view *const *views, size_t count,
    const struct leme_view *focused, struct leme_box area, int gap,
    leme_layout_apply_fn apply, void *data)
{
    if (layout->has_gap) {
        gap = layout->gap;
    }
    switch (layout->kind) {
    case LEME_LAYOUT_DWINDLE:
        leme_layout_arrange(layout->root, area, gap, apply, data);
        return;
    case LEME_LAYOUT_MASTER_STACK:
        leme_layout_arrange_master_stack(layout, views, count, area, gap,
            apply, data);
        return;
    case LEME_LAYOUT_ACCORDION:
        leme_layout_arrange_accordion(layout, views, count, focused, area,
            gap, apply, data);
        return;
    }
}

bool
leme_layout_resize(struct leme_layout *layout, struct leme_view *focused,
    enum leme_resize_edge edge, int delta, struct leme_box area)
{
    double step;

    switch (layout->kind) {
    case LEME_LAYOUT_DWINDLE:
        return leme_layout_resize_focused(layout->root, focused, edge, delta);
    case LEME_LAYOUT_MASTER_STACK:
        if (edge != LEME_RESIZE_LEFT && edge != LEME_RESIZE_RIGHT) {
            return false;
        }
        if (area.width < 1) {
            return false;
        }
        step = (double)delta / area.width;
        layout->mfact = leme_layout_clamp_ratio(
            edge == LEME_RESIZE_RIGHT ?
                layout->mfact + step : layout->mfact - step);
        return true;
    case LEME_LAYOUT_ACCORDION:
        if (edge != LEME_RESIZE_LEFT && edge != LEME_RESIZE_RIGHT) {
            return false;
        }
        {
            int width = leme_layout_saturate_int(
                (int64_t)layout->collapse_width +
                (edge == LEME_RESIZE_RIGHT ? (int64_t)delta : -(int64_t)delta));

            if (width < 10) {
                width = 10;
            } else if (width > 400) {
                width = 400;
            }
            layout->collapse_width = width;
        }
        return true;
    }
    return false;
}

void
leme_layout_set_kind(struct leme_layout *layout, enum leme_layout_kind kind,
    struct leme_view *const *views, size_t count)
{
    size_t index;

    if (layout->kind == kind) {
        return;
    }
    leme_layout_destroy(layout->root);
    layout->root = NULL;
    layout->kind = kind;
    if (kind != LEME_LAYOUT_DWINDLE) {
        return;
    }
    for (index = 0; index < count; index++) {
        layout->root = leme_layout_insert(layout->root,
            index == 0 ? NULL : views[index - 1], views[index]);
    }
}

bool
leme_layout_drag_supported(const struct leme_layout *layout)
{
    return layout != NULL && layout->kind == LEME_LAYOUT_DWINDLE;
}
