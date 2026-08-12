#ifndef LEME_WORKSPACE_TRANSITION_INTERNAL_H
#define LEME_WORKSPACE_TRANSITION_INTERNAL_H

#include "config/config.h"
#include "render/workspace_transition.h"

struct leme_workspace_effect;
struct wlr_scene_tree;

struct leme_workspace_transition_ops {
    struct wlr_scene_tree *(*snapshot)(struct wlr_scene_tree *source,
        struct wlr_scene_tree *parent);
    struct leme_workspace_effect *(*effect_create)(
        struct wlr_scene_tree *outgoing,
        struct wlr_scene_tree *incoming,
        struct leme_box viewport,
        enum leme_tag_change_direction direction,
        const struct leme_workspace_animation_settings *settings);
};

struct leme_workspace_transition *
leme_render_workspace_transition_prepare_with_ops(
    struct leme_output *output, uint16_t source_id,
    uint16_t destination_id,
    enum leme_tag_change_direction direction,
    const struct leme_workspace_transition_ops *ops);

#endif
