#ifndef LEME_TAG_H
#define LEME_TAG_H

#include "core/leme.h"
#include "workspace/layout.h"

#include <wayland-server-core.h>

struct leme_config;
struct leme_output;
struct leme_server;
struct leme_view;

struct leme_tags;
struct leme_tag_detach;

struct leme_tags_resize {
    struct leme_tags *tags;
    struct leme_tag **table;
    uint16_t max_tags;
};

/* As alocações que leme_gate_tags_prepare pode fazer falhar. */
enum leme_tags_prepare_checkpoint {
    LEME_TAGS_PREPARE_DETACH_PLAN_ALLOCATION,
    LEME_TAGS_PREPARE_DETACH_REMAINING_ALLOCATION,
    LEME_TAGS_PREPARE_MATERIALIZE_TAG_ALLOCATION,
    LEME_TAGS_PREPARE_RESIZE_ALLOCATION,
};

enum leme_tag_change_direction {
    LEME_TAG_CHANGE_BACKWARD = -1,
    LEME_TAG_CHANGE_FORWARD = 1,
};

struct leme_tag {
    uint16_t id;
    struct leme_tags *owner;
    struct leme_layout layout;
    struct wl_list views;
    struct leme_view *focused_view;
};

struct leme_tags {
    struct leme_server *server;
    struct leme_output *output;
    uint16_t initial_tags;
    uint16_t max_tags;
    uint16_t focused_id;
    bool focused_is_candidate;
    uint16_t previous_id;
    bool previous_is_candidate;
    bool previous_valid;
    enum leme_tag_change_direction last_change_direction;
    bool last_change_direction_valid;
    struct leme_tag **table;
};

bool leme_tags_init(struct leme_tags *tags, uint16_t initial_tags,
    uint16_t max_tags);
void leme_tags_finish(struct leme_tags *tags);
bool leme_tags_set_max(struct leme_tags *tags, uint16_t max_tags);
bool leme_tags_can_set_max(const struct leme_tags *tags, uint16_t max_tags);
bool leme_tags_prepare_set_max(struct leme_tags *tags, uint16_t max_tags,
    struct leme_tags_resize *resize);
void leme_tags_commit_set_max(struct leme_tags_resize *resize);
void leme_tags_discard_set_max(struct leme_tags_resize *resize);
struct leme_tag *leme_tags_focus_id(struct leme_tags *tags, uint16_t id);
struct leme_tag *leme_tags_focus_id_direction(struct leme_tags *tags,
    uint16_t id, enum leme_tag_change_direction direction);
struct leme_tag *leme_tags_step(struct leme_tags *tags,
    enum leme_tag_change_direction direction);
uint16_t leme_tags_adjacent_id(const struct leme_tags *tags,
    enum leme_tag_change_direction direction);
bool leme_tags_focus_last(struct leme_tags *tags);
bool leme_tags_remove_empty(struct leme_tags *tags, uint16_t id);
bool leme_tags_assign_view_to(struct leme_tags *tags,
    struct leme_view *view, uint16_t id);
void leme_tags_assign_view(struct leme_tags *tags, struct leme_view *view);
bool leme_tags_move_view(struct leme_tags *tags,
    struct leme_view *view, uint16_t id);
bool leme_tags_adopt_view(struct leme_tags *destination,
    struct leme_view *view, uint16_t id);
struct leme_view *leme_tags_directional_view(const struct leme_tags *tags,
    const struct leme_view *origin, enum leme_direction direction,
    bool tiled_only);
struct leme_view *leme_tags_pointer_drop_target(
    const struct leme_tags *tags, const struct leme_view *excluded,
    double layout_x, double layout_y);
bool leme_tags_swap_directional(struct leme_tags *tags,
    struct leme_view *view, enum leme_direction direction);
void leme_tags_arrange_current(struct leme_tags *tags,
    struct leme_box usable_box, int gap);
bool leme_tags_prepare_detach(struct leme_view *view,
    struct leme_tag_detach **detach);
void leme_tags_commit_detach(struct leme_view *view,
    struct leme_tag_detach **detach);
void leme_tags_discard_detach(struct leme_tag_detach **detach);
bool leme_tags_prepare_floating_attach(struct leme_tags *tags, uint16_t id);
void leme_tags_attach_floating(struct leme_tags *tags,
    struct leme_view *view, uint16_t id);
void leme_tags_remove_view(struct leme_view *view);
void leme_tags_refresh_visibility(struct leme_tags *tags);
enum leme_layout_kind leme_tags_layout_kind(const struct leme_tags *tags);
bool leme_tags_swap_list_order(struct leme_tag *tag,
    struct leme_view *first, struct leme_view *second);
bool leme_tags_set_layout(struct leme_tags *tags, enum leme_layout_kind kind);
bool leme_tags_cycle_layout(struct leme_tags *tags);
void leme_tags_apply_settings(struct leme_tags *tags,
    const struct leme_config *previous, const struct leme_config *next);
size_t leme_tags_navigable(const struct leme_tags *tags,
    uint16_t *ids, size_t capacity);
#endif
