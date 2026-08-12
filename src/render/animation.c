#include "render/animation.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>

struct leme_animation_hold {
    struct wlr_buffer *buffer;
    struct wl_listener destroy;
};

static void
leme_animation_hold_handle_destroy(struct wl_listener *listener, void *data)
{
    struct leme_animation_hold *hold =
        wl_container_of(listener, hold, destroy);

    (void)data;
    wl_list_remove(&hold->destroy.link);
    wlr_buffer_unlock(hold->buffer);
    free(hold);
}

static bool
leme_animation_hold_buffer(struct wlr_scene_buffer *node,
    struct wlr_buffer *buffer)
{
    struct leme_animation_hold *hold = calloc(1, sizeof(*hold));

    if (hold == NULL) {
        return false;
    }
    hold->buffer = wlr_buffer_lock(buffer);
    hold->destroy.notify = leme_animation_hold_handle_destroy;
    wl_signal_add(&node->node.events.destroy, &hold->destroy);
    return true;
}

struct leme_animation_copy_context {
    struct wlr_scene_tree *source;
    struct wlr_scene_tree *target;
};

static bool
leme_animation_copy_children(struct leme_animation_copy_context context)
{
    struct wlr_scene_node *node;

    wl_list_for_each(node, &context.source->children, link) {
        struct wlr_scene_node *copy = NULL;

        switch (node->type) {
        case WLR_SCENE_NODE_TREE: {
            struct wlr_scene_tree *tree = wl_container_of(node, tree, node);
            struct wlr_scene_tree *branch =
                wlr_scene_tree_create(context.target);

            if (branch == NULL ||
                    !leme_animation_copy_children(
                        (struct leme_animation_copy_context){
                            .source = tree,
                            .target = branch,
                        })) {
                return false;
            }
            copy = &branch->node;
            break;
        }
        case WLR_SCENE_NODE_RECT: {
            struct wlr_scene_rect *rect = wl_container_of(node, rect, node);
            struct wlr_scene_rect *branch = wlr_scene_rect_create(
                context.target, rect->width, rect->height, rect->color);

            if (branch == NULL) {
                return false;
            }
            copy = &branch->node;
            break;
        }
        case WLR_SCENE_NODE_BUFFER: {
            struct wlr_scene_buffer *buffer =
                wl_container_of(node, buffer, node);
            struct wlr_scene_buffer *branch;

            if (buffer->buffer == NULL) {
                continue;
            }
            branch = wlr_scene_buffer_create(
                context.target, buffer->buffer);
            if (branch == NULL ||
                    !leme_animation_hold_buffer(branch, buffer->buffer)) {
                return false;
            }
            wlr_scene_buffer_set_source_box(branch, &buffer->src_box);
            wlr_scene_buffer_set_dest_size(branch,
                buffer->dst_width, buffer->dst_height);
            wlr_scene_buffer_set_transform(branch, buffer->transform);
            wlr_scene_buffer_set_filter_mode(branch, buffer->filter_mode);
            wlr_scene_buffer_set_opacity(branch, buffer->opacity);
            copy = &branch->node;
            break;
        }
        }
        if (copy == NULL) {
            return false;
        }
        wlr_scene_node_set_position(copy, node->x, node->y);
        wlr_scene_node_set_enabled(copy, node->enabled);
    }
    return true;
}

struct wlr_scene_tree *
leme_animation_snapshot(struct wlr_scene_tree *source,
    struct wlr_scene_tree *parent)
{
    struct wlr_scene_tree *snapshot;

    if (source == NULL || parent == NULL) {
        return NULL;
    }
    snapshot = wlr_scene_tree_create(parent);
    if (snapshot == NULL) {
        return NULL;
    }
    if (!leme_animation_copy_children(
            (struct leme_animation_copy_context){
                .source = source,
                .target = snapshot,
            })) {
        leme_animation_snapshot_destroy(snapshot);
        return NULL;
    }
    wlr_scene_node_set_position(&snapshot->node,
        source->node.x, source->node.y);
    return snapshot;
}

void
leme_animation_snapshot_destroy(struct wlr_scene_tree *snapshot)
{
    if (snapshot != NULL) {
        wlr_scene_node_destroy(&snapshot->node);
    }
}

struct leme_animation {
    struct wl_list link;
    struct wlr_scene_tree *snapshot;
    struct leme_animation_subject subject;
    struct leme_animation_spec spec;
    struct timespec start;
    struct leme_animation *pending_next;
    bool finishing;
};

/* O instantâneo morre antes de done(): o sujeito nunca lhe toca depois. */
/* Quem chama já desligou esta animação do gestor. */
static void
leme_animation_release(struct leme_animation *animation)
{
    leme_animation_snapshot_destroy(animation->snapshot);
    if (animation->subject.done != NULL) {
        animation->subject.done(animation->subject.data);
    }
    free(animation);
}

static int
leme_animation_mix(int from, int to, double progress)
{
    const double mixed = (double)from +
        ((double)to - (double)from) * progress;

    if (isnan(mixed)) {
        return to;
    }
    if (mixed <= (double)INT_MIN) {
        return INT_MIN;
    }
    if (mixed >= (double)INT_MAX) {
        return INT_MAX;
    }
    return (int)mixed;
}

struct leme_animation_frame
leme_animation_frame_at(const struct leme_animation_spec *spec,
    double geometry_progress, double opacity_progress)
{
    return (struct leme_animation_frame){
        .box = {
            .x = leme_animation_mix(spec->from.x, spec->to.x,
                geometry_progress),
            .y = leme_animation_mix(spec->from.y, spec->to.y,
                geometry_progress),
            .width = leme_animation_mix(spec->from.width, spec->to.width,
                geometry_progress),
            .height = leme_animation_mix(spec->from.height, spec->to.height,
                geometry_progress),
        },
        .opacity = spec->from_opacity +
            (spec->to_opacity - spec->from_opacity) * opacity_progress,
    };
}

/* Um gestor a zeros conta como vazio: há testes que forjam o servidor. */
static bool
leme_animation_manager_ready(const struct leme_animation_manager *manager)
{
    return manager != NULL && manager->animations.next != NULL;
}

void
leme_animation_manager_init(struct leme_animation_manager *manager)
{
    wl_list_init(&manager->animations);
    manager->dispatching = false;
}

void
leme_animation_manager_finish(struct leme_animation_manager *manager)
{
    leme_animation_manager_finish_all(manager);
}

bool
leme_animation_manager_active(const struct leme_animation_manager *manager)
{
    return leme_animation_manager_ready(manager) &&
        !wl_list_empty(&manager->animations);
}

static struct leme_animation *
leme_animation_manager_take_finishing(
    struct leme_animation_manager *manager)
{
    struct leme_animation *animation;
    struct leme_animation *next;
    struct leme_animation *pending = NULL;
    struct leme_animation **pending_tail = &pending;

    /* O analisador não modela wl_list_remove() a actualizar a cabeça
     * intrusiva entre lotes; os testes de reentrância com ASan passam aqui. */
    wl_list_for_each_safe(animation, next, &manager->animations, link) { // NOLINT(clang-analyzer-unix.Malloc)
        if (animation->finishing) {
            wl_list_remove(&animation->link);
            animation->pending_next = NULL;
            *pending_tail = animation;
            pending_tail = &animation->pending_next;
        }
    }
    return pending;
}

static void
leme_animation_manager_drain(struct leme_animation_manager *manager)
{
    struct leme_animation *pending;

    if (!leme_animation_manager_ready(manager) || manager->dispatching) {
        return;
    }
    manager->dispatching = true;
    /* Desliga-se um lote inteiro antes de done(): um callback pode escolher
     * trabalho activo, mas não invalida outro nó da cadeia já desligada. */
    while ((pending =
            leme_animation_manager_take_finishing(manager)) != NULL) {
        while (pending != NULL) {
            struct leme_animation *animation = pending;

            pending = animation->pending_next;
            animation->pending_next = NULL;
            leme_animation_release(animation);
        }
    }
    manager->dispatching = false;
}

void
leme_animation_manager_finish_all(struct leme_animation_manager *manager)
{
    struct leme_animation *animation;

    if (!leme_animation_manager_ready(manager)) {
        return;
    }
    wl_list_for_each(animation, &manager->animations, link) {
        animation->finishing = true;
    }
    leme_animation_manager_drain(manager);
}

void
leme_animation_manager_finish_data(
    struct leme_animation_manager *manager, const void *data)
{
    struct leme_animation *animation;

    if (!leme_animation_manager_ready(manager) || data == NULL) {
        return;
    }
    wl_list_for_each(animation, &manager->animations, link) {
        if (animation->subject.data == data) {
            animation->finishing = true;
        }
    }
    leme_animation_manager_drain(manager);
}

void
leme_animation_manager_finish_owner(
    struct leme_animation_manager *manager, const void *owner)
{
    struct leme_animation *animation;

    if (!leme_animation_manager_ready(manager) || owner == NULL) {
        return;
    }
    wl_list_for_each(animation, &manager->animations, link) {
        if (animation->subject.owner == owner) {
            animation->finishing = true;
        }
    }
    leme_animation_manager_drain(manager);
}

/*
 * A posse do instantâneo passa para o motor. Se a animação não arrancar, o
 * caminho abandon destrói-o e corre done() na mesma: a restauração do
 * sujeito acontece exactamente uma vez em todos os caminhos.
 */
void
leme_animation_run(struct leme_animation_manager *manager,
    struct wlr_scene_tree *snapshot,
    const struct leme_animation_spec *spec,
    const struct leme_animation_subject *subject)
{
    struct leme_animation *animation;

    if (snapshot == NULL || spec == NULL || subject == NULL) {
        goto abandon;
    }
    if (!leme_animation_manager_ready(manager) || spec->duration_ms == 0) {
        goto abandon;
    }
    animation = calloc(1, sizeof(*animation));
    if (animation == NULL) {
        goto abandon;
    }
    animation->snapshot = snapshot;
    animation->spec = *spec;
    animation->subject = *subject;
    clock_gettime(CLOCK_MONOTONIC, &animation->start);
    wl_list_insert(&manager->animations, &animation->link);
    return;

abandon:
    leme_animation_snapshot_destroy(snapshot);
    if (subject != NULL && subject->done != NULL) {
        subject->done(subject->data);
    }
}

void
leme_animation_manager_tick(struct leme_animation_manager *manager,
    const struct timespec *now)
{
    struct leme_animation *animation;
    struct leme_animation *next;

    if (!leme_animation_manager_ready(manager) || now == NULL ||
            manager->dispatching) {
        return;
    }
    /* Um callback pode terminar-se a si ou a outra animação. Todos os nós
     * ficam vivos até a iteração parar, e só depois o esvaziamento corre
     * done() uma única vez. */
    manager->dispatching = true;
    wl_list_for_each_safe(animation, next, &manager->animations, link) {
        double linear;
        struct leme_animation_frame frame;
        double geometry;
        double opacity;

        if (animation->finishing) {
            continue;
        }
        linear = leme_animation_elapsed(&animation->start, now,
            animation->spec.duration_ms);
        if (linear >= 1.0) {
            animation->finishing = true;
            continue;
        }
        geometry = leme_animation_curve_at(&animation->spec.curve, linear);
        opacity = leme_animation_curve_at(&animation->spec.opacity_curve,
            linear);
        frame = leme_animation_frame_at(&animation->spec,
            geometry, opacity);
        if (animation->subject.apply != NULL) {
            animation->subject.apply(animation->subject.data, &frame);
        }
    }
    manager->dispatching = false;
    leme_animation_manager_drain(manager);
}

#define LEME_ANIMATION_BAKED_POINTS 64

struct leme_animation_curve
leme_animation_curve_preset(enum leme_animation_curve_preset preset)
{
    switch (preset) {
    case LEME_ANIMATION_CURVE_EASE_IN:
        return (struct leme_animation_curve){0.42, 0.0, 1.0, 1.0};
    case LEME_ANIMATION_CURVE_EASE_OUT:
        return (struct leme_animation_curve){0.0, 0.0, 0.58, 1.0};
    case LEME_ANIMATION_CURVE_EASE_IN_OUT:
        return (struct leme_animation_curve){0.42, 0.0, 0.58, 1.0};
    case LEME_ANIMATION_CURVE_LINEAR:
        break;
    }
    return (struct leme_animation_curve){0.0, 0.0, 1.0, 1.0};
}

static double
leme_animation_bezier(double a, double b, double t)
{
    double inverse = 1.0 - t;

    return 3.0 * t * inverse * inverse * a +
        3.0 * t * t * inverse * b + t * t * t;
}

/*
 * A curva é paramétrica em t, mas o tempo anda em x. Procura-se o t cujo
 * x bate com o progresso e devolve-se o y correspondente.
 */
double
leme_animation_curve_at(const struct leme_animation_curve *curve, double t)
{
    double low = 0.0;
    double high = 1.0;
    int step;

    if (t <= 0.0) {
        return 0.0;
    }
    if (t >= 1.0) {
        return 1.0;
    }
    for (step = 0; step < LEME_ANIMATION_BAKED_POINTS; step++) {
        double middle = (low + high) / 2.0;
        double x = leme_animation_bezier(curve->x1, curve->x2, middle);

        if (x < t) {
            low = middle;
        } else {
            high = middle;
        }
    }
    return leme_animation_bezier(curve->y1, curve->y2, (low + high) / 2.0);
}

double
leme_animation_elapsed(const struct timespec *start,
    const struct timespec *now, uint32_t duration_ms)
{
    double seconds;
    double progress;

    if (duration_ms == 0) {
        return 1.0;
    }
    seconds = (double)(now->tv_sec - start->tv_sec) +
        ((double)now->tv_nsec - (double)start->tv_nsec) / 1000000000.0;
    progress = seconds * 1000.0 / (double)duration_ms;
    if (progress <= 0.0) {
        return 0.0;
    }
    if (progress >= 1.0) {
        return 1.0;
    }
    return progress;
}
