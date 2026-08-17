#include "shell/sticky.h"

#include "core/gate.h"
#include "core/server.h"
#include "input/input.h"
#include "output/output.h"
#include "protocols/capture.h"
#include "protocols/publication.h"
#include "render/render.h"
#include "shell/ownership.h"
#include "shell/policy.h"
#include "shell/view.h"
#include "workspace/tag.h"

#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

struct leme_sticky_member {
    struct leme_sticky_group *group;
    struct leme_view *view;
    struct leme_sticky_member *parent;
    struct leme_box anchor_area;
    struct wl_list link;
};

struct leme_sticky_group {
    struct leme_sticky_manager *manager;
    struct leme_view *root;
    struct wlr_scene_tree *scene_tree;
    struct wl_list members;
    struct wl_list link;
    bool pending_attach;
};

struct leme_sticky_adoption {
    struct leme_sticky_member *member;
    struct leme_ownership_transition *owner;
};

struct leme_sticky_output_entry {
    struct leme_sticky_group *group;
    struct leme_view **views;
    struct leme_box *boxes;
    size_t view_count;
    struct leme_ownership_transition *owner;
    struct leme_output *output;
    struct leme_box anchor;
    enum leme_durable_presentation presentation;
    bool attach_pending;
    bool preserve_focus;
};

struct leme_sticky_pending_attach {
    struct leme_sticky_group *group;
    struct leme_view **views;
    struct leme_box *boxes;
    size_t view_count;
    struct leme_ownership_transition *owner;
};

struct leme_sticky_output_plan {
    struct leme_server *server;
    struct leme_sticky_output_entry *entries;
    struct leme_sticky_pending_attach *attachments;
    size_t entry_count;
    size_t attachment_count;
};

static size_t
leme_sticky_group_count(const struct leme_sticky_group *group)
{
    const struct leme_sticky_member *member;
    size_t count = 0;

    wl_list_for_each(member, &group->members, link) {
        count++;
    }
    return count;
}

static bool
leme_sticky_group_views(const struct leme_sticky_group *group,
    struct leme_view ***views, size_t *view_count)
{
    struct leme_sticky_member *member;
    struct leme_view **prepared;
    size_t index = 0;
    size_t count;

    if (group == NULL || views == NULL || *views != NULL ||
            view_count == NULL) {
        return false;
    }
    count = leme_sticky_group_count(group);
    if (count == 0 || count > SIZE_MAX / sizeof(*prepared)) {
        return false;
    }
    prepared = (struct leme_view **)calloc(count, sizeof(*prepared));
    if (prepared == NULL) {
        return false;
    }
    wl_list_for_each(member, &group->members, link) {
        if (index >= count || member->view == NULL) {
            free((void *)prepared);
            return false;
        }
        prepared[index++] = member->view;
    }
    if (index != count) {
        free((void *)prepared);
        return false;
    }
    *views = prepared;
    *view_count = count;
    return true;
}

static struct leme_sticky_member *
leme_sticky_descendant_tail(struct leme_sticky_member *parent)
{
    struct leme_sticky_member *tail = parent;
    struct leme_sticky_member *candidate;

    wl_list_for_each(candidate, &parent->group->members, link) {
        struct leme_sticky_member *ancestor = candidate->parent;

        while (ancestor != NULL && ancestor != parent) {
            ancestor = ancestor->parent;
        }
        if (ancestor == parent) {
            tail = candidate;
        }
    }
    return tail;
}

static struct leme_sticky_group *
leme_sticky_group(const struct leme_view *view)
{
    return view == NULL || view->sticky_member == NULL ? NULL :
        view->sticky_member->group;
}

static void
leme_sticky_hide_pending_group(struct leme_sticky_group *group,
    struct leme_sticky_member *removed)
{
    struct leme_sticky_member *member;

    if (group->manager != NULL && group->manager->server != NULL &&
            leme_sticky_group(group->manager->server->focused_view) == group) {
        leme_view_clear_focus(group->manager->server);
    }
    wl_list_for_each(member, &group->members, link) {
        if (member == removed) {
            continue;
        }
        leme_capture_invalidate_view(member->view);
        (void)leme_ownership_present_durable(member->view,
            LEME_DURABLE_HIDDEN, NULL);
        member->parent = NULL;
        if (member->view->render_tree != NULL) {
            wlr_scene_node_set_enabled(&member->view->render_tree->node,
                false);
        }
    }
    group->root = NULL;
    group->pending_attach = true;
    if (removed != NULL) {
        removed->view->sticky_member = NULL;
        wl_list_remove(&removed->link);
        free(removed);
    }
}

static void
leme_sticky_group_destroy(struct leme_sticky_group *group, bool clear_views)
{
    struct leme_sticky_member *member;
    struct leme_sticky_member *next;

    if (group == NULL) {
        return;
    }
    wl_list_for_each_safe(member, next, &group->members, link) {
        if (clear_views && member->view != NULL) {
            const bool sticky_owner = leme_ownership_is_durable(
                member->view, LEME_DURABLE_STICKY);

            member->view->sticky_member = NULL;
            if (sticky_owner) {
                leme_ownership_clear(member->view);
            }
        }
        wl_list_remove(&member->link);
        free(member);
    }
    if (group->link.next != NULL) {
        wl_list_remove(&group->link);
    }
    leme_render_durable_group_destroy(&group->scene_tree);
    free(group);
}

bool
leme_sticky_init(struct leme_server *server)
{
    if (server == NULL) {
        return false;
    }
    server->sticky = (struct leme_sticky_manager){
        .server = server,
    };
    wl_list_init(&server->sticky.groups);
    return true;
}

void
leme_sticky_finish(struct leme_server *server)
{
    struct leme_sticky_group *group;
    struct leme_sticky_group *next;

    if (server == NULL) {
        return;
    }
    if (server->sticky.groups.next != NULL) {
        wl_list_for_each_safe(group, next, &server->sticky.groups, link) {
            leme_sticky_group_destroy(group, true);
        }
    }
    server->sticky = (struct leme_sticky_manager){0};
    wl_list_init(&server->sticky.groups);
}

bool
leme_view_is_sticky(const struct leme_view *view)
{
    return leme_ownership_is_durable(view, LEME_DURABLE_STICKY) &&
        leme_sticky_group(view) != NULL;
}

bool
leme_sticky_is_dependent(const struct leme_view *view)
{
    const struct leme_sticky_group *group = leme_sticky_group(view);

    return group != NULL && group->root != view;
}

struct leme_view *
leme_sticky_group_root(struct leme_view *view)
{
    struct leme_sticky_group *group = leme_sticky_group(view);

    return group == NULL ? NULL : group->root;
}

struct wlr_scene_tree *
leme_sticky_render_parent(const struct leme_view *view)
{
    const struct leme_sticky_group *group = leme_sticky_group(view);

    return group == NULL ? NULL : group->scene_tree;
}

bool
// The parent and dependent roles are deliberately distinct.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
leme_sticky_prepare_transient(struct leme_view *parent,
    struct leme_view *dependent,
    struct leme_sticky_adoption **slot)
{
    struct leme_sticky_group *group = leme_sticky_group(parent);
    struct leme_sticky_adoption *adoption;
    const struct leme_durable_owner *durable;
    struct leme_output *output;
    enum leme_durable_presentation presentation;

    if (group == NULL || dependent == NULL || slot == NULL || *slot != NULL ||
            leme_ownership_kind(dependent) != LEME_VIEW_OWNER_NONE) {
        return false;
    }
    durable = leme_ownership_durable(parent);
    if (durable == NULL || durable->reason != LEME_DURABLE_STICKY) {
        return false;
    }
    presentation = durable->presentation;
    output = durable->output;
    if ((presentation == LEME_DURABLE_OUTPUT && output == NULL) ||
            (presentation == LEME_DURABLE_HIDDEN && output != NULL)) {
        return false;
    }
    adoption = calloc(1, sizeof(*adoption));
    if (adoption == NULL) {
        return false;
    }
    adoption->member = calloc(1, sizeof(*adoption->member));
    if (adoption->member == NULL ||
            !leme_ownership_prepare_none_to_durable(dependent,
                LEME_DURABLE_STICKY, presentation,
                output, &adoption->owner)) {
        free(adoption->member);
        free(adoption);
        return false;
    }
    adoption->member->group = group;
    adoption->member->view = dependent;
    adoption->member->parent = parent->sticky_member;
    adoption->member->anchor_area = output == NULL ?
        parent->sticky_member->anchor_area : output->usable_box;
    *slot = adoption;
    return true;
}

void
leme_sticky_commit_transient(struct leme_sticky_adoption **slot)
{
    struct leme_sticky_adoption *adoption;
    struct leme_sticky_member *member;

    assert(slot != NULL && *slot != NULL);
    adoption = *slot;
    *slot = NULL;
    member = adoption->member;
    leme_capture_invalidate_view(member->view);
    leme_ownership_commit(&adoption->owner);
    member->view->floating = true;
    member->view->sticky_member = member;
    wl_list_insert(&leme_sticky_descendant_tail(member->parent)->link,
        &member->link);
    if (member->view->render_tree != NULL) {
        const bool visible =
            leme_ownership_effective_output(member->view) != NULL;

        wlr_scene_node_reparent(&member->view->render_tree->node,
            member->group->scene_tree);
        wlr_scene_node_set_enabled(&member->view->render_tree->node,
            visible);
        if (visible) {
            leme_render_view_set_box(member->view, member->view->box);
        }
    }
    leme_publication_invalidate(member->view->server);
    free(adoption);
}

void
leme_sticky_discard_transient(struct leme_sticky_adoption **slot)
{
    struct leme_sticky_adoption *adoption;

    if (slot == NULL || *slot == NULL) {
        return;
    }
    adoption = *slot;
    *slot = NULL;
    leme_ownership_discard(&adoption->owner);
    free(adoption->member);
    free(adoption);
}

static bool
leme_sticky_enter(struct leme_view *view)
{
    struct leme_sticky_manager *manager;
    struct leme_sticky_group *group;
    struct leme_sticky_member *member;
    struct leme_ownership_transition *owner = NULL;
    struct leme_output *output;

    if (view == NULL || view->server == NULL || !view->mapped ||
            view->unmanaged || view->fullscreen ||
            leme_ownership_kind(view) != LEME_VIEW_OWNER_TAG ||
            leme_view_is_scratchpad(view)) {
        return false;
    }
    output = leme_ownership_effective_output(view);
    if (output == NULL || !leme_ownership_prepare_tag_to_durable(view,
            LEME_DURABLE_STICKY, LEME_DURABLE_OUTPUT,
            output, &owner)) {
        return false;
    }
    group = calloc(1, sizeof(*group));
    member = calloc(1, sizeof(*member));
    if (group == NULL || member == NULL) {
        free(group);
        free(member);
        leme_ownership_discard(&owner);
        return false;
    }
    group->scene_tree = leme_render_durable_group_create(view->server);
    if (group->scene_tree == NULL) {
        free(member);
        free(group);
        leme_ownership_discard(&owner);
        return false;
    }
    manager = &view->server->sticky;
    group->manager = manager;
    group->root = view;
    wl_list_init(&group->members);
    member->group = group;
    member->view = view;
    member->anchor_area = leme_output_usable_box(output);

    leme_capture_invalidate_view(view);
    leme_input_pointer_grab_cancel_view(view);
    leme_ownership_commit(&owner);
    view->floating = true;
    view->fullscreen = false;
    view->sticky_member = member;
    wl_list_insert(&group->members, &member->link);
    wl_list_insert(&manager->groups, &group->link);
    if (view->render_tree != NULL) {
        wlr_scene_node_reparent(&view->render_tree->node, group->scene_tree);
    }
    view->box = leme_view_policy_clamp_box(view->box, output->usable_box);
    leme_render_view_set_box(view, view->box);
    leme_publication_invalidate(view->server);
    return true;
}

static struct leme_view *
leme_sticky_group_focused(struct leme_sticky_group *group)
{
    struct leme_view *focused;

    if (group == NULL || group->manager == NULL ||
            group->manager->server == NULL) {
        return NULL;
    }
    focused = group->manager->server->focused_view;
    return focused != NULL && leme_sticky_group(focused) == group ?
        focused : NULL;
}

static bool
leme_sticky_leave(struct leme_view *view)
{
    struct leme_sticky_group *group = leme_sticky_group(view);
    struct leme_output *output = leme_ownership_effective_output(view);
    struct leme_ownership_transition *owner = NULL;
    struct leme_view *focus = leme_sticky_group_focused(group);
    struct leme_view **views = NULL;
    size_t view_count = 0;
    struct leme_tags *tags;

    if (group == NULL || group->root != view || output == NULL) {
        return false;
    }
    tags = &output->tags;
    if (focus == NULL) {
        focus = view;
    }
    if (!leme_sticky_group_views(group, &views, &view_count) ||
            !leme_ownership_prepare_durable_group_to_tag(views,
                view_count, focus, tags, tags->focused_id, &owner)) {
        free((void *)views);
        return false;
    }
    for (size_t index = 0; index < view_count; index++) {
        leme_capture_invalidate_view(views[index]);
    }
    leme_ownership_commit(&owner);
    for (size_t index = 0; index < view_count; index++) {
        views[index]->sticky_member = NULL;
        if (views[index]->render_tree != NULL) {
            wlr_scene_node_reparent(&views[index]->render_tree->node,
                view->server->scene_floating);
        }
    }
    free((void *)views);
    leme_sticky_group_destroy(group, false);
    leme_publication_invalidate(view->server);
    return true;
}

bool
leme_sticky_toggle(struct leme_view *view)
{
    return leme_view_is_sticky(view) ?
        leme_sticky_leave(view) : leme_sticky_enter(view);
}

static int
leme_sticky_delta(int to, int from)
{
    const int64_t result = (int64_t)to - from;

    if (result > INT_MAX) {
        return INT_MAX;
    }
    if (result < INT_MIN) {
        return INT_MIN;
    }
    return (int)result;
}

static int
leme_sticky_add_saturated(int value, int delta)
{
    const int64_t result = (int64_t)value + delta;

    if (result > INT_MAX) {
        return INT_MAX;
    }
    if (result < INT_MIN) {
        return INT_MIN;
    }
    return (int)result;
}

bool
leme_sticky_apply_box(struct leme_view *view,
    struct leme_box box, bool resizing)
{
    struct leme_sticky_group *group = leme_sticky_group(view);
    struct leme_output *output;

    if (group == NULL || !leme_view_is_sticky(view) || view->fullscreen) {
        return false;
    }
    output = leme_ownership_effective_output(view);
    if (output == NULL) {
        return false;
    }
    /*
     * Um movimento pedido pelo utilizador não é preso à área útil: uma vista
     * presa que a preencha por inteiro não teria para onde ir, e uma flutuante
     * na mesma situação move-se à vontade. O reancorar quando a área muda
     * continua a garantir que nenhuma se perde fora do ecrã.
     */
    if (group->root == view) {
        const int delta_x = leme_sticky_delta(box.x, view->box.x);
        const int delta_y = leme_sticky_delta(box.y, view->box.y);
        struct leme_sticky_member *member;

        wl_list_for_each(member, &group->members, link) {
            struct leme_box member_box = member->view == view ? box :
                (struct leme_box){
                    .x = leme_sticky_add_saturated(
                        member->view->box.x, delta_x),
                    .y = leme_sticky_add_saturated(
                        member->view->box.y, delta_y),
                    .width = member->view->box.width,
                    .height = member->view->box.height,
                };

            member->view->box = member_box;
            if (resizing && member->view == view &&
                    view->kind == LEME_VIEW_XDG) {
                leme_view_configure(view,
                    leme_render_view_content_box(view, member_box));
            }
            leme_render_view_set_box(member->view, member_box);
            member->anchor_area = output->usable_box;
        }
        return true;
    }
    view->box = box;
    if (resizing && view->kind == LEME_VIEW_XDG) {
        leme_view_configure(view,
            leme_render_view_content_box(view, box));
    }
    leme_render_view_set_box(view, box);
    view->sticky_member->anchor_area = output->usable_box;
    return true;
}

bool
leme_sticky_move_to_output(struct leme_view *view,
    struct leme_output *output, bool follow)
{
    struct leme_sticky_group *group = leme_sticky_group(view);
    struct leme_ownership_transition *transition = NULL;
    struct leme_output *previous;
    struct leme_view **views = NULL;
    struct leme_box *boxes = NULL;
    size_t view_count = 0;

    if (group == NULL || group->root != view || output == NULL ||
            output->server != view->server) {
        return false;
    }
    previous = leme_ownership_effective_output(view);
    if (previous == NULL) {
        return false;
    }
    if (previous == output) {
        if (follow) {
            leme_output_set_focused(view->server, output, false);
            leme_view_focus(view);
        }
        return true;
    }
    if (!leme_sticky_group_views(group, &views, &view_count) ||
            view_count > SIZE_MAX / sizeof(*boxes)) {
        free((void *)views);
        return false;
    }
    boxes = calloc(view_count, sizeof(*boxes));
    if (boxes == NULL ||
            !leme_ownership_prepare_durable_group_to_durable(views,
                view_count, LEME_DURABLE_OUTPUT, output, &transition)) {
        free(boxes);
        free((void *)views);
        return false;
    }
    for (size_t index = 0; index < view_count; index++) {
        boxes[index] = leme_view_policy_reanchor_box(views[index]->box,
            views[index]->sticky_member->anchor_area, output->usable_box);
        leme_capture_invalidate_view(views[index]);
    }
    leme_ownership_commit(&transition);
    for (size_t index = 0; index < view_count; index++) {
        views[index]->box = boxes[index];
        views[index]->sticky_member->anchor_area = output->usable_box;
        leme_render_view_set_box(views[index], boxes[index]);
    }
    free(boxes);
    free((void *)views);
    if (follow) {
        leme_output_set_focused(view->server, output, false);
        leme_view_focus(view);
    }
    leme_publication_invalidate(view->server);
    return true;
}

bool
leme_sticky_move_to_tag(struct leme_view *view,
    uint16_t tag_id, bool follow)
{
    struct leme_sticky_group *group = leme_sticky_group(view);
    struct leme_output *output = leme_ownership_effective_output(view);
    struct leme_ownership_transition *owner = NULL;
    struct leme_view *focus = leme_sticky_group_focused(group);
    struct leme_view **views = NULL;
    size_t view_count = 0;

    if (group == NULL || group->root != view || output == NULL ||
            tag_id == 0 || tag_id > output->tags.max_tags) {
        return false;
    }
    if (focus == NULL) {
        focus = view;
    }
    if (!leme_sticky_group_views(group, &views, &view_count) ||
            !leme_ownership_prepare_durable_group_to_tag(views,
                view_count, focus, &output->tags, tag_id, &owner)) {
        free((void *)views);
        return false;
    }
    for (size_t index = 0; index < view_count; index++) {
        leme_capture_invalidate_view(views[index]);
    }
    leme_ownership_commit(&owner);
    for (size_t index = 0; index < view_count; index++) {
        views[index]->sticky_member = NULL;
        if (views[index]->render_tree != NULL) {
            wlr_scene_node_reparent(&views[index]->render_tree->node,
                view->server->scene_floating);
        }
    }
    free((void *)views);
    leme_sticky_group_destroy(group, false);
    if (follow) {
        (void)leme_tags_focus_id(&output->tags, tag_id);
        leme_output_set_focused(view->server, output, false);
        leme_view_focus(focus == NULL ? view : focus);
    }
    leme_publication_invalidate(view->server);
    return true;
}

bool
leme_sticky_enter_fullscreen(struct leme_view *view)
{
    struct leme_output *output = leme_ownership_effective_output(view);

    if (!leme_view_is_sticky(view) || leme_sticky_is_dependent(view) ||
            output == NULL) {
        return false;
    }
    if (!leme_sticky_move_to_tag(view, output->tags.focused_id, false)) {
        return false;
    }
    return leme_view_set_fullscreen(view, true);
}

static const struct leme_sticky_output_snapshot *
leme_sticky_snapshot_for(
    const struct leme_sticky_output_snapshot *snapshots,
    size_t snapshot_count, const struct leme_output *output)
{
    for (size_t index = 0; index < snapshot_count; index++) {
        if (snapshots[index].output == output) {
            return &snapshots[index];
        }
    }
    return NULL;
}

static const struct leme_sticky_output_snapshot *
leme_sticky_choose_snapshot(struct leme_server *server,
    const struct leme_sticky_output_snapshot *snapshots,
    size_t snapshot_count, const struct leme_output *current)
{
    const struct leme_sticky_output_snapshot *candidate =
        leme_sticky_snapshot_for(snapshots, snapshot_count, current);

    if (candidate != NULL && candidate->usable) {
        return candidate;
    }
    candidate = leme_sticky_snapshot_for(snapshots, snapshot_count,
        server->focused_output);
    if (candidate != NULL && candidate->usable) {
        return candidate;
    }
    for (size_t index = 0; index < snapshot_count; index++) {
        if (snapshots[index].usable) {
            return &snapshots[index];
        }
    }
    return NULL;
}

static void
leme_sticky_output_entry_finish(struct leme_sticky_output_entry *entry)
{
    leme_ownership_discard(&entry->owner);
    free(entry->boxes);
    free((void *)entry->views);
    *entry = (struct leme_sticky_output_entry){0};
}

bool
leme_sticky_prepare_outputs(struct leme_server *server,
    const struct leme_sticky_output_snapshot *snapshots,
    size_t snapshot_count, struct leme_sticky_output_plan **slot)
{
    struct leme_sticky_output_plan *plan;
    struct leme_sticky_group *group;
    size_t group_count = 0;
    size_t index = 0;

    if (server == NULL || snapshots == NULL || snapshot_count == 0 ||
            slot == NULL || *slot != NULL ||
            (leme_gate_sticky_output_prepare != NULL &&
                !leme_gate_sticky_output_prepare(
                    LEME_STICKY_OUTPUT_PREPARE_PLAN))) {
        return false;
    }
    wl_list_for_each(group, &server->sticky.groups, link) {
        group_count++;
    }
    plan = calloc(1, sizeof(*plan));
    if (plan == NULL) {
        return false;
    }
    plan->server = server;
    if (group_count > 0) {
        if (group_count > SIZE_MAX / sizeof(*plan->entries) ||
                group_count > SIZE_MAX / sizeof(*plan->attachments)) {
            free(plan);
            return false;
        }
        plan->entries = calloc(group_count, sizeof(*plan->entries));
        plan->attachments = calloc(group_count, sizeof(*plan->attachments));
        if (plan->entries == NULL || plan->attachments == NULL) {
            leme_sticky_discard_outputs(&plan);
            return false;
        }
    }
    wl_list_for_each(group, &server->sticky.groups, link) {
        if (index >= group_count) {
            leme_sticky_discard_outputs(&plan);
            return false;
        }
        index++;
        struct leme_output *current = group->root == NULL ? NULL :
            leme_ownership_effective_output(group->root);
        const struct leme_sticky_output_snapshot *destination =
            leme_sticky_choose_snapshot(server, snapshots,
                snapshot_count, current);

        if (leme_gate_sticky_output_prepare != NULL &&
                !leme_gate_sticky_output_prepare(
                    LEME_STICKY_OUTPUT_PREPARE_ENTRY)) {
            leme_sticky_discard_outputs(&plan);
            return false;
        }
        if (group->pending_attach && destination != NULL) {
            struct leme_sticky_pending_attach *attachment =
                &plan->attachments[plan->attachment_count++];

            if (destination->output == NULL ||
                    destination->output->tags.table == NULL ||
                    destination->output->tags.focused_id == 0 ||
                    !leme_sticky_group_views(group, &attachment->views,
                        &attachment->view_count) ||
                    attachment->view_count >
                        SIZE_MAX / sizeof(*attachment->boxes)) {
                leme_sticky_discard_outputs(&plan);
                return false;
            }
            attachment->boxes = calloc(attachment->view_count,
                sizeof(*attachment->boxes));
            if (attachment->boxes == NULL ||
                    !leme_ownership_prepare_durable_group_to_tag(
                        attachment->views, attachment->view_count, NULL,
                        &destination->output->tags,
                        destination->output->tags.focused_id,
                        &attachment->owner)) {
                leme_sticky_discard_outputs(&plan);
                return false;
            }
            attachment->group = group;
            for (size_t view_index = 0;
                    view_index < attachment->view_count; view_index++) {
                struct leme_view *pending = attachment->views[view_index];

                attachment->boxes[view_index] =
                    leme_view_policy_reanchor_box(pending->box,
                        pending->sticky_member->anchor_area,
                        destination->future_full);
            }
            continue;
        }
        struct leme_sticky_output_entry *entry =
            &plan->entries[plan->entry_count++];

        entry->group = group;
        entry->presentation = destination == NULL ?
            LEME_DURABLE_HIDDEN : LEME_DURABLE_OUTPUT;
        entry->output = destination == NULL ? NULL : destination->output;
        entry->anchor = destination == NULL ?
            (struct leme_box){0} : destination->future_full;
        entry->attach_pending = group->pending_attach;
        entry->preserve_focus = group->root != NULL &&
            server->focused_view != NULL &&
            leme_sticky_group(server->focused_view) == group &&
            destination != NULL;
        if (!leme_sticky_group_views(group, &entry->views,
                &entry->view_count) ||
                entry->view_count > SIZE_MAX / sizeof(*entry->boxes)) {
            leme_sticky_discard_outputs(&plan);
            return false;
        }
        entry->boxes = calloc(entry->view_count, sizeof(*entry->boxes));
        if (entry->boxes == NULL ||
                !leme_ownership_prepare_durable_group_to_durable(
                    entry->views, entry->view_count,
                    entry->presentation, entry->output, &entry->owner)) {
            leme_sticky_discard_outputs(&plan);
            return false;
        }
        for (size_t member_index = 0;
                member_index < entry->view_count; member_index++) {
            struct leme_view *view = entry->views[member_index];

            entry->boxes[member_index] = destination == NULL ? view->box :
                leme_view_policy_reanchor_box(view->box,
                    view->sticky_member->anchor_area,
                    destination->future_full);
        }
    }
    if (index != group_count ||
            plan->entry_count + plan->attachment_count != group_count) {
        leme_sticky_discard_outputs(&plan);
        return false;
    }
    *slot = plan;
    return true;
}

void
leme_sticky_commit_outputs(struct leme_sticky_output_plan **slot)
{
    struct leme_sticky_output_plan *plan;

    assert(slot != NULL && *slot != NULL);
    plan = *slot;
    *slot = NULL;
    for (size_t index = 0; index < plan->entry_count; index++) {
        struct leme_sticky_output_entry *entry = &plan->entries[index];

        for (size_t member_index = 0;
                member_index < entry->view_count; member_index++) {
            leme_capture_invalidate_view(entry->views[member_index]);
        }
        leme_ownership_commit(&entry->owner);
        for (size_t member_index = 0;
                member_index < entry->view_count; member_index++) {
            struct leme_view *view = entry->views[member_index];

            view->box = entry->boxes[member_index];
            if (entry->presentation == LEME_DURABLE_OUTPUT) {
                view->sticky_member->anchor_area = entry->anchor;
            }
            if (view->render_tree != NULL) {
                wlr_scene_node_set_enabled(&view->render_tree->node,
                    entry->presentation == LEME_DURABLE_OUTPUT);
                if (entry->presentation == LEME_DURABLE_OUTPUT) {
                    leme_render_view_set_box(view, view->box);
                }
            }
        }
        entry->group->pending_attach = entry->attach_pending;
        if (entry->presentation == LEME_DURABLE_HIDDEN &&
                plan->server->focused_view != NULL &&
                leme_sticky_group(plan->server->focused_view) == entry->group) {
            leme_view_clear_focus(plan->server);
        } else if (entry->preserve_focus) {
            leme_output_set_focused(plan->server, entry->output, false);
        }
        leme_sticky_output_entry_finish(entry);
    }
    for (size_t index = 0; index < plan->attachment_count; index++) {
        struct leme_sticky_pending_attach *attachment =
            &plan->attachments[index];

        for (size_t view_index = 0;
                view_index < attachment->view_count; view_index++) {
            leme_capture_invalidate_view(attachment->views[view_index]);
        }
        leme_ownership_commit(&attachment->owner);
        for (size_t view_index = 0;
                view_index < attachment->view_count; view_index++) {
            struct leme_view *view = attachment->views[view_index];

            view->box = attachment->boxes[view_index];
            view->sticky_member = NULL;
            if (view->render_tree != NULL) {
                wlr_scene_node_reparent(&view->render_tree->node,
                    plan->server->scene_floating);
                wlr_scene_node_set_enabled(&view->render_tree->node, true);
                leme_render_view_set_box(view, view->box);
            }
        }
        free(attachment->boxes);
        free((void *)attachment->views);
        leme_sticky_group_destroy(attachment->group, false);
    }
    leme_publication_invalidate(plan->server);
    free(plan->attachments);
    free(plan->entries);
    free(plan);
}

void
leme_sticky_discard_outputs(struct leme_sticky_output_plan **slot)
{
    struct leme_sticky_output_plan *plan;

    if (slot == NULL || *slot == NULL) {
        return;
    }
    plan = *slot;
    *slot = NULL;
    for (size_t index = 0; index < plan->entry_count; index++) {
        leme_sticky_output_entry_finish(&plan->entries[index]);
    }
    for (size_t index = 0; index < plan->attachment_count; index++) {
        leme_ownership_discard(&plan->attachments[index].owner);
        free(plan->attachments[index].boxes);
        free((void *)plan->attachments[index].views);
    }
    free(plan->attachments);
    free(plan->entries);
    free(plan);
}

void
// The removed output and successor have deliberately distinct roles.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
leme_sticky_handle_output_destroy(struct leme_server *server,
    struct leme_output *output, // NOLINT(bugprone-easily-swappable-parameters)
    struct leme_output *successor)
{
    struct leme_sticky_group *group;

    if (server == NULL || output == NULL) {
        return;
    }
    wl_list_for_each(group, &server->sticky.groups, link) {
        struct leme_sticky_member *member;

        if (group->root == NULL ||
                leme_ownership_effective_output(group->root) != output) {
            continue;
        }
        wl_list_for_each(member, &group->members, link) {
            leme_capture_invalidate_view(member->view);
            (void)leme_ownership_present_durable(member->view,
                successor == NULL ? LEME_DURABLE_HIDDEN :
                    LEME_DURABLE_OUTPUT, successor);
            if (member->view->render_tree != NULL) {
                wlr_scene_node_set_enabled(&member->view->render_tree->node,
                    successor != NULL);
            }
            if (successor != NULL) {
                member->view->box = leme_view_policy_reanchor_box(
                    member->view->box, member->anchor_area,
                    successor->usable_box);
                member->anchor_area = successor->usable_box;
                leme_render_view_set_box(member->view, member->view->box);
            }
        }
        if (successor == NULL && server->focused_view != NULL &&
                leme_sticky_group(server->focused_view) == group) {
            leme_view_clear_focus(server);
        }
    }
}

void
leme_sticky_handle_usable_area(struct leme_output *output)
{
    struct leme_sticky_group *group;

    if (output == NULL || output->server == NULL) {
        return;
    }
    wl_list_for_each(group, &output->server->sticky.groups, link) {
        if (group->root != NULL &&
                leme_ownership_effective_output(group->root) == output) {
            struct leme_sticky_member *member;

            wl_list_for_each(member, &group->members, link) {
                const struct leme_box box =
                    leme_view_policy_reanchor_box(member->view->box,
                        member->anchor_area, output->usable_box);

                member->view->box = box;
                member->anchor_area = output->usable_box;
                leme_render_view_set_box(member->view, box);
            }
        }
    }
}

void
leme_sticky_raise_group(struct leme_view *view)
{
    struct leme_sticky_group *group = leme_sticky_group(view);

    if (group != NULL) {
        leme_render_durable_group_raise(group->scene_tree);
    }
}

static bool
leme_sticky_focusable_on(const struct leme_view *view,
    const struct leme_output *output)
{
    return view != NULL && view->mapped && !view->unmanaged &&
        leme_ownership_focus_eligible(view) &&
        leme_ownership_effective_output(view) == output;
}

/*
 * Uma vista presa não pertence a nenhuma tag, por isso as travessias de
 * foco que percorrem tag->views nunca lhe chegam. Este candidato é a porta de
 * entrada delas.
 */
struct leme_view *
leme_sticky_focus_candidate(struct leme_server *server,
    struct leme_output *output)
{
    struct leme_sticky_group *group;
    struct leme_sticky_member *member;
    struct leme_view *recent;

    if (server == NULL || output == NULL || server->sticky.groups.next == NULL) {
        return NULL;
    }
    if (server->focus_history.next != NULL) {
        wl_list_for_each(recent, &server->focus_history, focus_link) {
            if (leme_view_is_sticky(recent) &&
                    leme_sticky_focusable_on(recent, output)) {
                return recent;
            }
        }
    }
    wl_list_for_each(group, &server->sticky.groups, link) {
        if (group->root == NULL) {
            continue;
        }
        wl_list_for_each(member, &group->members, link) {
            if (leme_sticky_focusable_on(member->view, output)) {
                return member->view;
            }
        }
    }
    return NULL;
}

void
leme_sticky_handle_unmap(struct leme_view *view)
{
    struct leme_sticky_member *member;
    struct leme_sticky_group *group;

    if (view == NULL) {
        return;
    }
    member = view->sticky_member;
    if (member == NULL) {
        return;
    }
    group = member->group;
    if (group->root != view) {
        struct leme_sticky_member *candidate;

        wl_list_for_each(candidate, &group->members, link) {
            if (candidate->parent == member) {
                candidate->parent = member->parent;
            }
        }
        view->sticky_member = NULL;
        wl_list_remove(&member->link);
        free(member);
        leme_ownership_clear(view);
        return;
    }

    if (leme_sticky_group_count(group) == 1) {
        view->sticky_member = NULL;
        if (view->render_tree != NULL && view->server->scene_durable != NULL) {
            wlr_scene_node_reparent(&view->render_tree->node,
                view->server->scene_durable);
        }
        leme_sticky_group_destroy(group, false);
        leme_ownership_clear(view);
        return;
    }

    struct leme_output *output = leme_ownership_effective_output(view);
    struct leme_view **views = NULL;
    struct leme_ownership_transition *owner = NULL;
    size_t view_count = 0;
    size_t survivor_count;

    if ((leme_gate_sticky_unmap_prepare != NULL &&
            !leme_gate_sticky_unmap_prepare(
                LEME_STICKY_UNMAP_PREPARE_VIEWS)) ||
            !leme_sticky_group_views(group, &views, &view_count)) {
        leme_sticky_hide_pending_group(group, member);
        leme_ownership_clear(view);
        leme_publication_invalidate(view->server);
        return;
    }
    survivor_count = 0;
    for (size_t index = 0; index < view_count; index++) {
        if (views[index] != view) {
            views[survivor_count++] = views[index];
        }
    }
    if (survivor_count + 1 != view_count) {
        leme_sticky_hide_pending_group(group, member);
        leme_ownership_clear(view);
        free((void *)views);
        return;
    }
    if (output != NULL &&
            (leme_gate_sticky_unmap_prepare == NULL ||
                leme_gate_sticky_unmap_prepare(
                    LEME_STICKY_UNMAP_PREPARE_OWNER)) &&
            leme_ownership_prepare_durable_group_to_tag(views,
                survivor_count,
                leme_sticky_group_focused(group) == view ? NULL :
                    leme_sticky_group_focused(group),
                &output->tags, output->tags.focused_id, &owner)) {
        leme_ownership_commit(&owner);
        for (size_t index = 0; index < survivor_count; index++) {
            views[index]->sticky_member = NULL;
            if (views[index]->render_tree != NULL) {
                wlr_scene_node_reparent(&views[index]->render_tree->node,
                    view->server->scene_floating);
            }
        }
        view->sticky_member = NULL;
        if (view->render_tree != NULL &&
                view->server->scene_durable != NULL) {
            wlr_scene_node_reparent(&view->render_tree->node,
                view->server->scene_durable);
        }
        leme_ownership_clear(view);
        free((void *)views);
        leme_sticky_group_destroy(group, false);
        return;
    }
    leme_ownership_discard(&owner);
    leme_sticky_hide_pending_group(group, member);
    leme_ownership_clear(view);
    free((void *)views);
    leme_publication_invalidate(view->server);
}
