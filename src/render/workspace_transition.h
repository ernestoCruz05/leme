#ifndef LEME_WORKSPACE_TRANSITION_H
#define LEME_WORKSPACE_TRANSITION_H

#include "workspace/tag.h"

#include <stdbool.h>
#include <stdint.h>

struct leme_output;
struct leme_view;
struct leme_workspace_transition;

struct leme_workspace_transition *
leme_render_workspace_transition_prepare(struct leme_output *output,
    uint16_t source_id, uint16_t destination_id,
    enum leme_tag_change_direction direction);
void leme_render_workspace_transition_commit(
    struct leme_workspace_transition *transition);
void leme_render_workspace_transition_finish(struct leme_output *output);
void leme_render_workspace_transition_restack_scratchpad(
    struct leme_output *output);
void leme_render_output_animations_finish(struct leme_output *output);
bool leme_render_workspace_transition_active(
    const struct leme_output *output);
bool leme_render_workspace_transition_hides_view(
    const struct leme_view *view);

#endif
