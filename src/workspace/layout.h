#ifndef LEME_LAYOUT_H
#define LEME_LAYOUT_H

#include "core/leme.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum leme_drop_mode {
    LEME_DROP_MODE_SIMPLE,
    LEME_DROP_MODE_EDGES,
};

enum leme_direction {
    LEME_DIRECTION_LEFT,
    LEME_DIRECTION_RIGHT,
    LEME_DIRECTION_UP,
    LEME_DIRECTION_DOWN,
};

enum leme_grab_edge {
    LEME_GRAB_EDGE_NONE = 0,
    LEME_GRAB_EDGE_TOP = 1 << 0,
    LEME_GRAB_EDGE_BOTTOM = 1 << 1,
    LEME_GRAB_EDGE_LEFT = 1 << 2,
    LEME_GRAB_EDGE_RIGHT = 1 << 3,
};

enum leme_split { LEME_SPLIT_HORIZONTAL, LEME_SPLIT_VERTICAL };
enum leme_resize_edge {
    LEME_RESIZE_LEFT,
    LEME_RESIZE_RIGHT,
    LEME_RESIZE_UP,
    LEME_RESIZE_DOWN,
};

enum leme_layout_kind {
    LEME_LAYOUT_DWINDLE,
    LEME_LAYOUT_MASTER_STACK,
    LEME_LAYOUT_ACCORDION,
};

struct leme_layout_node;

struct leme_layout {
    enum leme_layout_kind kind;
    struct leme_layout_node *root;
    double split_ratio;
    double mfact;
    uint16_t nmaster;
    int collapse_width;
    int gap;
    bool has_gap;
};

struct leme_view;

struct leme_layout_candidate {
    struct leme_view *view;
    struct leme_box box;
};

struct leme_layout_drop {
    enum leme_direction direction;
    struct leme_box box;
};

struct leme_layout_node {
    struct leme_layout_node *parent;
    union {
        struct {
            struct leme_layout_node *first;
            struct leme_layout_node *second;
            enum leme_split split;
            double ratio;
            int axis_length;
        } branch;
        struct leme_view *view;
    } data;
    bool is_leaf;
};

struct leme_layout_detach;

struct leme_layout_resize_drag {
    struct leme_layout_node *horizontal;
    struct leme_layout_node *vertical;
    double horizontal_ratio;
    double vertical_ratio;
    int horizontal_length;
    int vertical_length;
};

typedef void (*leme_layout_apply_fn)(struct leme_view *view,
    struct leme_box box, void *data);

struct leme_layout_node *leme_layout_insert(struct leme_layout_node *root,
    struct leme_view *focused, struct leme_view *added);
struct leme_layout_node *leme_layout_remove(struct leme_layout_node *root,
    struct leme_view *removed);
void leme_layout_arrange(struct leme_layout_node *root, struct leme_box area,
    int gap, leme_layout_apply_fn apply, void *data);
struct leme_view *leme_layout_neighbor(const struct leme_layout_node *root,
    const struct leme_view *view);
struct leme_view *leme_layout_directional_candidate(
    const struct leme_layout_candidate *candidates, size_t count,
    const struct leme_view *origin, enum leme_direction direction);
bool leme_layout_swap_views(struct leme_layout_node *root,
    struct leme_view *first, struct leme_view *second);
struct leme_layout_detach *leme_layout_detach_view(
    struct leme_layout_node **root, struct leme_view *view);
bool leme_layout_restore_view(struct leme_layout_node **root,
    struct leme_layout_detach **detach);
bool leme_layout_insert_detached(struct leme_layout_node **root,
    struct leme_layout_detach **detach, struct leme_view *target,
    enum leme_direction direction);
void leme_layout_discard_detached(struct leme_layout_detach **detach);
struct leme_view *leme_layout_nearest_candidate(
    const struct leme_layout_candidate *candidates, size_t count,
    double layout_x, double layout_y);
bool leme_layout_drop_at(enum leme_drop_mode mode,
    struct leme_box content, double layout_x, double layout_y,
    struct leme_layout_drop *drop);
bool leme_layout_resize_drag_begin(struct leme_layout_node *root,
    struct leme_view *view, enum leme_grab_edge edges,
    struct leme_layout_resize_drag *drag);
bool leme_layout_resize_drag_update(
    struct leme_layout_resize_drag *drag, int dx, int dy);
struct leme_box leme_layout_move_box(struct leme_box box,
    enum leme_direction direction, int amount);
enum leme_grab_edge leme_layout_resize_edges(
    struct leme_box box, double cursor_x, double cursor_y);
struct leme_box leme_layout_resize_box(struct leme_box box,
    enum leme_grab_edge edges, int dx, int dy);
bool leme_layout_resize_focused(struct leme_layout_node *root,
    struct leme_view *focused, enum leme_resize_edge edge, int delta);
void leme_layout_destroy(struct leme_layout_node *root);

void leme_layout_init(struct leme_layout *layout, enum leme_layout_kind kind);
void leme_layout_finish(struct leme_layout *layout);
void leme_layout_add_view(struct leme_layout *layout,
    struct leme_view *focused, struct leme_view *added);
void leme_layout_remove_view(struct leme_layout *layout,
    struct leme_view *removed);
void leme_layout_arrange_subject(struct leme_layout *layout,
    struct leme_view *const *views, size_t count,
    const struct leme_view *focused, struct leme_box area, int gap,
    leme_layout_apply_fn apply, void *data);
bool leme_layout_resize(struct leme_layout *layout,
    struct leme_view *focused, enum leme_resize_edge edge, int delta,
    struct leme_box area);
void leme_layout_set_kind(struct leme_layout *layout,
    enum leme_layout_kind kind, struct leme_view *const *views, size_t count);
bool leme_layout_drag_supported(const struct leme_layout *layout);

#endif
