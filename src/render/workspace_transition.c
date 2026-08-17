#include "render/workspace_transition.h"
#include "render/workspace_transition_internal.h"

#include "core/server.h"
#include "output/output.h"
#include "protocols/session.h"
#include "render/animation.h"
#include "render/render.h"
#include "render/workspace_effect.h"
#include "shell/layer.h"
#include "shell/view.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <wlr/backend/session.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>

struct leme_workspace_transition {
    struct leme_output *output;
    struct wlr_scene_tree *master;
    struct wlr_scene_tree *outgoing;
    struct wlr_scene_tree *incoming;
    struct leme_workspace_effect *effect;
    struct leme_workspace_animation_settings settings;
    uint16_t destination_id;
};

static int
leme_workspace_coordinate_subtract(int left, int right)
{
    const int64_t value = (int64_t)left - (int64_t)right;

    if (value < INT_MIN) {
        return INT_MIN;
    }
    if (value > INT_MAX) {
        return INT_MAX;
    }
    return (int)value;
}

static bool
leme_workspace_view_is_participant(const struct leme_view *view,
    const struct leme_output *output, uint16_t tag_id)
{
    return view != NULL && view->render_tree != NULL && view->mapped &&
        !view->unmanaged && !view->detached && leme_ownership_tag(view) != NULL &&
        leme_ownership_tag(view)->owner != NULL && leme_ownership_tag(view)->owner->output == output &&
        leme_ownership_tag(view)->id == tag_id;
}

static const struct leme_view *
leme_workspace_destination_focus(
    const struct leme_output *output, uint16_t tag_id)
{
    const struct leme_tag *tag;
    const struct leme_view *view;

    if (output == NULL || output->server == NULL ||
            output->server->focused_output != output ||
            leme_layer_keyboard_is_exclusive(output->server) ||
            tag_id == 0 || tag_id > output->tags.max_tags) {
        return NULL;
    }
    tag = output->tags.table[tag_id];
    if (tag == NULL) {
        return NULL;
    }
    if (tag->focused_view != NULL && tag->focused_view->mapped) {
        return tag->focused_view;
    }
    wl_list_for_each(view, &tag->views, tag_link) {
        if (view->mapped) {
            return view;
        }
    }
    return NULL;
}

static bool
leme_workspace_tag_has_fullscreen(const struct leme_output *output,
    uint16_t tag_id)
{
    const struct leme_tag *tag;
    const struct leme_view *view;

    if (tag_id == 0 || tag_id > output->tags.max_tags) {
        return false;
    }
    tag = output->tags.table[tag_id];
    if (tag == NULL) {
        return false;
    }
    wl_list_for_each(view, &tag->views, tag_link) {
        if (view->mapped && view->fullscreen) {
            return true;
        }
    }
    return false;
}

static bool
leme_workspace_tag_is_capturable(const struct leme_output *output,
    uint16_t tag_id)
{
    struct leme_tag *tag;
    struct leme_view *view;

    if (tag_id == 0 || tag_id > output->tags.max_tags) {
        return false;
    }
    tag = output->tags.table[tag_id];
    if (tag == NULL) {
        return true;
    }
    wl_list_for_each(view, &tag->views, tag_link) {
        if (view->mapped && !view->unmanaged && !view->detached &&
                view->render_tree == NULL) {
            return false;
        }
    }
    return true;
}

struct leme_workspace_collection {
    struct wlr_scene_tree *source;
    struct wlr_scene_tree *destination;
};

static bool
leme_workspace_collect_parent(struct leme_output *output,
    uint16_t tag_id, const struct leme_workspace_collection *collection,
    const struct leme_view *active_view,
    const struct leme_workspace_transition_ops *ops)
{
    struct wlr_scene_node *node;

    wl_list_for_each(node, &collection->source->children, link) {
        struct leme_view *view = node->data;
        struct wlr_scene_tree *copy;

        if (!leme_workspace_view_is_participant(view, output, tag_id) ||
                &view->render_tree->node != node) {
            continue;
        }
        copy = ops->snapshot(view->render_tree, collection->destination);
        if (copy == NULL) {
            return false;
        }
        if (view == active_view) {
            leme_render_view_apply_active_snapshot(view, copy);
        }
        wlr_scene_node_set_position(&copy->node,
            leme_workspace_coordinate_subtract(
                copy->node.x, output->full_box.x),
            leme_workspace_coordinate_subtract(
                copy->node.y, output->full_box.y));
    }
    return true;
}

static bool
leme_workspace_collect_tag(struct leme_output *output, uint16_t tag_id,
    struct wlr_scene_tree *composition,
    const struct leme_view *active_view,
    const struct leme_workspace_transition_ops *ops)
{
    if (!leme_workspace_tag_is_capturable(output, tag_id)) {
        return false;
    }
    return leme_workspace_collect_parent(output, tag_id,
            &(struct leme_workspace_collection){
                .source = output->server->scene_tiled,
                .destination = composition,
            }, active_view, ops) &&
        leme_workspace_collect_parent(output, tag_id,
            &(struct leme_workspace_collection){
                .source = output->server->scene_floating,
                .destination = composition,
            }, active_view, ops);
}

static void
leme_workspace_transition_discard(
    struct leme_workspace_transition *transition)
{
    if (transition == NULL) {
        return;
    }
    leme_workspace_effect_destroy(transition->effect);
    if (transition->master != NULL) {
        wlr_scene_node_destroy(&transition->master->node);
    }
    free(transition);
}

static void
leme_workspace_finish_live_view_animations(struct leme_output *output)
{
    struct leme_view *view;

    if (output == NULL || output->server == NULL ||
            output->server->views.next == NULL) {
        return;
    }
    wl_list_for_each(view, &output->server->views, link) {
        if (leme_ownership_tag(view) != NULL && leme_ownership_tag(view)->owner != NULL &&
                leme_ownership_tag(view)->owner->output == output) {
            leme_render_view_finish_animation(view);
        }
    }
}

static bool
leme_workspace_transition_is_eligible(struct leme_output *output,
    uint16_t source_id, uint16_t destination_id,
    enum leme_tag_change_direction direction)
{
    struct leme_server *server;
    const struct leme_workspace_animation_settings *settings;

    if (output == NULL || output->server == NULL) {
        return false;
    }
    server = output->server;
    settings = server->config == NULL ? NULL :
        &server->config->workspace_animation;
    return settings != NULL && settings->configured &&
        settings->duration_ms > 0 && source_id != destination_id &&
        output->tags.table != NULL &&
        source_id > 0 && source_id <= output->tags.max_tags &&
        destination_id > 0 && destination_id <= output->tags.max_tags &&
        !leme_workspace_tag_has_fullscreen(output, source_id) &&
        !leme_workspace_tag_has_fullscreen(output, destination_id) &&
        (direction == LEME_TAG_CHANGE_FORWARD ||
            direction == LEME_TAG_CHANGE_BACKWARD) &&
        output->power_on && output->full_box.width > 0 &&
        output->full_box.height > 0 &&
        (output->wlr_output == NULL || output->wlr_output->enabled) &&
        (server->session_protocols == NULL ||
            !server->session_protocols->locked) &&
        (server->session == NULL || server->session->active) &&
        server->scene_floating != NULL && server->scene_tiled != NULL;
}

struct leme_workspace_transition *
leme_render_workspace_transition_prepare_with_ops(
    struct leme_output *output, uint16_t source_id,
    uint16_t destination_id,
    enum leme_tag_change_direction direction,
    const struct leme_workspace_transition_ops *ops)
{
    struct leme_workspace_transition *transition = NULL;
    struct leme_workspace_transition *previous = output == NULL ? NULL :
        output->workspace_transition;
    const struct leme_view *destination_focus;
    bool previous_finished = previous == NULL;

    if (!leme_workspace_transition_is_eligible(output,
            source_id, destination_id, direction) || ops == NULL ||
            ops->snapshot == NULL || ops->effect_create == NULL) {
        if (output != NULL) {
            leme_render_output_animations_finish(output);
        }
        return NULL;
    }
    destination_focus = leme_workspace_destination_focus(
        output, destination_id);
    if (previous == NULL) {
        leme_render_output_animations_finish(output);
    } else {
        leme_workspace_finish_live_view_animations(output);
    }
    transition = calloc(1, sizeof(*transition));
    if (transition == NULL) {
        goto fail;
    }
    transition->output = output;
    transition->settings = output->server->config->workspace_animation;
    transition->destination_id = destination_id;
    transition->master = wlr_scene_tree_create(
        output->server->scene_floating);
    if (transition->master == NULL) {
        goto fail;
    }
    wlr_scene_node_set_enabled(&transition->master->node, false);
    transition->outgoing = wlr_scene_tree_create(transition->master);
    transition->incoming = wlr_scene_tree_create(transition->master);
    if (transition->outgoing == NULL || transition->incoming == NULL) {
        goto fail;
    }
    if (previous == NULL) {
        if (!leme_workspace_collect_tag(output, source_id,
                transition->outgoing, NULL, ops)) {
            goto fail;
        }
    } else {
        struct wlr_scene_tree *current = ops->snapshot(
            previous->master, transition->outgoing);

        if (current == NULL) {
            goto fail;
        }
        wlr_scene_node_set_position(&current->node,
            leme_workspace_coordinate_subtract(
                current->node.x, output->full_box.x),
            leme_workspace_coordinate_subtract(
                current->node.y, output->full_box.y));
        leme_render_output_animations_finish(output);
        previous_finished = true;
    }
    if (!leme_workspace_collect_tag(output, destination_id,
            transition->incoming, destination_focus, ops)) {
        goto fail;
    }
    transition->effect = ops->effect_create(transition->outgoing,
        transition->incoming, output->full_box, direction,
        &transition->settings);
    if (transition->effect == NULL ||
            !leme_workspace_effect_has_content(transition->effect)) {
        goto fail;
    }
    return transition;

fail:
    if (!previous_finished) {
        leme_render_output_animations_finish(output);
    }
    leme_workspace_transition_discard(transition);
    return NULL;
}

struct leme_workspace_transition *
leme_render_workspace_transition_prepare(struct leme_output *output,
    uint16_t source_id, uint16_t destination_id,
    enum leme_tag_change_direction direction)
{
    static const struct leme_workspace_transition_ops ops = {
        .snapshot = leme_animation_snapshot,
        .effect_create = leme_workspace_effect_create,
    };

    return leme_render_workspace_transition_prepare_with_ops(output,
        source_id, destination_id, direction, &ops);
}

static void
leme_render_workspace_transition_refresh_views(struct leme_output *output)
{
    struct leme_view *view;

    if (output == NULL || output->server == NULL ||
            output->server->views.next == NULL) {
        return;
    }
    wl_list_for_each(view, &output->server->views, link) {
        if (leme_ownership_tag(view) != NULL && leme_ownership_tag(view)->owner != NULL &&
                leme_ownership_tag(view)->owner->output == output) {
            leme_render_view_sync_presentation(view);
        }
    }
}

static void
leme_render_workspace_transition_apply(void *data,
    const struct leme_animation_frame *frame)
{
    struct leme_workspace_transition *transition = data;

    leme_workspace_effect_apply(transition->effect, frame);
}

static void
leme_render_workspace_transition_done(void *data)
{
    struct leme_workspace_transition *transition = data;
    struct leme_output *output = transition->output;

    if (output != NULL && output->workspace_transition == transition) {
        output->workspace_transition = NULL;
    }
    leme_workspace_effect_destroy(transition->effect);
    leme_render_workspace_transition_refresh_views(output);
    free(transition);
}

void
leme_render_workspace_transition_commit(
    struct leme_workspace_transition *transition)
{
    struct leme_animation_subject subject;
    struct leme_animation_spec spec;
    struct leme_animation_frame initial;
    struct leme_output *output;

    if (transition == NULL) {
        return;
    }
    output = transition->output;
    output->workspace_transition = transition;
    wlr_scene_node_set_enabled(&transition->master->node, true);
    leme_render_workspace_transition_refresh_views(output);
    spec = leme_workspace_effect_animation_spec(
        transition->effect, &transition->settings);
    initial = leme_animation_frame_at(&spec, 0.0, 0.0);
    leme_workspace_effect_apply(transition->effect, &initial);
    subject = (struct leme_animation_subject){
        .data = transition,
        .owner = output,
        .apply = leme_render_workspace_transition_apply,
        .done = leme_render_workspace_transition_done,
    };
    leme_animation_run(&output->server->animations,
        transition->master, &spec, &subject);
    if (output->workspace_transition != NULL &&
            output->wlr_output != NULL) {
        wlr_output_schedule_frame(output->wlr_output);
    }
}

void
leme_render_workspace_transition_finish(struct leme_output *output)
{
    if (output == NULL || output->server == NULL ||
            output->workspace_transition == NULL) {
        return;
    }
    leme_animation_manager_finish_data(&output->server->animations,
        output->workspace_transition);
}

void
leme_render_output_animations_finish(struct leme_output *output)
{
    if (output == NULL || output->server == NULL) {
        return;
    }
    leme_workspace_finish_live_view_animations(output);
    leme_animation_manager_finish_owner(
        &output->server->animations, output);
}

bool
leme_render_workspace_transition_active(const struct leme_output *output)
{
    return output != NULL && output->workspace_transition != NULL;
}

bool
leme_render_workspace_transition_hides_view(const struct leme_view *view)
{
    const struct leme_output *output;
    const struct leme_workspace_transition *transition;

    if (view == NULL || view->unmanaged || view->detached ||
            leme_ownership_tag(view) == NULL || leme_ownership_tag(view)->owner == NULL) {
        return false;
    }
    output = leme_ownership_tag(view)->owner->output;
    transition = output == NULL ? NULL : output->workspace_transition;
    return transition != NULL && transition->destination_id == leme_ownership_tag(view)->id;
}
