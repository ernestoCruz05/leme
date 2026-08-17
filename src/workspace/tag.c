#include "core/gate.h"
#include "workspace/tag.h"

#include "config/config.h"
#include "shell/layer.h"
#include "shell/ownership.h"
#include "shell/ownership_internal.h"
#include "shell/view.h"
#include "core/server.h"
#include "output/output.h"
#include "protocols/publication.h"
#include "render/render.h"
#include "render/workspace_transition.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static void
leme_tag_apply_settings(struct leme_tag *tag,
    const struct leme_tag_settings *settings);


static size_t
leme_tags_tiled_views(const struct leme_tag *tag,
    struct leme_view **views, size_t capacity)
{
    struct wl_list *link;
    size_t count = 0;

    for (link = tag->views.next; link != &tag->views; link = link->next) {
        struct leme_view *view = wl_container_of(link, view, tag_link);

        if (!view->mapped || view->unmanaged || view->floating ||
                view->fullscreen) {
            continue;
        }
        if (count < capacity) {
            views[count] = view;
        }
        count++;
    }
    return count;
}

bool
leme_tags_swap_list_order(struct leme_tag *tag,
    struct leme_view *first, struct leme_view *second)
{
    struct wl_list *first_prev;
    struct wl_list *second_prev;

    if (tag == NULL || first == NULL || second == NULL || first == second) {
        return false;
    }
    first_prev = first->tag_link.prev;
    second_prev = second->tag_link.prev;
    if (first_prev == &second->tag_link) {
        wl_list_remove(&second->tag_link);
        wl_list_insert(&first->tag_link, &second->tag_link);
        return true;
    }
    if (second_prev == &first->tag_link) {
        wl_list_remove(&first->tag_link);
        wl_list_insert(&second->tag_link, &first->tag_link);
        return true;
    }
    wl_list_remove(&first->tag_link);
    wl_list_remove(&second->tag_link);
    wl_list_insert(second_prev, &first->tag_link);
    wl_list_insert(first_prev, &second->tag_link);
    return true;
}

static bool
leme_tag_settings_equal(const struct leme_tag_settings *left,
    const struct leme_tag_settings *right)
{
    return left->layout == right->layout &&
        left->drop_mode == right->drop_mode &&
        left->nmaster == right->nmaster &&
        left->collapse_width == right->collapse_width &&
        left->has_gap == right->has_gap &&
        left->gap == right->gap &&
        left->mfact == right->mfact &&
        left->split_ratio == right->split_ratio;
}

void
leme_tags_apply_settings(struct leme_tags *tags,
    const struct leme_config *previous, const struct leme_config *next)
{
    uint16_t id;

    if (tags == NULL || next == NULL) {
        return;
    }
    for (id = 1; id <= tags->max_tags; id++) {
        struct leme_tag_settings old_settings;
        struct leme_tag_settings new_settings;
        struct leme_tag *tag = tags->table[id];

        if (tag == NULL) {
            continue;
        }
        leme_config_tag_settings(next, id, &new_settings);
        if (previous != NULL) {
            leme_config_tag_settings(previous, id, &old_settings);
            if (leme_tag_settings_equal(&old_settings, &new_settings)) {
                continue;
            }
        }
        if (tag->layout.kind != new_settings.layout) {
            struct leme_view **views;
            size_t count = leme_tags_tiled_views(tag, NULL, 0);

            if (count == 0) {
                leme_layout_set_kind(&tag->layout, new_settings.layout,
                    NULL, 0);
            } else {
                views = (struct leme_view **)calloc(
                    count, sizeof(*views));
                if (views != NULL) {
                    const size_t collected =
                        leme_tags_tiled_views(tag, views, count);

                    if (collected == count) {
                        leme_layout_set_kind(&tag->layout,
                            new_settings.layout, views, count);
                    }
                    free((void *)views);
                }
            }
        }
        leme_tag_apply_settings(tag, &new_settings);
    }
}

bool
leme_tags_set_layout(struct leme_tags *tags, enum leme_layout_kind kind)
{
    struct leme_tag *tag;
    struct leme_view **views;
    size_t count;

    if (tags == NULL || tags->focused_is_candidate) {
        return false;
    }
    tag = tags->table[tags->focused_id];
    if (tag == NULL) {
        return false;
    }
    if (tag->layout.kind == kind) {
        return true;
    }
    count = leme_tags_tiled_views(tag, NULL, 0);
    if (count == 0) {
        leme_layout_set_kind(&tag->layout, kind, NULL, 0);
        return true;
    }
    views = (struct leme_view **)calloc(count, sizeof(*views));
    if (views == NULL) {
        return false;
    }
    if (leme_tags_tiled_views(tag, views, count) != count) {
        free((void *)views);
        return false;
    }
    leme_layout_set_kind(&tag->layout, kind, views, count);
    free((void *)views);
    return true;
}

bool
leme_tags_cycle_layout(struct leme_tags *tags)
{
    struct leme_tag *tag;
    enum leme_layout_kind next;

    if (tags == NULL || tags->focused_is_candidate) {
        return false;
    }
    tag = tags->table[tags->focused_id];
    if (tag == NULL) {
        return false;
    }
    switch (tag->layout.kind) {
    case LEME_LAYOUT_DWINDLE:
        next = LEME_LAYOUT_MASTER_STACK;
        break;
    case LEME_LAYOUT_MASTER_STACK:
        next = LEME_LAYOUT_ACCORDION;
        break;
    case LEME_LAYOUT_ACCORDION:
    default:
        next = LEME_LAYOUT_DWINDLE;
        break;
    }
    return leme_tags_set_layout(tags, next);
}

enum leme_layout_kind
leme_tags_layout_kind(const struct leme_tags *tags)
{
    const struct leme_tag *tag;

    if (tags == NULL || tags->focused_is_candidate || tags->table == NULL) {
        return LEME_LAYOUT_DWINDLE;
    }
    tag = tags->table[tags->focused_id];
    return tag == NULL ? LEME_LAYOUT_DWINDLE : tag->layout.kind;
}

static void
leme_tag_apply_settings(struct leme_tag *tag,
    const struct leme_tag_settings *settings)
{
    tag->layout.kind = settings->layout;
    tag->layout.mfact = settings->mfact;
    tag->layout.nmaster = settings->nmaster;
    tag->layout.split_ratio = settings->split_ratio;
    tag->layout.collapse_width = settings->collapse_width;
    tag->layout.gap = settings->gap;
    tag->layout.has_gap = settings->has_gap;
}

static struct leme_tag *
materialize(struct leme_tags *tags, uint16_t id)
{
    struct leme_tag *tag;

    if (id == 0 || id > tags->max_tags) {
        return NULL;
    }
    if (tags->table[id] != NULL) {
        return tags->table[id];
    }
    if (leme_gate_tags_prepare != NULL && !leme_gate_tags_prepare(
            LEME_TAGS_PREPARE_MATERIALIZE_TAG_ALLOCATION, NULL, tags)) {
        return NULL;
    }
    tag = calloc(1, sizeof(*tag));
    if (tag == NULL) {
        return NULL;
    }
    tag->id = id;
    tag->owner = tags;
    leme_layout_init(&tag->layout, LEME_LAYOUT_DWINDLE);
    if (tags->server != NULL && tags->server->config != NULL) {
        struct leme_tag_settings settings;

        leme_config_tag_settings(tags->server->config, id, &settings);
        leme_tag_apply_settings(tag, &settings);
    }
    wl_list_init(&tag->views);
    tags->table[id] = tag;
    return tag;
}

static bool
leme_tags_set_focus_state(struct leme_tags *tags,
    uint16_t id, bool is_candidate, bool record_previous,
    enum leme_tag_change_direction direction)
{
    struct leme_workspace_transition *transition;

    if (id == 0 || id > tags->max_tags ||
            (direction != LEME_TAG_CHANGE_FORWARD &&
                direction != LEME_TAG_CHANGE_BACKWARD)) {
        return false;
    }
    if (tags->focused_id == id &&
            tags->focused_is_candidate == is_candidate) {
        return true;
    }
    transition = tags->output == NULL ? NULL :
        leme_render_workspace_transition_prepare(tags->output,
            tags->focused_id, id, direction);
    if (record_previous) {
        tags->previous_id = tags->focused_id;
        tags->previous_is_candidate = tags->focused_is_candidate;
        tags->previous_valid = true;
    }
    tags->focused_id = id;
    tags->focused_is_candidate = is_candidate;
    tags->last_change_direction = direction;
    tags->last_change_direction_valid = true;
    leme_publication_invalidate(tags->server);
    leme_render_workspace_transition_commit(transition);
    return true;
}

bool
leme_tags_init(struct leme_tags *tags, uint16_t initial_tags, uint16_t max_tags)
{
    uint16_t id;

    if (initial_tags == 0 || initial_tags > max_tags) {
        return false;
    }
    tags->table = (struct leme_tag **)calloc(
        (size_t)max_tags + 1, sizeof(*tags->table));
    if (tags->table == NULL) {
        return false;
    }
    tags->initial_tags = initial_tags;
    tags->max_tags = max_tags;
    for (id = 1; id <= initial_tags; id++) {
        if (materialize(tags, id) == NULL) {
            leme_tags_finish(tags);
            return false;
        }
    }
    tags->focused_id = 1;
    return true;
}

void
leme_tags_finish(struct leme_tags *tags)
{
    uint16_t id;

    for (id = 1; id <= tags->max_tags; id++) {
        struct leme_view *view;
        struct leme_view *next;

        if (tags->table == NULL || tags->table[id] == NULL) {
            continue;
        }
        wl_list_for_each_safe(view, next, &tags->table[id]->views, tag_link) {
            wl_list_remove(&view->tag_link);
            wl_list_init(&view->tag_link);
            leme_ownership_release_tag(view, tags->table[id]);
        }
        leme_layout_finish(&tags->table[id]->layout);
        free(tags->table[id]);
    }
    free((void *)tags->table);
    *tags = (struct leme_tags){0};
}

bool
leme_tags_can_set_max(const struct leme_tags *tags, uint16_t max_tags)
{
    uint16_t id;

    if (max_tags == 0) {
        return false;
    }
    for (id = max_tags + 1; id > max_tags && id <= tags->max_tags; id++) {
        if (tags->table[id] != NULL) {
            return false;
        }
    }
    return true;
}

bool
leme_tags_prepare_set_max(struct leme_tags *tags, uint16_t max_tags,
    struct leme_tags_resize *resize)
{
    struct leme_tag **table;
    const size_t count = (size_t)max_tags + 1;

    if (tags == NULL || resize == NULL || tags->table == NULL ||
            !leme_tags_can_set_max(tags, max_tags)) {
        return false;
    }
    if (tags->focused_id > max_tags) {
        uint16_t id;

        id = 1;
        while (id <= max_tags && tags->table[id] == NULL) {
            id++;
        }
        if (id > max_tags) {
            return false;
        }
    }
    *resize = (struct leme_tags_resize){
        .tags = tags,
        .max_tags = max_tags,
    };
    if (max_tags <= tags->max_tags) {
        return true;
    }
    if (count > SIZE_MAX / sizeof(*table)) {
        *resize = (struct leme_tags_resize){0};
        return false;
    }
    if (leme_gate_tags_prepare != NULL && !leme_gate_tags_prepare(
            LEME_TAGS_PREPARE_RESIZE_ALLOCATION, NULL, tags)) {
        *resize = (struct leme_tags_resize){0};
        return false;
    }
    table = (struct leme_tag **)calloc(count, sizeof(*table));
    if (table == NULL) {
        *resize = (struct leme_tags_resize){0};
        return false;
    }
    for (size_t index = 0; index <= tags->max_tags; index++) {
        table[index] = tags->table[index];
    }
    resize->table = table;
    return true;
}

void
leme_tags_discard_set_max(struct leme_tags_resize *resize)
{
    if (resize == NULL) {
        return;
    }
    free((void *)resize->table);
    *resize = (struct leme_tags_resize){0};
}

void
leme_tags_commit_set_max(struct leme_tags_resize *resize)
{
    struct leme_tags *tags;
    const uint16_t max_tags = resize == NULL ? 0 : resize->max_tags;

    if (resize == NULL || resize->tags == NULL || max_tags == 0) {
        return;
    }
    tags = resize->tags;
    if (resize->table != NULL) {
        free((void *)tags->table);
        tags->table = resize->table;
        resize->table = NULL;
    }
    tags->max_tags = max_tags;
    if (tags->previous_valid && tags->previous_id > max_tags) {
        tags->previous_valid = false;
    }
    if (tags->focused_id > max_tags) {
        leme_render_output_animations_finish(tags->output);
        tags->last_change_direction_valid = false;
        tags->focused_id = 1;
        tags->focused_is_candidate = false;
        while (tags->focused_id <= max_tags &&
                tags->table[tags->focused_id] == NULL) {
            tags->focused_id++;
        }
    }
    *resize = (struct leme_tags_resize){0};
}

bool
leme_tags_set_max(struct leme_tags *tags, uint16_t max_tags)
{
    struct leme_tags_resize resize = {0};

    if (!leme_tags_prepare_set_max(tags, max_tags, &resize)) {
        return false;
    }
    leme_tags_commit_set_max(&resize);
    return tags->focused_id <= max_tags;
}

struct leme_tag *
leme_tags_focus_id_direction(struct leme_tags *tags, uint16_t id,
    enum leme_tag_change_direction direction)
{
    struct leme_tag *tag;
    bool is_candidate;

    if (id == 0 || id > tags->max_tags ||
            (direction != LEME_TAG_CHANGE_FORWARD &&
                direction != LEME_TAG_CHANGE_BACKWARD)) {
        return NULL;
    }
    tag = tags->table[id];
    is_candidate = tag == NULL;
    if (tags->focused_id == id) {
        tags->focused_is_candidate = is_candidate;
        return tag;
    }
    if (!leme_tags_set_focus_state(tags, id, is_candidate, true,
            direction)) {
        return NULL;
    }
    return tag;
}

struct leme_tag *
leme_tags_focus_id(struct leme_tags *tags, uint16_t id)
{
    enum leme_tag_change_direction direction;

    if (id == 0 || id > tags->max_tags) {
        return NULL;
    }
    if (tags->focused_id == id) {
        const bool is_candidate = tags->table[id] == NULL;

        tags->focused_is_candidate = is_candidate;
        return tags->table[id];
    }
    direction = id > tags->focused_id ? LEME_TAG_CHANGE_FORWARD :
        LEME_TAG_CHANGE_BACKWARD;
    return leme_tags_focus_id_direction(tags, id, direction);
}

struct leme_tag *
leme_tags_step(struct leme_tags *tags,
    enum leme_tag_change_direction direction)
{
    uint16_t id;
    uint16_t count;
    bool is_candidate;

    if (direction != LEME_TAG_CHANGE_FORWARD &&
            direction != LEME_TAG_CHANGE_BACKWARD) {
        return NULL;
    }
    if (tags->focused_is_candidate) {
        for (count = 0, id = tags->focused_id;
                count < tags->max_tags; count++) {
            id = direction > 0 ? (id == tags->max_tags ? 1 : id + 1) :
                (id == 1 ? tags->max_tags : id - 1);
            if (tags->table[id] != NULL) {
                leme_tags_set_focus_state(tags, id, false, true, direction);
                return tags->table[id];
            }
        }
        return NULL;
    }
    id = direction > 0 ?
        (tags->focused_id == tags->max_tags ? 1 : tags->focused_id + 1) :
        (tags->focused_id == 1 ? tags->max_tags : tags->focused_id - 1);
    is_candidate = tags->table[id] == NULL;
    leme_tags_set_focus_state(tags, id, is_candidate, true, direction);
    return is_candidate ? NULL : tags->table[id];
}

uint16_t
leme_tags_adjacent_id(const struct leme_tags *tags,
    enum leme_tag_change_direction direction)
{
    if (tags == NULL ||
            (direction != LEME_TAG_CHANGE_FORWARD &&
                direction != LEME_TAG_CHANGE_BACKWARD) ||
            tags->focused_id == 0 || tags->focused_id > tags->max_tags) {
        return 0;
    }
    return direction > 0 ?
        (tags->focused_id == tags->max_tags ? 1 : tags->focused_id + 1) :
        (tags->focused_id == 1 ? tags->max_tags : tags->focused_id - 1);
}

bool
leme_tags_focus_last(struct leme_tags *tags)
{
    struct leme_workspace_transition *transition;
    enum leme_tag_change_direction direction;
    uint16_t current_id;
    bool current_is_candidate;
    bool previous_is_candidate;

    if (!tags->previous_valid || tags->previous_id == 0 ||
            tags->previous_id > tags->max_tags) {
        return false;
    }
    previous_is_candidate = tags->previous_is_candidate &&
        tags->table[tags->previous_id] == NULL;
    if (!previous_is_candidate && tags->table[tags->previous_id] == NULL) {
        tags->previous_valid = false;
        return false;
    }
    direction = tags->last_change_direction_valid ?
        (tags->last_change_direction == LEME_TAG_CHANGE_FORWARD ?
            LEME_TAG_CHANGE_BACKWARD : LEME_TAG_CHANGE_FORWARD) :
        (tags->previous_id > tags->focused_id ?
            LEME_TAG_CHANGE_FORWARD : LEME_TAG_CHANGE_BACKWARD);
    transition = tags->output == NULL ? NULL :
        leme_render_workspace_transition_prepare(tags->output,
            tags->focused_id, tags->previous_id, direction);
    current_id = tags->focused_id;
    current_is_candidate = tags->focused_is_candidate;
    tags->focused_id = tags->previous_id;
    tags->focused_is_candidate = previous_is_candidate;
    tags->previous_id = current_id;
    tags->previous_is_candidate = current_is_candidate;
    tags->last_change_direction = direction;
    tags->last_change_direction_valid = true;
    leme_publication_invalidate(tags->server);
    leme_render_workspace_transition_commit(transition);
    return true;
}

static bool
leme_tags_prune_empty(struct leme_tags *tags, struct leme_tag *tag)
{
    uint16_t id;

    if (tag == NULL || tag->id <= tags->initial_tags ||
            !wl_list_empty(&tag->views)) {
        return false;
    }
    id = tag->id;
    if (tags->focused_id == id && !tags->focused_is_candidate) {
        tags->focused_is_candidate = true;
    }
    if (tags->previous_valid && tags->previous_id == id &&
            !tags->previous_is_candidate) {
        tags->previous_is_candidate = true;
    }
    tags->table[id] = NULL;
    leme_layout_finish(&tag->layout);
    free(tag);
    return true;
}

bool
leme_tags_remove_empty(struct leme_tags *tags, uint16_t id)
{
    if (id == 0 || id > tags->max_tags || id <= tags->initial_tags ||
            tags->table[id] == NULL) {
        return false;
    }
    return leme_tags_prune_empty(tags, tags->table[id]);
}

bool
leme_tags_assign_view_to(struct leme_tags *tags,
    struct leme_view *view, uint16_t id)
{
    struct leme_tag *tag;

    if (tags == NULL || view == NULL || leme_ownership_tag(view) != NULL || id == 0 ||
            id > tags->max_tags) {
        return false;
    }
    tag = materialize(tags, id);
    if (tag == NULL) {
        return false;
    }
    if (tags->server == NULL) {
        tags->server = view->server;
    }
    if (id == tags->focused_id) {
        tags->focused_is_candidate = false;
    }
    leme_ownership_commit_tag(view, tag);
    wl_list_insert(&tag->views, &view->tag_link);
    if (!view->floating) {
        leme_layout_add_view(&tag->layout, tag->focused_view, view);
    }
    return true;
}

void
leme_tags_assign_view(struct leme_tags *tags, struct leme_view *view)
{
    if (tags != NULL) {
        (void)leme_tags_assign_view_to(tags, view, tags->focused_id);
    }
}

bool
leme_tags_adopt_view(struct leme_tags *destination, struct leme_view *view,
    uint16_t id)
{
    struct leme_tags *source_tags;
    struct leme_tag *source;
    struct leme_tag *target;

    if (destination == NULL || view == NULL || leme_ownership_tag(view) == NULL) {
        return false;
    }
    if (id == 0) {
        id = destination->focused_id;
    }
    if (id > destination->max_tags) {
        id = destination->max_tags;
    }
    source = leme_ownership_tag(view);
    source_tags = source->owner;
    target = materialize(destination, id);
    if (target == NULL) {
        return false;
    }
    if (source == target) {
        return true;
    }
    if (!view->floating && !view->detached) {
        leme_layout_remove_view(&source->layout, view);
    }
    if (source->focused_view == view) {
        source->focused_view = NULL;
    }
    wl_list_remove(&view->tag_link);
    leme_ownership_replace_tag(view, source, target);
    wl_list_insert(&target->views, &view->tag_link);
    if (!view->floating && !view->detached) {
        leme_layout_add_view(&target->layout, target->focused_view, view);
    }
    target->focused_view = view;
    if (target->id == destination->focused_id) {
        destination->focused_is_candidate = false;
    }
    if (source_tags != NULL && source_tags != destination) {
        (void)leme_tags_prune_empty(source_tags, source);
    }
    return true;
}

bool
leme_tags_move_view(struct leme_tags *tags,
    struct leme_view *view, uint16_t id)
{
    struct leme_tag *source;
    struct leme_tag *destination;

    if (view == NULL || leme_ownership_tag(view) == NULL || id == 0 || id > tags->max_tags) {
        return false;
    }
    destination = materialize(tags, id);
    if (destination == NULL) {
        return false;
    }
    source = leme_ownership_tag(view);
    if (source == destination) {
        return true;
    }
    if (!view->floating) {
        leme_layout_remove_view(&source->layout, view);
    }
    if (source->focused_view == view) {
        source->focused_view = NULL;
    }
    wl_list_remove(&view->tag_link);
    leme_ownership_replace_tag(view, source, destination);
    wl_list_insert(&destination->views, &view->tag_link);
    if (!view->floating) {
        leme_layout_add_view(&destination->layout,
            destination->focused_view, view);
    }
    destination->focused_view = view;
    (void)leme_tags_prune_empty(tags, source);
    return true;
}

struct leme_view *
leme_tags_directional_view(const struct leme_tags *tags,
    const struct leme_view *origin, enum leme_direction direction,
    bool tiled_only)
{
    struct leme_layout_candidate *candidates;
    struct leme_view *fullscreen = NULL;
    struct leme_view *view;
    struct leme_view *result;
    struct leme_tag *tag;
    size_t count = 0;
    size_t index = 0;

    if (tags == NULL || tags->focused_is_candidate ||
            tags->focused_id == 0 || tags->focused_id > tags->max_tags) {
        return NULL;
    }
    tag = tags->table[tags->focused_id];
    if (tag == NULL) {
        return NULL;
    }
    wl_list_for_each(view, &tag->views, tag_link) {
        if (view->mapped && view->fullscreen) {
            fullscreen = view;
            break;
        }
    }
    wl_list_for_each(view, &tag->views, tag_link) {
        if (!view->mapped || view->unmanaged || view->detached ||
                (fullscreen != NULL && view != fullscreen) ||
                (tiled_only && (view->floating || view->fullscreen))) {
            continue;
        }
        count++;
    }
    if (count == 0) {
        return NULL;
    }
    candidates = calloc(count, sizeof(*candidates));
    if (candidates == NULL) {
        return NULL;
    }
    wl_list_for_each(view, &tag->views, tag_link) {
        if (!view->mapped || view->unmanaged || view->detached ||
                (fullscreen != NULL && view != fullscreen) ||
                (tiled_only && (view->floating || view->fullscreen))) {
            continue;
        }
        if (index >= count) {
            free(candidates);
            return NULL;
        }
        candidates[index++] = (struct leme_layout_candidate){
            .view = view,
            .box = view->box,
        };
    }
    if (index != count) {
        free(candidates);
        return NULL;
    }
    result = leme_layout_directional_candidate(
        candidates, count, origin, direction);
    free(candidates);
    return result;
}

struct leme_view *
leme_tags_pointer_drop_target(
    const struct leme_tags *tags, const struct leme_view *excluded,
    double layout_x, double layout_y)
{
    struct leme_layout_candidate *candidates;
    struct leme_view *view;
    struct leme_view *result;
    struct leme_tag *tag;
    size_t count = 0;
    size_t index = 0;

    if (tags == NULL || tags->focused_is_candidate ||
            tags->focused_id == 0 || tags->focused_id > tags->max_tags) {
        return NULL;
    }
    tag = tags->table[tags->focused_id];
    if (tag == NULL) {
        return NULL;
    }
    wl_list_for_each(view, &tag->views, tag_link) {
        if (view->mapped && view->fullscreen) {
            return NULL;
        }
    }
    wl_list_for_each(view, &tag->views, tag_link) {
        if (view != excluded && view->mapped && !view->unmanaged &&
                !view->floating && !view->detached && !view->fullscreen) {
            count++;
        }
    }
    if (count == 0) {
        return NULL;
    }
    candidates = calloc(count, sizeof(*candidates));
    if (candidates == NULL) {
        return NULL;
    }
    wl_list_for_each(view, &tag->views, tag_link) {
        if (view == excluded || !view->mapped || view->unmanaged ||
                view->floating || view->detached || view->fullscreen) {
            continue;
        }
        if (index >= count) {
            free(candidates);
            return NULL;
        }
        candidates[index++] = (struct leme_layout_candidate){
            .view = view,
            .box = view->box,
        };
    }
    if (index != count) {
        free(candidates);
        return NULL;
    }
    result = leme_layout_nearest_candidate(
        candidates, count, layout_x, layout_y);
    free(candidates);
    return result;
}

bool
leme_tags_swap_directional(struct leme_tags *tags,
    struct leme_view *view, enum leme_direction direction)
{
    struct leme_view *neighbor;

    if (tags == NULL || view == NULL || leme_ownership_tag(view) == NULL ||
            view->floating || view->detached || view->fullscreen) {
        return false;
    }
    neighbor = leme_tags_directional_view(
        tags, view, direction, true);
    if (neighbor == NULL) {
        return false;
    }
    if (leme_ownership_tag(view)->layout.kind == LEME_LAYOUT_DWINDLE) {
        return leme_layout_swap_views(leme_ownership_tag(view)->layout.root, view, neighbor);
    }
    return leme_tags_swap_list_order(leme_ownership_tag(view), view, neighbor);
}

static void
leme_tags_apply_box(struct leme_view *view, struct leme_box box, void *data)
{
    (void)data;
    leme_view_apply_layout_box(view, box);
}

void
leme_tags_arrange_current(struct leme_tags *tags,
    struct leme_box usable_box, int gap)
{
    struct leme_tag *tag;
    struct leme_view **views;
    size_t count;

    if (tags->focused_is_candidate) {
        return;
    }
    tag = tags->table[tags->focused_id];
    if (tag == NULL) {
        return;
    }
    count = leme_tags_tiled_views(tag, NULL, 0);
    if (count == 0) {
        leme_layout_arrange_subject(&tag->layout, NULL, 0,
            tag->focused_view, usable_box, gap, leme_tags_apply_box, NULL);
        return;
    }
    views = (struct leme_view **)calloc(count, sizeof(*views));
    if (views == NULL) {
        return;
    }
    if (leme_tags_tiled_views(tag, views, count) != count) {
        free((void *)views);
        return;
    }
    leme_layout_arrange_subject(&tag->layout, views, count,
        tag->focused_view, usable_box, gap, leme_tags_apply_box, NULL);
    if (tag->layout.kind == LEME_LAYOUT_ACCORDION) {
        size_t index;

        for (index = 0; index < count; index++) {
            if (views[index] != tag->focused_view) {
                leme_render_view_focus(views[index]);
            }
        }
        if (tag->focused_view != NULL) {
            leme_render_view_focus(tag->focused_view);
        }
    }
    free((void *)views);
}

struct leme_tag_detach {
    struct leme_tag *tag;
    struct leme_view **remaining;
    size_t remaining_count;
    struct leme_box area;
    int gap;
};

bool
leme_tags_prepare_detach(struct leme_view *view,
    struct leme_tag_detach **detach)
{
    struct leme_tag_detach *plan;
    struct wl_list *link;
    size_t count = 0;
    size_t index = 0;

    if (view == NULL || detach == NULL || *detach != NULL || leme_ownership_tag(view) == NULL) {
        return false;
    }
    for (link = leme_ownership_tag(view)->views.next; link != &leme_ownership_tag(view)->views;
            link = link->next) {
        struct leme_view *candidate = wl_container_of(link, candidate,
            tag_link);

        if (candidate != view && candidate->mapped && !candidate->unmanaged &&
                !candidate->floating && !candidate->fullscreen) {
            count++;
        }
    }
    if (leme_gate_tags_prepare != NULL && !leme_gate_tags_prepare(
            LEME_TAGS_PREPARE_DETACH_PLAN_ALLOCATION, view,
            leme_ownership_tag(view)->owner)) {
        return false;
    }
    plan = calloc(1, sizeof(*plan));
    if (plan == NULL) {
        return false;
    }
    if (count > SIZE_MAX / sizeof(*plan->remaining)) {
        free(plan);
        return false;
    }
    if (count > 0) {
        if (leme_gate_tags_prepare != NULL && !leme_gate_tags_prepare(
                LEME_TAGS_PREPARE_DETACH_REMAINING_ALLOCATION, view,
                leme_ownership_tag(view)->owner)) {
            free(plan);
            return false;
        }
        plan->remaining = (struct leme_view **)calloc(
            count, sizeof(*plan->remaining));
        if (plan->remaining == NULL) {
            free(plan);
            return false;
        }
        for (link = leme_ownership_tag(view)->views.next; link != &leme_ownership_tag(view)->views;
                link = link->next) {
            struct leme_view *candidate = wl_container_of(link, candidate,
                tag_link);

            if (candidate != view && candidate->mapped &&
                    !candidate->unmanaged && !candidate->floating &&
                    !candidate->fullscreen) {
                if (index >= count) {
                    free((void *)plan->remaining);
                    free(plan);
                    return false;
                }
                plan->remaining[index++] = candidate;
            }
        }
        if (index != count) {
            free((void *)plan->remaining);
            free(plan);
            return false;
        }
    }
    plan->tag = leme_ownership_tag(view);
    plan->remaining_count = count;
    if (leme_ownership_tag(view)->owner != NULL && leme_ownership_tag(view)->owner->output != NULL) {
        plan->area = leme_ownership_tag(view)->owner->output->usable_box;
    }
    if (leme_ownership_tag(view)->owner != NULL && leme_ownership_tag(view)->owner->server != NULL &&
            leme_ownership_tag(view)->owner->server->config != NULL) {
        plan->gap = leme_ownership_tag(view)->owner->server->config->gap;
    }
    *detach = plan;
    return true;
}

void
leme_tags_commit_detach(struct leme_view *view,
    struct leme_tag_detach **detach)
{
    struct leme_tag_detach *plan;
    struct leme_tag *tag;
    struct leme_tags *tags;

    if (view == NULL || detach == NULL || *detach == NULL) {
        return;
    }
    plan = *detach;
    *detach = NULL;
    tag = plan->tag;
    tags = tag == NULL ? NULL : tag->owner;
    assert(tag != NULL && leme_ownership_tag(view) == tag);
    if (!view->floating) {
        leme_layout_remove_view(&tag->layout, view);
    }
    if (tag->focused_view == view) {
        tag->focused_view = NULL;
    }
    wl_list_remove(&view->tag_link);
    wl_list_init(&view->tag_link);
    leme_ownership_release_tag(view, tag);
    if (tags != NULL && tags->output != NULL &&
            !tags->focused_is_candidate &&
            tags->table[tags->focused_id] == tag) {
        leme_layout_arrange_subject(&tag->layout, plan->remaining,
            plan->remaining_count, tag->focused_view, plan->area, plan->gap,
            leme_tags_apply_box, NULL);
    }
    if (tags != NULL) {
        (void)leme_tags_prune_empty(tags, tag);
    }
    free((void *)plan->remaining);
    free(plan);
}

void
leme_tags_discard_detach(struct leme_tag_detach **detach)
{
    if (detach == NULL || *detach == NULL) {
        return;
    }
    free((void *)(*detach)->remaining);
    free(*detach);
    *detach = NULL;
}

struct leme_tag_materialize {
    struct leme_tags *tags;
    struct leme_tag *tag;
    bool owned;
};

bool
leme_tags_prepare_materialize(struct leme_tags *tags, uint16_t id,
    struct leme_tag_materialize **slot)
{
    struct leme_tag_materialize *plan;
    struct leme_tag *tag;

    if (tags == NULL || slot == NULL || *slot != NULL || id == 0 ||
            id > tags->max_tags || tags->table == NULL) {
        return false;
    }
    plan = calloc(1, sizeof(*plan));
    if (plan == NULL) {
        return false;
    }
    tag = tags->table[id];
    if (tag == NULL) {
        if (leme_gate_tags_prepare != NULL && !leme_gate_tags_prepare(
                LEME_TAGS_PREPARE_MATERIALIZE_TAG_ALLOCATION,
                NULL, tags)) {
            free(plan);
            return false;
        }
        tag = calloc(1, sizeof(*tag));
        if (tag == NULL) {
            free(plan);
            return false;
        }
        tag->id = id;
        tag->owner = tags;
        leme_layout_init(&tag->layout, LEME_LAYOUT_DWINDLE);
        if (tags->server != NULL && tags->server->config != NULL) {
            struct leme_tag_settings settings;

            leme_config_tag_settings(tags->server->config, id, &settings);
            leme_tag_apply_settings(tag, &settings);
        }
        wl_list_init(&tag->views);
        plan->owned = true;
    }
    plan->tags = tags;
    plan->tag = tag;
    *slot = plan;
    return true;
}

struct leme_tag *
leme_tags_materialize_target(const struct leme_tag_materialize *plan)
{
    return plan == NULL ? NULL : plan->tag;
}

void
leme_tags_commit_materialize(struct leme_tag_materialize **slot)
{
    struct leme_tag_materialize *plan;

    assert(slot != NULL && *slot != NULL);
    plan = *slot;
    *slot = NULL;
    if (plan->owned) {
        assert(plan->tags->table[plan->tag->id] == NULL);
        plan->tags->table[plan->tag->id] = plan->tag;
        plan->owned = false;
    }
    free(plan);
}

void
leme_tags_discard_materialize(struct leme_tag_materialize **slot)
{
    struct leme_tag_materialize *plan;

    if (slot == NULL || *slot == NULL) {
        return;
    }
    plan = *slot;
    *slot = NULL;
    if (plan->owned) {
        leme_layout_finish(&plan->tag->layout);
        free(plan->tag);
    }
    free(plan);
}

void
leme_tags_attach_floating_prepared(struct leme_tag *tag,
    struct leme_view *view, bool focused)
{
    struct leme_tags *tags;

    assert(tag != NULL && view != NULL && view->floating &&
        leme_ownership_kind(view) == LEME_VIEW_OWNER_NONE);
    tags = tag->owner;
    leme_ownership_commit_tag(view, tag);
    wl_list_insert(&tag->views, &view->tag_link);
    if (focused) {
        tag->focused_view = view;
    }
    if (tags != NULL && tag->id == tags->focused_id) {
        tags->focused_is_candidate = false;
    }
}

bool
leme_tags_prepare_floating_attach(struct leme_tags *tags, uint16_t id)
{
    if (tags == NULL || id == 0 || id > tags->max_tags) {
        return false;
    }
    return materialize(tags, id) != NULL;
}

void
leme_tags_attach_floating(struct leme_tags *tags,
    struct leme_view *view, uint16_t id)
{
    struct leme_tag *tag;

    assert(tags != NULL && view != NULL && leme_ownership_tag(view) == NULL &&
        view->floating && id > 0 && id <= tags->max_tags &&
        tags->table[id] != NULL);
    tag = tags->table[id];
    leme_ownership_commit_tag(view, tag);
    wl_list_insert(&tag->views, &view->tag_link);
    tag->focused_view = view;
    if (id == tags->focused_id) {
        tags->focused_is_candidate = false;
    }
}

void
leme_tags_remove_view(struct leme_view *view)
{
    struct leme_tag *tag;
    struct leme_tags *tags;

    if (leme_ownership_tag(view) == NULL) {
        return;
    }
    tag = leme_ownership_tag(view);
    tags = tag->owner;
    if (!view->floating) {
        leme_layout_remove_view(&tag->layout, view);
    }
    if (tag->focused_view == view) {
        tag->focused_view = NULL;
    }
    wl_list_remove(&view->tag_link);
    wl_list_init(&view->tag_link);
    leme_ownership_release_tag(view, tag);
    if (tags != NULL) {
        (void)leme_tags_prune_empty(tags, tag);
    }
}

void
leme_tags_refresh_visibility(struct leme_tags *tags)
{
    struct leme_tag *current = tags->focused_is_candidate ? NULL :
        tags->table[tags->focused_id];
    struct leme_view *fullscreen = NULL;
    struct leme_view *focus = NULL;
    uint16_t id;
    struct wl_list *link;
    struct leme_view *view;

    if (current != NULL) {
        for (link = current->views.next;
                link != &current->views; link = link->next) {
            view = wl_container_of(link, view, tag_link);
            if (view->mapped && view->fullscreen) {
                fullscreen = view;
                break;
            }
        }
        if (current->focused_view != NULL &&
                current->focused_view->mapped &&
                (fullscreen == NULL || current->focused_view == fullscreen)) {
            focus = current->focused_view;
        }
    }
    for (id = 1; id <= tags->max_tags; id++) {
        if (tags->table[id] == NULL) {
            continue;
        }
        for (link = tags->table[id]->views.next;
                link != &tags->table[id]->views; link = link->next) {
            bool visible;

            view = wl_container_of(link, view, tag_link);
            visible = current == tags->table[id] &&
                (fullscreen == NULL || fullscreen == view);
            leme_render_set_view_visible(view, visible);
            if (visible && focus == NULL) {
                focus = view;
            }
        }
    }
    if (tags->server != NULL && tags->output != NULL &&
            tags->server->focused_output != tags->output) {
        return;
    }
    if (focus != NULL) {
        leme_view_focus(focus);
    } else if (tags->server != NULL &&
            !leme_layer_keyboard_is_exclusive(tags->server)) {
        leme_layer_release_for_view(tags->server);
        leme_view_clear_focus(tags->server);
    }
}

size_t
leme_tags_navigable(const struct leme_tags *tags,
    uint16_t *ids, size_t capacity)
{
    size_t count = 0;
    uint16_t id;

    if (tags == NULL || tags->table == NULL) {
        return 0;
    }
    for (id = 1; id <= tags->max_tags; id++) {
        bool navigable = tags->table[id] != NULL ||
            (tags->focused_is_candidate && tags->focused_id == id);

        if (!navigable) {
            continue;
        }
        if (ids != NULL && count < capacity) {
            ids[count] = id;
        }
        count++;
    }
    return count;
}

