#ifndef LEME_WORKSPACE_EFFECT_H
#define LEME_WORKSPACE_EFFECT_H

#include "config/config.h"
#include "render/animation.h"
#include "workspace/tag.h"

#include <stdbool.h>

struct wlr_scene_tree;

struct leme_workspace_effect;

struct leme_workspace_effect *leme_workspace_effect_create(
    struct wlr_scene_tree *outgoing,
    struct wlr_scene_tree *incoming,
    struct leme_box viewport,
    enum leme_tag_change_direction direction,
    const struct leme_workspace_animation_settings *settings);
void leme_workspace_effect_destroy(struct leme_workspace_effect *effect);
bool leme_workspace_effect_has_content(
    const struct leme_workspace_effect *effect);
struct leme_animation_spec leme_workspace_effect_animation_spec(
    const struct leme_workspace_effect *effect,
    const struct leme_workspace_animation_settings *settings);
void leme_workspace_effect_apply(struct leme_workspace_effect *effect,
    const struct leme_animation_frame *frame);

#endif
