#ifndef LEME_WORKSPACE_TRANSITION_H
#define LEME_WORKSPACE_TRANSITION_H

#include "workspace/tag.h"

#include <stdbool.h>
#include <stdint.h>

struct leme_output;
struct leme_view;
struct leme_workspace_transition;

struct leme_workspace_transition *leme_render_workspace_transition_prepare(
    struct leme_output *output, uint16_t source_id, uint16_t destination_id,
    enum leme_tag_change_direction direction);
struct leme_workspace_transition *
leme_render_workspace_transition_prepare_gesture(
    struct leme_output *output, uint16_t source_id, uint16_t destination_id,
    enum leme_tag_change_direction direction);
void leme_render_workspace_transition_commit(
    struct leme_workspace_transition *transition);
void leme_render_workspace_transition_begin_gesture(
    struct leme_workspace_transition *transition);
bool leme_render_workspace_transition_set_gesture_bounds(
    struct leme_output *output, double center_position);
bool leme_render_workspace_transition_set_gesture_range(
    struct leme_output *output, bool bounded, double minimum, double maximum);
bool leme_render_workspace_transition_presented_position(
    const struct leme_output *output, double *position);
void leme_render_workspace_transition_finish(struct leme_output *output);
void leme_render_output_animations_finish(struct leme_output *output);
bool leme_render_workspace_transition_active(const struct leme_output *output);
void leme_render_workspace_transition_set_position(struct leme_output *output,
                                                   double position);
void leme_render_workspace_transition_settle(struct leme_output *output,
                                             double from_position,
                                             double to_position,
                                             double initial_velocity);
uint16_t
leme_render_workspace_transition_bound_id(const struct leme_output *output,
                                          bool incoming);
const uint16_t *
leme_render_workspace_transition_ring(const struct leme_output *output,
                                      size_t *ring_count);
bool leme_render_workspace_transition_targets(const struct leme_output *output,
                                              double *from_scalar,
                                              double *to_scalar);
bool leme_render_workspace_transition_hides_view(const struct leme_view *view);

#endif
