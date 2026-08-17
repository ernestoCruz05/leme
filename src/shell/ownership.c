#include "shell/ownership.h"

#include "core/gate.h"
#include "output/output.h"
#include "shell/ownership_internal.h"
#include "shell/view.h"
#include "workspace/tag.h"

#include <assert.h>
#include <stdlib.h>

enum leme_ownership_transition_kind {
    LEME_OWNERSHIP_NONE_TO_DURABLE,
    LEME_OWNERSHIP_TAG_TO_DURABLE,
    LEME_OWNERSHIP_DURABLE_TO_TAG,
    LEME_OWNERSHIP_DURABLE_TO_DURABLE,
};

struct leme_ownership_transition {
    enum leme_ownership_transition_kind kind;
    struct leme_view **views;
    size_t view_count;
    struct leme_view *focused;
    struct leme_tag_detach *detach;
    struct leme_tag_materialize *materialize;
    enum leme_durable_reason reason;
    enum leme_durable_presentation presentation;
    struct leme_output *output;
};

static bool
leme_ownership_valid_durable(enum leme_durable_reason reason,
    enum leme_durable_presentation presentation,
    const struct leme_output *output)
{
    bool reason_valid = false;

    switch (reason) {
    case LEME_DURABLE_SCRATCHPAD:
    case LEME_DURABLE_STICKY:
        reason_valid = true;
        break;
    }
    if (!reason_valid) {
        return false;
    }
    switch (presentation) {
    case LEME_DURABLE_HIDDEN:
        return output == NULL;
    case LEME_DURABLE_OUTPUT:
        return output != NULL;
    }
    return false;
}

static bool
leme_ownership_base_eligible(const struct leme_view *view)
{
    if (view == NULL || !view->mapped || view->unmanaged) {
        return false;
    }
    switch (view->owner.kind) {
    case LEME_VIEW_OWNER_NONE:
        return false;
    case LEME_VIEW_OWNER_TAG:
        return view->owner.value.tag != NULL;
    case LEME_VIEW_OWNER_DURABLE:
        return view->owner.value.durable.presentation ==
                LEME_DURABLE_OUTPUT &&
            view->owner.value.durable.output != NULL;
    }
    return false;
}

void
leme_ownership_init(struct leme_view *view)
{
    if (view == NULL) {
        return;
    }
    view->owner = (struct leme_view_owner){
        .kind = LEME_VIEW_OWNER_NONE,
    };
    view->unmanaged_output = NULL;
}

enum leme_view_owner_kind
leme_ownership_kind(const struct leme_view *view)
{
    if (view == NULL) {
        return LEME_VIEW_OWNER_NONE;
    }
    return view->owner.kind;
}

struct leme_tag *
leme_ownership_tag(const struct leme_view *view)
{
    if (view == NULL) {
        return NULL;
    }
    return view->owner.kind == LEME_VIEW_OWNER_TAG ?
        view->owner.value.tag : NULL;
}

bool
leme_ownership_is_durable(const struct leme_view *view,
    enum leme_durable_reason reason)
{
    return view != NULL && view->owner.kind == LEME_VIEW_OWNER_DURABLE &&
        view->owner.value.durable.reason == reason;
}

const struct leme_durable_owner *
leme_ownership_durable(const struct leme_view *view)
{
    return view != NULL && view->owner.kind == LEME_VIEW_OWNER_DURABLE ?
        &view->owner.value.durable : NULL;
}

struct leme_output *
leme_ownership_effective_output(const struct leme_view *view)
{
    if (view == NULL) {
        return NULL;
    }
    switch (view->owner.kind) {
    case LEME_VIEW_OWNER_NONE:
        return view->unmanaged ? view->unmanaged_output : NULL;
    case LEME_VIEW_OWNER_TAG:
        return view->owner.value.tag == NULL ||
            view->owner.value.tag->owner == NULL ? NULL :
            view->owner.value.tag->owner->output;
    case LEME_VIEW_OWNER_DURABLE:
        return view->owner.value.durable.presentation ==
            LEME_DURABLE_OUTPUT ?
            view->owner.value.durable.output : NULL;
    }
    return NULL;
}

const struct leme_view_restore *
leme_ownership_restore(const struct leme_view *view)
{
    const struct leme_durable_owner *durable =
        leme_ownership_durable(view);

    return durable == NULL ? NULL : durable->restore;
}

bool
leme_ownership_scene_visible(const struct leme_view *view)
{
    return leme_ownership_base_eligible(view);
}

bool
leme_ownership_hit_test_eligible(const struct leme_view *view)
{
    return leme_ownership_base_eligible(view);
}

bool
leme_ownership_focus_eligible(const struct leme_view *view)
{
    return leme_ownership_base_eligible(view);
}

bool
leme_ownership_activation_eligible(const struct leme_view *view)
{
    return leme_ownership_base_eligible(view);
}

bool
leme_ownership_publication_eligible(const struct leme_view *view)
{
    return leme_ownership_base_eligible(view);
}

bool
leme_ownership_direct_capture_eligible(const struct leme_view *view)
{
    return leme_ownership_base_eligible(view);
}

bool
leme_ownership_prepare_none_to_durable(struct leme_view *view,
    enum leme_durable_reason reason,
    enum leme_durable_presentation presentation,
    struct leme_output *output,
    struct leme_ownership_transition **transition)
{
    struct leme_ownership_transition *prepared;

    if (view == NULL || transition == NULL || *transition != NULL ||
            view->owner.kind != LEME_VIEW_OWNER_NONE ||
            !leme_ownership_valid_durable(reason, presentation, output)) {
        return false;
    }
    if (leme_gate_ownership_prepare != NULL &&
            !leme_gate_ownership_prepare(view)) {
        return false;
    }
    prepared = calloc(1, sizeof(*prepared));
    if (prepared == NULL) {
        return false;
    }
    prepared->views =
        (struct leme_view **)calloc(1, sizeof(*prepared->views));
    if (prepared->views == NULL) {
        free(prepared);
        return false;
    }
    prepared->views[0] = view;
    *prepared = (struct leme_ownership_transition){
        .kind = LEME_OWNERSHIP_NONE_TO_DURABLE,
        .views = prepared->views,
        .view_count = 1,
        .reason = reason,
        .presentation = presentation,
        .output = output,
    };
    *transition = prepared;
    return true;
}

bool
leme_ownership_prepare_tag_to_durable(struct leme_view *view,
    enum leme_durable_reason reason,
    enum leme_durable_presentation presentation,
    struct leme_output *output,
    struct leme_ownership_transition **slot)
{
    struct leme_ownership_transition *prepared;

    if (view == NULL || slot == NULL || *slot != NULL ||
            leme_ownership_tag(view) == NULL ||
            !leme_ownership_valid_durable(reason, presentation, output)) {
        return false;
    }
    if (leme_gate_ownership_prepare != NULL &&
            !leme_gate_ownership_prepare(view)) {
        return false;
    }
    prepared = calloc(1, sizeof(*prepared));
    if (prepared == NULL) {
        return false;
    }
    prepared->views =
        (struct leme_view **)calloc(1, sizeof(*prepared->views));
    if (prepared->views == NULL) {
        free(prepared);
        return false;
    }
    prepared->views[0] = view;
    if (!leme_tags_prepare_detach(view, &prepared->detach)) {
        free((void *)prepared->views);
        free(prepared);
        return false;
    }
    prepared->kind = LEME_OWNERSHIP_TAG_TO_DURABLE;
    prepared->view_count = 1;
    prepared->reason = reason;
    prepared->presentation = presentation;
    prepared->output = output;
    *slot = prepared;
    return true;
}

bool
leme_ownership_prepare_durable_group_to_tag(
    struct leme_view *const *views, size_t view_count,
    struct leme_view *focused, struct leme_tags *tags, uint16_t tag_id,
    struct leme_ownership_transition **slot)
{
    struct leme_ownership_transition *prepared;
    size_t index;

    if (views == NULL || view_count == 0 ||
            view_count > SIZE_MAX / sizeof(*prepared->views) ||
            slot == NULL || *slot != NULL || tags == NULL ||
            tag_id == 0 || tag_id > tags->max_tags) {
        return false;
    }
    for (index = 0; index < view_count; index++) {
        if (views[index] == NULL ||
                views[index]->owner.kind != LEME_VIEW_OWNER_DURABLE) {
            return false;
        }
    }
    if (leme_gate_ownership_prepare != NULL &&
            !leme_gate_ownership_prepare(views[0])) {
        return false;
    }
    prepared = calloc(1, sizeof(*prepared));
    if (prepared == NULL) {
        return false;
    }
    prepared->views = (struct leme_view **)calloc(
        view_count, sizeof(*prepared->views));
    if (prepared->views == NULL) {
        free(prepared);
        return false;
    }
    for (index = 0; index < view_count; index++) {
        prepared->views[index] = views[index];
    }
    if (!leme_tags_prepare_materialize(tags, tag_id,
            &prepared->materialize)) {
        free((void *)prepared->views);
        free(prepared);
        return false;
    }
    prepared->kind = LEME_OWNERSHIP_DURABLE_TO_TAG;
    prepared->view_count = view_count;
    prepared->focused = focused;
    *slot = prepared;
    return true;
}

bool
// The adjacent count and presentation have distinct named semantics.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
leme_ownership_prepare_durable_group_to_durable(
    struct leme_view *const *views, size_t view_count, // NOLINT(bugprone-easily-swappable-parameters)
    enum leme_durable_presentation presentation,
    struct leme_output *output,
    struct leme_ownership_transition **slot)
{
    struct leme_ownership_transition *prepared;
    enum leme_durable_reason reason;
    size_t index;

    if (views == NULL || view_count == 0 || slot == NULL || *slot != NULL ||
            view_count > SIZE_MAX / sizeof(*prepared->views) ||
            views[0] == NULL ||
            views[0]->owner.kind != LEME_VIEW_OWNER_DURABLE) {
        return false;
    }
    reason = views[0]->owner.value.durable.reason;
    if (!leme_ownership_valid_durable(reason, presentation, output)) {
        return false;
    }
    for (index = 0; index < view_count; index++) {
        if (!leme_ownership_is_durable(views[index], reason)) {
            return false;
        }
    }
    if (leme_gate_ownership_prepare != NULL &&
            !leme_gate_ownership_prepare(views[0])) {
        return false;
    }
    prepared = calloc(1, sizeof(*prepared));
    if (prepared == NULL) {
        return false;
    }
    prepared->views = (struct leme_view **)calloc(
        view_count, sizeof(*prepared->views));
    if (prepared->views == NULL) {
        free(prepared);
        return false;
    }
    for (index = 0; index < view_count; index++) {
        prepared->views[index] = views[index];
    }
    prepared->kind = LEME_OWNERSHIP_DURABLE_TO_DURABLE;
    prepared->view_count = view_count;
    prepared->reason = reason;
    prepared->presentation = presentation;
    prepared->output = output;
    *slot = prepared;
    return true;
}

static void
leme_ownership_install_durable(struct leme_view *view,
    enum leme_durable_reason reason,
    enum leme_durable_presentation presentation,
    struct leme_output *output)
{
    view->owner = (struct leme_view_owner){
        .kind = LEME_VIEW_OWNER_DURABLE,
        .value.durable = {
            .reason = reason,
            .presentation = presentation,
            .output = output,
            .restore = NULL,
        },
    };
}

void
leme_ownership_commit(struct leme_ownership_transition **slot)
{
    struct leme_ownership_transition *prepared;
    size_t index;

    assert(slot != NULL && *slot != NULL);
    prepared = *slot;
    *slot = NULL;
    switch (prepared->kind) {
    case LEME_OWNERSHIP_NONE_TO_DURABLE:
        assert(prepared->view_count == 1 &&
            leme_ownership_kind(prepared->views[0]) ==
                LEME_VIEW_OWNER_NONE);
        leme_ownership_install_durable(prepared->views[0],
            prepared->reason, prepared->presentation, prepared->output);
        break;
    case LEME_OWNERSHIP_TAG_TO_DURABLE:
        assert(prepared->view_count == 1 && prepared->detach != NULL);
        leme_tags_commit_detach(prepared->views[0], &prepared->detach);
        leme_ownership_install_durable(prepared->views[0],
            prepared->reason, prepared->presentation, prepared->output);
        break;
    case LEME_OWNERSHIP_DURABLE_TO_TAG: {
        struct leme_tag *tag =
            leme_tags_materialize_target(prepared->materialize);

        assert(tag != NULL);
        leme_tags_commit_materialize(&prepared->materialize);
        for (index = 0; index < prepared->view_count; index++) {
            struct leme_view *view = prepared->views[index];

            assert(view->owner.kind == LEME_VIEW_OWNER_DURABLE &&
                view->owner.value.durable.restore == NULL);
            view->owner = (struct leme_view_owner){
                .kind = LEME_VIEW_OWNER_NONE,
            };
            view->floating = true;
            leme_tags_attach_floating_prepared(tag, view,
                view == prepared->focused);
        }
        break;
    }
    case LEME_OWNERSHIP_DURABLE_TO_DURABLE:
        for (index = 0; index < prepared->view_count; index++) {
            assert(prepared->views[index]->owner.kind ==
                LEME_VIEW_OWNER_DURABLE);
            prepared->views[index]->owner.value.durable.presentation =
                prepared->presentation;
            prepared->views[index]->owner.value.durable.output =
                prepared->output;
        }
        break;
    }
    free((void *)prepared->views);
    free(prepared);
}

void
leme_ownership_discard(struct leme_ownership_transition **slot)
{
    struct leme_ownership_transition *prepared;

    if (slot == NULL || *slot == NULL) {
        return;
    }
    prepared = *slot;
    *slot = NULL;
    leme_tags_discard_detach(&prepared->detach);
    leme_tags_discard_materialize(&prepared->materialize);
    free((void *)prepared->views);
    free(prepared);
}

bool
leme_ownership_present_durable(struct leme_view *view,
    enum leme_durable_presentation presentation,
    struct leme_output *output)
{
    if (view == NULL || view->owner.kind != LEME_VIEW_OWNER_DURABLE ||
            !leme_ownership_valid_durable(view->owner.value.durable.reason,
                presentation, output)) {
        return false;
    }
    view->owner.value.durable.presentation = presentation;
    view->owner.value.durable.output = output;
    return true;
}

void
leme_ownership_clear(struct leme_view *view)
{
    if (view == NULL) {
        return;
    }
    if (view->owner.kind == LEME_VIEW_OWNER_DURABLE) {
        assert(view->owner.value.durable.restore == NULL);
    }
    view->owner = (struct leme_view_owner){
        .kind = LEME_VIEW_OWNER_NONE,
    };
    view->unmanaged_output = NULL;
}

void
leme_ownership_set_unmanaged_output(struct leme_view *view,
    struct leme_output *output)
{
    if (view == NULL || !view->unmanaged ||
            view->owner.kind != LEME_VIEW_OWNER_NONE) {
        return;
    }
    view->unmanaged_output = output;
}

void
leme_ownership_commit_tag(struct leme_view *view, struct leme_tag *tag)
{
    assert(view != NULL && tag != NULL &&
        view->owner.kind == LEME_VIEW_OWNER_NONE);
    view->owner = (struct leme_view_owner){
        .kind = LEME_VIEW_OWNER_TAG,
        .value.tag = tag,
    };
}

void
leme_ownership_replace_tag(struct leme_view *view,
    struct leme_tag *source, struct leme_tag *destination)
{
    assert(view != NULL && source != NULL && destination != NULL &&
        view->owner.kind == LEME_VIEW_OWNER_TAG &&
        view->owner.value.tag == source);
    view->owner.value.tag = destination;
}

void
leme_ownership_release_tag(struct leme_view *view, struct leme_tag *tag)
{
    assert(view != NULL && tag != NULL &&
        view->owner.kind == LEME_VIEW_OWNER_TAG &&
        view->owner.value.tag == tag);
    view->owner = (struct leme_view_owner){
        .kind = LEME_VIEW_OWNER_NONE,
    };
}
