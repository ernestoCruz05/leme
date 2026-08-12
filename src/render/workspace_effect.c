#include "render/workspace_effect.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>
#include <wlr/util/transform.h>

enum leme_workspace_leaf_kind {
    LEME_WORKSPACE_LEAF_RECT,
    LEME_WORKSPACE_LEAF_BUFFER,
};

struct leme_workspace_leaf {
    enum leme_workspace_leaf_kind kind;
    struct wlr_scene_node *node;
    int x;
    int y;
    bool enabled;
    union {
        struct {
            int width;
            int height;
            float color[4];
        } rect;
        struct {
            int width;
            int height;
            int dst_width;
            int dst_height;
            int buffer_width;
            int buffer_height;
            struct wlr_fbox src_box;
            enum wl_output_transform transform;
            float opacity;
        } buffer;
    } base;
};

struct leme_workspace_effect {
    struct wlr_scene_tree *outgoing;
    struct wlr_scene_tree *incoming;
    struct wl_array outgoing_leaves;
    struct wl_array incoming_leaves;
    struct leme_box viewport;
    enum leme_tag_change_direction direction;
    enum leme_workspace_animation_style style;
    size_t drawable_count;
    int travel;
};

static bool
leme_workspace_reject_input(struct wlr_scene_buffer *buffer,
    double *sx, double *sy)
{
    (void)buffer;
    (void)sx;
    (void)sy;
    return false;
}

static int
leme_workspace_offset(int origin, int direction, int distance)
{
    const int64_t value = (int64_t)origin +
        (int64_t)direction * (int64_t)distance;

    if (value < INT_MIN) {
        return INT_MIN;
    }
    if (value > INT_MAX) {
        return INT_MAX;
    }
    return (int)value;
}

static double
leme_workspace_opacity(double opacity)
{
    if (isnan(opacity) || opacity <= 0.0) {
        return 0.0;
    }
    if (opacity >= 1.0) {
        return 1.0;
    }
    return opacity;
}

static void
leme_workspace_buffer_dimensions(const struct wlr_scene_buffer *buffer,
    int *width, int *height)
{
    if (buffer->dst_width != 0 || buffer->dst_height != 0) {
        *width = buffer->dst_width;
        *height = buffer->dst_height;
        return;
    }
    *width = buffer->buffer->width;
    *height = buffer->buffer->height;
    wlr_output_transform_coords(buffer->transform, width, height);
}

static struct leme_workspace_leaf *
leme_workspace_leaf_add(struct wl_array *leaves)
{
    if (leaves->size > SIZE_MAX - sizeof(struct leme_workspace_leaf)) {
        return NULL;
    }
    return wl_array_add(leaves, sizeof(struct leme_workspace_leaf));
}

static bool
leme_workspace_collect_tree(struct wlr_scene_tree *tree,
    struct wl_array *leaves, size_t *drawable_count)
{
    struct wlr_scene_node *node;

    wl_list_for_each(node, &tree->children, link) {
        struct leme_workspace_leaf *leaf;

        if (node->type == WLR_SCENE_NODE_TREE) {
            struct wlr_scene_tree *child =
                wl_container_of(node, child, node);

            if (!leme_workspace_collect_tree(child, leaves,
                    drawable_count)) {
                return false;
            }
            continue;
        }
        if (node->type == WLR_SCENE_NODE_BUFFER) {
            struct wlr_scene_buffer *buffer =
                wl_container_of(node, buffer, node);

            if (buffer->buffer == NULL) {
                continue;
            }
            leaf = leme_workspace_leaf_add(leaves);
            if (leaf == NULL) {
                return false;
            }
            *leaf = (struct leme_workspace_leaf){
                .kind = LEME_WORKSPACE_LEAF_BUFFER,
                .node = node,
                .x = node->x,
                .y = node->y,
                .enabled = node->enabled,
                .base.buffer = {
                    .dst_width = buffer->dst_width,
                    .dst_height = buffer->dst_height,
                    .buffer_width = buffer->buffer->width,
                    .buffer_height = buffer->buffer->height,
                    .src_box = buffer->src_box,
                    .transform = buffer->transform,
                    .opacity = buffer->opacity,
                },
            };
            leme_workspace_buffer_dimensions(buffer,
                &leaf->base.buffer.width, &leaf->base.buffer.height);
            buffer->point_accepts_input = leme_workspace_reject_input;
            if (leaf->enabled && leaf->base.buffer.width > 0 &&
                    leaf->base.buffer.height > 0) {
                (*drawable_count)++;
            }
            continue;
        }
        if (node->type == WLR_SCENE_NODE_RECT) {
            struct wlr_scene_rect *rect =
                wl_container_of(node, rect, node);
            size_t index;

            leaf = leme_workspace_leaf_add(leaves);
            if (leaf == NULL) {
                return false;
            }
            *leaf = (struct leme_workspace_leaf){
                .kind = LEME_WORKSPACE_LEAF_RECT,
                .node = node,
                .x = node->x,
                .y = node->y,
                .enabled = node->enabled,
                .base.rect = {
                    .width = rect->width,
                    .height = rect->height,
                },
            };
            for (index = 0; index < 4; index++) {
                leaf->base.rect.color[index] = rect->color[index];
            }
            if (leaf->enabled && leaf->base.rect.width > 0 &&
                    leaf->base.rect.height > 0) {
                (*drawable_count)++;
            }
        }
    }
    return true;
}

struct leme_workspace_effect *
leme_workspace_effect_create(struct wlr_scene_tree *outgoing,
    struct wlr_scene_tree *incoming, struct leme_box viewport,
    enum leme_tag_change_direction direction,
    const struct leme_workspace_animation_settings *settings)
{
    struct leme_workspace_effect *effect;

    if (outgoing == NULL || incoming == NULL || settings == NULL ||
            viewport.width <= 0 || viewport.height <= 0 ||
            (direction != LEME_TAG_CHANGE_FORWARD &&
                direction != LEME_TAG_CHANGE_BACKWARD) ||
            (settings->style != LEME_WORKSPACE_ANIMATION_FULL_SLIDE &&
                settings->style != LEME_WORKSPACE_ANIMATION_GLIDE_FADE) ||
            !isfinite(settings->distance) || settings->distance < 0.0 ||
            settings->distance > 1.0) {
        return NULL;
    }
    effect = calloc(1, sizeof(*effect));
    if (effect == NULL) {
        return NULL;
    }
    effect->outgoing = outgoing;
    effect->incoming = incoming;
    effect->viewport = viewport;
    effect->direction = direction;
    effect->style = settings->style;
    effect->travel = settings->style == LEME_WORKSPACE_ANIMATION_FULL_SLIDE ?
        viewport.width : (int)((double)viewport.width * settings->distance);
    wl_array_init(&effect->outgoing_leaves);
    wl_array_init(&effect->incoming_leaves);
    if (!leme_workspace_collect_tree(outgoing, &effect->outgoing_leaves,
            &effect->drawable_count) ||
            !leme_workspace_collect_tree(incoming, &effect->incoming_leaves,
                &effect->drawable_count)) {
        leme_workspace_effect_destroy(effect);
        return NULL;
    }
    return effect;
}

void
leme_workspace_effect_destroy(struct leme_workspace_effect *effect)
{
    if (effect == NULL) {
        return;
    }
    wl_array_release(&effect->incoming_leaves);
    wl_array_release(&effect->outgoing_leaves);
    free(effect);
}

bool
leme_workspace_effect_has_content(const struct leme_workspace_effect *effect)
{
    return effect != NULL && effect->drawable_count > 0;
}

struct leme_animation_spec
leme_workspace_effect_animation_spec(
    const struct leme_workspace_effect *effect,
    const struct leme_workspace_animation_settings *settings)
{
    struct leme_animation_spec spec = {0};

    if (effect == NULL || settings == NULL) {
        return spec;
    }
    spec.from = effect->viewport;
    spec.to = effect->viewport;
    spec.to.x = leme_workspace_offset(effect->viewport.x,
        -(int)effect->direction, effect->travel);
    spec.from_opacity = 1.0;
    spec.to_opacity = effect->style == LEME_WORKSPACE_ANIMATION_GLIDE_FADE ?
        0.0 : 1.0;
    spec.duration_ms = settings->duration_ms;
    spec.curve = settings->curve;
    spec.opacity_curve = settings->opacity_curve;
    return spec;
}

static void
leme_workspace_clear_opaque_region(struct wlr_scene_buffer *buffer)
{
    pixman_region32_t empty;

    pixman_region32_init(&empty);
    wlr_scene_buffer_set_opaque_region(buffer, &empty);
    pixman_region32_fini(&empty);
}

static void
leme_workspace_restore_leaf(struct leme_workspace_leaf *leaf,
    double opacity)
{
    wlr_scene_node_set_position(leaf->node, leaf->x, leaf->y);
    wlr_scene_node_set_enabled(leaf->node, leaf->enabled);
    if (leaf->kind == LEME_WORKSPACE_LEAF_RECT) {
        struct wlr_scene_rect *rect =
            wl_container_of(leaf->node, rect, node);
        float color[4];
        size_t index;

        wlr_scene_rect_set_size(rect,
            leaf->base.rect.width, leaf->base.rect.height);
        for (index = 0; index < 4; index++) {
            color[index] = leaf->base.rect.color[index] * (float)opacity;
        }
        wlr_scene_rect_set_color(rect, color);
        return;
    }
    if (leaf->kind == LEME_WORKSPACE_LEAF_BUFFER) {
        struct wlr_scene_buffer *buffer =
            wl_container_of(leaf->node, buffer, node);
        const struct wlr_fbox *source =
            wlr_fbox_empty(&leaf->base.buffer.src_box) ? NULL :
            &leaf->base.buffer.src_box;

        wlr_scene_buffer_set_source_box(buffer, source);
        wlr_scene_buffer_set_dest_size(buffer,
            leaf->base.buffer.dst_width, leaf->base.buffer.dst_height);
        wlr_scene_buffer_set_opacity(buffer,
            leaf->base.buffer.opacity * (float)opacity);
        buffer->point_accepts_input = leme_workspace_reject_input;
        leme_workspace_clear_opaque_region(buffer);
    }
}

struct leme_workspace_clip {
    int left;
    int top;
    int width;
    int height;
};

static bool
leme_workspace_intersection(const struct leme_workspace_effect *effect,
    struct leme_box leaf, struct leme_workspace_clip *clip)
{
    const int64_t leaf_left = leaf.x;
    const int64_t leaf_top = leaf.y;
    const int64_t leaf_right = leaf_left + leaf.width;
    const int64_t leaf_bottom = leaf_top + leaf.height;
    const int64_t viewport_left = effect->viewport.x;
    const int64_t viewport_top = effect->viewport.y;
    const int64_t viewport_right = viewport_left + effect->viewport.width;
    const int64_t viewport_bottom = viewport_top + effect->viewport.height;
    const int64_t intersection_left = leaf_left > viewport_left ?
        leaf_left : viewport_left;
    const int64_t intersection_top = leaf_top > viewport_top ?
        leaf_top : viewport_top;
    const int64_t intersection_right = leaf_right < viewport_right ?
        leaf_right : viewport_right;
    const int64_t intersection_bottom = leaf_bottom < viewport_bottom ?
        leaf_bottom : viewport_bottom;

    if (intersection_right <= intersection_left ||
            intersection_bottom <= intersection_top) {
        return false;
    }
    *clip = (struct leme_workspace_clip){
        .left = (int)(intersection_left - leaf_left),
        .top = (int)(intersection_top - leaf_top),
        .width = (int)(intersection_right - intersection_left),
        .height = (int)(intersection_bottom - intersection_top),
    };
    return true;
}

static void
leme_workspace_clip_buffer(struct leme_workspace_leaf *leaf,
    const struct leme_workspace_clip *clip)
{
    struct wlr_scene_buffer *buffer =
        wl_container_of(leaf->node, buffer, node);
    struct wlr_fbox source = leaf->base.buffer.src_box;
    struct wlr_fbox oriented;
    struct wlr_fbox cropped;
    int transformed_width = leaf->base.buffer.buffer_width;
    int transformed_height = leaf->base.buffer.buffer_height;

    if (wlr_fbox_empty(&source)) {
        source = (struct wlr_fbox){
            .width = leaf->base.buffer.buffer_width,
            .height = leaf->base.buffer.buffer_height,
        };
    }
    wlr_fbox_transform(&oriented, &source, leaf->base.buffer.transform,
        (double)leaf->base.buffer.buffer_width,
        (double)leaf->base.buffer.buffer_height);
    cropped = (struct wlr_fbox){
        .x = oriented.x + (double)clip->left * oriented.width /
            (double)leaf->base.buffer.width,
        .y = oriented.y + (double)clip->top * oriented.height /
            (double)leaf->base.buffer.height,
        .width = (double)clip->width * oriented.width /
            (double)leaf->base.buffer.width,
        .height = (double)clip->height * oriented.height /
            (double)leaf->base.buffer.height,
    };
    wlr_output_transform_coords(leaf->base.buffer.transform,
        &transformed_width, &transformed_height);
    wlr_fbox_transform(&source, &cropped,
        wlr_output_transform_invert(leaf->base.buffer.transform),
        (double)transformed_width, (double)transformed_height);
    wlr_scene_buffer_set_source_box(buffer, &source);
    wlr_scene_buffer_set_dest_size(buffer, clip->width, clip->height);
}

static void
leme_workspace_apply_leaf(struct leme_workspace_effect *effect,
    struct leme_workspace_leaf *leaf, double opacity)
{
    struct leme_box box = {0};
    struct leme_workspace_clip clip = {0};

    leme_workspace_restore_leaf(leaf, opacity);
    if (!leaf->enabled) {
        return;
    }
    if (leaf->kind == LEME_WORKSPACE_LEAF_RECT) {
        box.width = leaf->base.rect.width;
        box.height = leaf->base.rect.height;
    } else {
        box.width = leaf->base.buffer.width;
        box.height = leaf->base.buffer.height;
    }
    if (box.width <= 0 || box.height <= 0 ||
            !wlr_scene_node_coords(leaf->node, &box.x, &box.y) ||
            !leme_workspace_intersection(effect, box, &clip)) {
        wlr_scene_node_set_enabled(leaf->node, false);
        return;
    }
    if (clip.left == 0 && clip.top == 0 &&
            clip.width == box.width && clip.height == box.height) {
        return;
    }
    wlr_scene_node_set_position(leaf->node,
        leme_workspace_offset(leaf->x, 1, clip.left),
        leme_workspace_offset(leaf->y, 1, clip.top));
    if (leaf->kind == LEME_WORKSPACE_LEAF_RECT) {
        struct wlr_scene_rect *rect =
            wl_container_of(leaf->node, rect, node);

        wlr_scene_rect_set_size(rect, clip.width, clip.height);
    } else {
        leme_workspace_clip_buffer(leaf, &clip);
    }
}

static void
leme_workspace_apply_leaves(struct leme_workspace_effect *effect,
    struct wl_array *leaves, double opacity)
{
    struct leme_workspace_leaf *leaf;

    wl_array_for_each(leaf, leaves) {
        leme_workspace_apply_leaf(effect, leaf, opacity);
    }
}

void
leme_workspace_effect_apply(struct leme_workspace_effect *effect,
    const struct leme_animation_frame *frame)
{
    double outgoing_opacity = 1.0;
    double incoming_opacity = 1.0;

    if (effect == NULL || frame == NULL) {
        return;
    }
    if (effect->style == LEME_WORKSPACE_ANIMATION_GLIDE_FADE) {
        outgoing_opacity = leme_workspace_opacity(frame->opacity);
        incoming_opacity = 1.0 - outgoing_opacity;
    }
    wlr_scene_node_set_position(&effect->outgoing->node,
        frame->box.x, frame->box.y);
    wlr_scene_node_set_position(&effect->incoming->node,
        leme_workspace_offset(frame->box.x, (int)effect->direction,
            effect->travel),
        frame->box.y);
    leme_workspace_apply_leaves(effect, &effect->outgoing_leaves,
        outgoing_opacity);
    leme_workspace_apply_leaves(effect, &effect->incoming_leaves,
        incoming_opacity);
}
