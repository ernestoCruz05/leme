#include "render/render.h"

#include "config/config.h"
#include "core/server.h"
#include "output/output.h"
#include "protocols/capture.h"
#include "protocols/session.h"
#include "shell/rules.h"
#include "shell/view.h"
#include "workspace/tag.h"
#include "render/workspace_transition.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/xwayland/xwayland.h>

static struct wlr_scene_tree *
leme_render_view_parent(const struct leme_view *view)
{
    return view->scratchpad_state == LEME_SCRATCHPAD_SHOWN ||
        view->floating || view->detached || view->fullscreen ?
        view->server->scene_floating : view->server->scene_tiled;
}

struct leme_render_surface_tracker {
    struct leme_render_view_state *state;
    struct wlr_surface *surface;
    struct wlr_subsurface *subsurface;
    struct wl_list children;
    struct wl_list link;
    struct wl_listener commit;
    struct wl_listener map;
    struct wl_listener destroy;
    struct wl_listener new_subsurface;
    struct wl_listener subsurface_destroy;
};

struct leme_render_view_state {
    struct leme_view *view;
    struct wl_list roots;
};

struct leme_render_view_content_base {
    struct wlr_scene_node *node;
    int x;
    int y;
    int width;
    int height;
    float opacity;
};

struct leme_render_view_animation {
    struct leme_view *view;
    struct leme_output *output;
    struct wlr_scene_tree *root;
    struct leme_render_view_frame_nodes nodes;
    struct wl_array content_bases;
    float border_color[4][4];
    int border_width;
    int final_content_width;
    int final_content_height;
    bool restore_on_done;
};

static void leme_render_view_animation_begin(struct leme_view *view,
    const struct leme_animation_settings *settings, bool opening);
static bool leme_render_surface_tracker_create(
    struct leme_render_view_state *state,
    struct leme_render_surface_tracker *parent,
    struct wlr_surface *surface, struct wlr_subsurface *owner);
static void leme_render_surface_tracker_finish(
    struct leme_render_surface_tracker *tracker);
static bool leme_render_view_tracking_init(struct leme_view *view,
    struct wlr_surface *surface);
static bool leme_render_view_track_root(struct leme_view *view,
    struct wlr_surface *surface);
static void leme_render_view_untrack_root(struct leme_view *view,
    struct wlr_surface *surface);
static void leme_render_view_tracking_finish(struct leme_view *view);
static void leme_render_view_apply_surface_effects(struct leme_view *view,
    int corner_radius, int border_width, int blur);

void
leme_render_view_create(struct leme_view *view)
{
    static const float fallback[4] = {0.16f, 0.42f, 0.72f, 1.0f};
    const float *border_color = view->server != NULL &&
        view->server->config != NULL ?
        view->server->config->border_inactive : fallback;
    struct wlr_surface *surface = NULL;
    size_t index;

    view->render_tree = wlr_scene_tree_create(leme_render_view_parent(view));
    if (view->render_tree == NULL) {
        return;
    }
    view->render_tree->node.data = view;
    for (index = 0; index < LEME_ARRAY_LENGTH(view->border); index++) {
        view->border[index] = wlr_scene_rect_create(
            view->render_tree, 1, 1, border_color);
        if (view->border[index] == NULL) {
            leme_render_view_destroy(view);
            return;
        }
    }
    if (view->kind == LEME_VIEW_XDG) {
        surface = view->xdg_toplevel->base->surface;
        view->scene_tree = wlr_scene_xdg_surface_create(
            view->render_tree, view->xdg_toplevel->base);
        if (view->scene_tree != NULL) {
            view->xdg_toplevel->base->data = view->scene_tree;
        }
    } else {
        surface = view->xwayland_surface == NULL ? NULL :
            view->xwayland_surface->surface;
        if (!leme_render_xwayland_create(view)) {
            view->scene_tree = NULL;
        }
    }
    /* A cena regista primeiro os listeners de commit existentes. */
    if (view->scene_tree == NULL ||
            !leme_render_view_tracking_init(view, surface)) {
        leme_render_view_destroy(view);
        return;
    }
    leme_session_refresh_idle_inhibitors(view->server);
}

#define LEME_ANIMATION_OPEN_TIMEOUT_MS 200

static void
leme_render_view_cancel_open_timeout(struct leme_view *view)
{
    view->open_animation_pending = false;
    if (view->open_animation_timeout != NULL) {
        wl_event_source_remove(view->open_animation_timeout);
        view->open_animation_timeout = NULL;
    }
}

static bool
leme_render_view_animation_allowed(const struct leme_view *view)
{
    /* Um destino mascarado continua vivo para frames e commits, mas não
     * pode acrescentar um instantâneo de janela por cima da composição da
     * área de trabalho. */
    return !view->unmanaged && !view->fullscreen &&
        !leme_render_workspace_transition_hides_view(view) &&
        (view->server->session_protocols == NULL ||
            !view->server->session_protocols->locked);
}

static void
leme_render_view_start_open(struct leme_view *view)
{
    leme_render_view_cancel_open_timeout(view);
    if (view->render_tree == NULL) {
        return;
    }
    /* O estado pode ter mudado enquanto esperávamos pela confirmação. */
    leme_render_view_sync_presentation(view);
    if (!leme_render_view_animation_allowed(view)) {
        wlr_scene_node_set_enabled(&view->render_tree->node, true);
        return;
    }
    wlr_scene_node_set_enabled(&view->render_tree->node, false);
    leme_render_view_animation_begin(view,
        &view->server->config->animation[LEME_ANIMATION_OPEN], true);
}

/* Um cliente que nunca mais confirma não pode ficar invisível. */
static int
leme_render_view_handle_open_timeout(void *data)
{
    struct leme_view *view = data;

    leme_render_view_cancel_open_timeout(view);
    if (view->render_tree != NULL) {
        leme_render_view_sync_presentation(view);
        wlr_scene_node_set_enabled(&view->render_tree->node, true);
    }
    return 0;
}

/*
 * Só depois de o cliente confirmar o configure é que a vista mostra o
 * tamanho que lhe demos; copiá-la antes disso capta o buffer inicial.
 */
static bool
leme_render_view_open_settled(const struct leme_view *view)
{
    const struct wlr_xdg_surface *base;

    if (view->kind != LEME_VIEW_XDG || view->xdg_toplevel == NULL) {
        return true;
    }
    base = view->xdg_toplevel->base;
    return base->current.configure_serial >= view->open_animation_serial;
}

void
leme_render_view_open_ready(struct leme_view *view)
{
    if (view == NULL || !view->open_animation_pending ||
            !leme_render_view_open_settled(view)) {
        return;
    }
    leme_render_view_start_open(view);
}

void
leme_render_view_animate(struct leme_view *view,
    enum leme_animation_event event)
{
    const struct leme_animation_settings *settings;

    if (view == NULL || view->server == NULL ||
            view->server->config == NULL || view->render_tree == NULL) {
        return;
    }
    /* Largar a animação anterior antes de a vista poder desaparecer. */
    if (view->animation_state != NULL) {
        leme_animation_manager_finish_data(&view->server->animations,
            view->animation_state);
    }
    leme_render_view_cancel_open_timeout(view);
    if (!leme_render_view_animation_allowed(view) ||
            !view->render_tree->node.enabled) {
        return;
    }
    settings = &view->server->config->animation[event];
    if (!settings->configured) {
        return;
    }
    if (event != LEME_ANIMATION_OPEN) {
        leme_render_view_animation_begin(view, settings, false);
        return;
    }
    /*
     * A vista ainda mostra o primeiro buffer do cliente, não o tamanho que
     * acabámos de configurar. Esperar pela confirmação antes de a copiar.
     */
    view->open_animation_pending = true;
    view->open_animation_serial = view->kind == LEME_VIEW_XDG &&
        view->xdg_toplevel != NULL ?
        view->xdg_toplevel->base->scheduled_serial : 0;
    leme_render_view_sync_presentation(view);
    view->open_animation_timeout = wl_event_loop_add_timer(
        wl_display_get_event_loop(view->server->display),
        leme_render_view_handle_open_timeout, view);
    if (view->open_animation_timeout == NULL) {
        leme_render_view_start_open(view);
        return;
    }
    wl_event_source_timer_update(view->open_animation_timeout,
        LEME_ANIMATION_OPEN_TIMEOUT_MS);
}

void
leme_render_view_finish_animation(struct leme_view *view)
{
    if (view == NULL) {
        return;
    }
    leme_render_view_cancel_open_timeout(view);
    if (view->server != NULL && view->animation_state != NULL) {
        leme_animation_manager_finish_data(&view->server->animations,
            view->animation_state);
    }
    leme_render_view_sync_presentation(view);
}

void
leme_render_view_destroy(struct leme_view *view)
{
    struct leme_render_view_animation *state = view->animation_state;

    /* A animação de fecho sobrevive à vista: fica sem quem restaurar. */
    if (state != NULL) {
        state->view = NULL;
        view->animation_state = NULL;
    }
    leme_render_view_cancel_open_timeout(view);
    leme_capture_invalidate_view(view);
    leme_render_view_tracking_finish(view);
    if (view->kind == LEME_VIEW_XDG && view->xdg_toplevel != NULL) {
        view->xdg_toplevel->base->data = NULL;
        if (view->render_tree != NULL) {
            wlr_scene_node_destroy(&view->render_tree->node);
        }
    } else if (view->kind == LEME_VIEW_XWAYLAND) {
        leme_render_xwayland_destroy(view);
    }
    view->render_tree = NULL;
    view->scene_tree = NULL;
    view->drop_preview = NULL;
    memset(view->border, 0, sizeof(view->border));
    leme_session_refresh_idle_inhibitors(view->server);
}

bool
leme_render_view_popup_create(struct leme_view_popup *popup)
{
    struct wlr_xdg_surface *parent =
        wlr_xdg_surface_try_from_wlr_surface(popup->wlr_popup->parent);
    struct wlr_scene_tree *parent_tree =
        parent == NULL ? NULL : parent->data;

    if (parent_tree == NULL) {
        return false;
    }
    popup->scene_tree = wlr_scene_xdg_surface_create(
        parent_tree, popup->wlr_popup->base);
    /* Repõe a opacidade do popup depois dos listeners da cena. */
    if (popup->scene_tree == NULL ||
            !leme_render_view_track_root(
                popup->view, popup->wlr_popup->base->surface)) {
        if (popup->scene_tree != NULL) {
            wlr_scene_node_destroy(&popup->scene_tree->node);
            popup->scene_tree = NULL;
        }
        return false;
    }
    popup->wlr_popup->base->data = popup->scene_tree;
    leme_session_refresh_idle_inhibitors(popup->view->server);
    return true;
}

static int
leme_render_saturate_coordinate(int64_t value)
{
    if (value > INT_MAX) {
        return INT_MAX;
    }
    if (value < INT_MIN) {
        return INT_MIN;
    }
    return (int)value;
}

struct leme_box
leme_render_view_popup_constraint_box(
    struct leme_box full, int view_x, int view_y,
    struct leme_box root_geometry)
{
    full.x = leme_render_saturate_coordinate(
        (int64_t)full.x - view_x + root_geometry.x);
    full.y = leme_render_saturate_coordinate(
        (int64_t)full.y - view_y + root_geometry.y);
    return full;
}

void
leme_render_view_popup_unconstrain(struct leme_view_popup *popup,
    struct leme_box full)
{
    const struct wlr_box root =
        popup->view->xdg_toplevel->base->geometry;
    struct leme_box converted;
    struct wlr_box constraint;
    int view_x;
    int view_y;

    if (popup->view->scene_tree == NULL ||
            !wlr_scene_node_coords(&popup->view->scene_tree->node,
                &view_x, &view_y)) {
        return;
    }
    converted = leme_render_view_popup_constraint_box(
        full, view_x, view_y, (struct leme_box){
            .x = root.x,
            .y = root.y,
            .width = root.width,
            .height = root.height,
        });
    constraint = (struct wlr_box){
        .x = converted.x,
        .y = converted.y,
        .width = converted.width,
        .height = converted.height,
    };
    wlr_xdg_popup_unconstrain_from_box(popup->wlr_popup, &constraint);
}

void
leme_render_view_popup_destroy(struct leme_view_popup *popup)
{
    leme_render_view_untrack_root(
        popup->view, popup->wlr_popup->base->surface);
    popup->wlr_popup->base->data = NULL;
    if (popup->scene_tree != NULL) {
        wlr_scene_node_destroy(&popup->scene_tree->node);
        popup->scene_tree = NULL;
    }
    leme_session_refresh_idle_inhibitors(popup->view->server);
}

void
leme_render_set_view_visible(struct leme_view *view, bool visible)
{
    if (view->render_tree != NULL) {
        wlr_scene_node_set_enabled(&view->render_tree->node, visible);
    }
    leme_render_view_sync_presentation(view);
    leme_session_refresh_idle_inhibitors(view->server);
}

static void
leme_render_view_apply_opacity(struct wlr_scene_buffer *buffer,
    int sx, int sy, void *data)
{
    const float *opacity = data;

    (void)sx;
    (void)sy;
    wlr_scene_buffer_set_opacity(buffer, *opacity);
}

/*
 * Enquanto a apresentação está mascarada, a vista fica transparente em vez
 * de desactivada. Assim continua a receber frames e commits sem aparecer por
 * cima do instantâneo que representa a transição.
 */
static bool
leme_render_view_presentation_hidden(const struct leme_view *view)
{
    return view->open_animation_pending ||
        leme_render_workspace_transition_hides_view(view);
}

static float
leme_render_view_effective_opacity(const struct leme_view *view)
{
    return leme_render_view_presentation_hidden(view) ?
        0.0f : view->render_opacity;
}

static void
leme_render_view_apply_cached_opacity(struct leme_view *view)
{
    float opacity;

    if (view == NULL || view->scene_tree == NULL) {
        return;
    }
    opacity = leme_render_view_effective_opacity(view);
    wlr_scene_node_for_each_buffer(&view->scene_tree->node,
        leme_render_view_apply_opacity, &opacity);
}

/*
 * Esconder com set_enabled(false) corta os frame callbacks ao cliente, e um
 * cliente que só desenha neles nunca confirma o configure: a espera pela
 * confirmação impedia-a. A vista fica visível para o wlroots e transparente
 * para quem olha. As molduras são rectângulos e não precisam de frames.
 */
void
leme_render_view_sync_presentation(struct leme_view *view)
{
    const bool hidden = view != NULL &&
        leme_render_view_presentation_hidden(view);
    size_t index;

    leme_render_view_apply_cached_opacity(view);
    if (view == NULL) {
        return;
    }
    for (index = 0; index < LEME_ARRAY_LENGTH(view->border); index++) {
        if (view->border[index] != NULL) {
            wlr_scene_node_set_enabled(&view->border[index]->node, !hidden);
        }
    }
}

struct leme_render_surface_opacity {
    struct wlr_surface *surface;
    float opacity;
};

static void
leme_render_view_apply_surface_opacity(struct wlr_scene_buffer *buffer,
    int sx, int sy, void *data)
{
    const struct leme_render_surface_opacity *target = data;
    struct wlr_scene_surface *scene_surface =
        wlr_scene_surface_try_from_buffer(buffer);

    (void)sx;
    (void)sy;
    if (scene_surface != NULL && scene_surface->surface == target->surface) {
        wlr_scene_buffer_set_opacity(buffer, target->opacity);
    }
}

static void
leme_render_view_apply_cached_surface_opacity(
    struct leme_view *view, struct wlr_surface *surface)
{
    struct leme_render_surface_opacity target = {
        .surface = surface,
        .opacity = leme_render_view_effective_opacity(view),
    };

    if (view->scene_tree != NULL) {
        wlr_scene_node_for_each_buffer(&view->scene_tree->node,
            leme_render_view_apply_surface_opacity, &target);
    }
}

static void
leme_render_surface_tracker_finish(
    struct leme_render_surface_tracker *tracker)
{
    struct leme_render_surface_tracker *child;
    struct leme_render_surface_tracker *tmp;

    wl_list_for_each_safe(child, tmp, &tracker->children, link) {
        leme_render_surface_tracker_finish(child);
    }
    wl_list_remove(&tracker->commit.link);
    wl_list_remove(&tracker->map.link);
    wl_list_remove(&tracker->destroy.link);
    wl_list_remove(&tracker->new_subsurface.link);
    if (tracker->subsurface != NULL) {
        wl_list_remove(&tracker->subsurface_destroy.link);
    }
    wl_list_remove(&tracker->link);
    free(tracker);
}

static void
leme_render_surface_tracker_handle_commit(
    struct wl_listener *listener, void *data)
{
    struct leme_render_surface_tracker *tracker =
        wl_container_of(listener, tracker, commit);

    (void)data;
    leme_render_view_apply_cached_surface_opacity(
        tracker->state->view, tracker->surface);
    if (tracker->subsurface == NULL) {
        leme_render_view_open_ready(tracker->state->view);
    }
}

static void
leme_render_surface_tracker_handle_map(
    struct wl_listener *listener, void *data)
{
    struct leme_render_surface_tracker *tracker =
        wl_container_of(listener, tracker, map);

    (void)data;
    leme_render_view_apply_cached_surface_opacity(
        tracker->state->view, tracker->surface);
}

static void
leme_render_surface_tracker_handle_destroy(
    struct wl_listener *listener, void *data)
{
    struct leme_render_surface_tracker *tracker =
        wl_container_of(listener, tracker, destroy);

    (void)data;
    leme_render_surface_tracker_finish(tracker);
}

static void
leme_render_surface_tracker_handle_subsurface_destroy(
    struct wl_listener *listener, void *data)
{
    struct leme_render_surface_tracker *tracker =
        wl_container_of(listener, tracker, subsurface_destroy);

    (void)data;
    leme_render_surface_tracker_finish(tracker);
}

static void
leme_render_surface_tracker_handle_new_subsurface(
    struct wl_listener *listener, void *data)
{
    struct leme_render_surface_tracker *tracker =
        wl_container_of(listener, tracker, new_subsurface);
    struct wlr_subsurface *subsurface = data;

    /* O listener anterior da cena já criou a superfície filha. */
    if (!leme_render_surface_tracker_create(
            tracker->state, tracker, subsurface->surface, subsurface)) {
        wl_resource_post_no_memory(subsurface->resource);
    }
}

static bool
leme_render_surface_tracker_create(
    struct leme_render_view_state *state,
    struct leme_render_surface_tracker *parent,
    struct wlr_surface *surface, struct wlr_subsurface *owner)
{
    struct leme_render_surface_tracker *tracker =
        calloc(1, sizeof(*tracker));
    struct wlr_subsurface *subsurface;

    if (tracker == NULL) {
        return false;
    }
    tracker->state = state;
    tracker->surface = surface;
    tracker->subsurface = owner;
    wl_list_init(&tracker->children);
    wl_list_insert(parent == NULL ? &state->roots : &parent->children,
        &tracker->link);
    tracker->commit.notify = leme_render_surface_tracker_handle_commit;
    wl_signal_add(&surface->events.commit, &tracker->commit);
    tracker->map.notify = leme_render_surface_tracker_handle_map;
    wl_signal_add(&surface->events.map, &tracker->map);
    tracker->destroy.notify = leme_render_surface_tracker_handle_destroy;
    wl_signal_add(&surface->events.destroy, &tracker->destroy);
    tracker->new_subsurface.notify =
        leme_render_surface_tracker_handle_new_subsurface;
    wl_signal_add(&surface->events.new_subsurface,
        &tracker->new_subsurface);
    if (owner != NULL) {
        tracker->subsurface_destroy.notify =
            leme_render_surface_tracker_handle_subsurface_destroy;
        wl_signal_add(&owner->events.destroy,
            &tracker->subsurface_destroy);
    }

    wl_list_for_each(subsurface, &surface->current.subsurfaces_below,
            current.link) {
        if (!leme_render_surface_tracker_create(
                state, tracker, subsurface->surface, subsurface)) {
            leme_render_surface_tracker_finish(tracker);
            return false;
        }
    }
    wl_list_for_each(subsurface, &surface->current.subsurfaces_above,
            current.link) {
        if (!leme_render_surface_tracker_create(
                state, tracker, subsurface->surface, subsurface)) {
            leme_render_surface_tracker_finish(tracker);
            return false;
        }
    }
    leme_render_view_apply_cached_surface_opacity(state->view, surface);
    return true;
}

static bool
leme_render_view_tracking_init(
    struct leme_view *view, struct wlr_surface *surface)
{
    struct leme_render_view_state *state;

    if (view == NULL || surface == NULL || view->render_state != NULL) {
        return false;
    }
    state = calloc(1, sizeof(*state));
    if (state == NULL) {
        return false;
    }
    state->view = view;
    wl_list_init(&state->roots);
    view->render_state = state;
    if (!leme_render_surface_tracker_create(
            state, NULL, surface, NULL)) {
        leme_render_view_tracking_finish(view);
        return false;
    }
    return true;
}

static bool
leme_render_view_track_root(
    struct leme_view *view, struct wlr_surface *surface)
{
    return view != NULL && view->render_state != NULL && surface != NULL &&
        leme_render_surface_tracker_create(
            view->render_state, NULL, surface, NULL);
}

static void
leme_render_view_untrack_root(
    struct leme_view *view, struct wlr_surface *surface)
{
    struct leme_render_surface_tracker *tracker;
    struct leme_render_surface_tracker *tmp;

    if (view == NULL || view->render_state == NULL || surface == NULL) {
        return;
    }
    wl_list_for_each_safe(
            tracker, tmp, &view->render_state->roots, link) {
        if (tracker->surface == surface) {
            leme_render_surface_tracker_finish(tracker);
            return;
        }
    }
}

static void
leme_render_view_tracking_finish(struct leme_view *view)
{
    struct leme_render_surface_tracker *tracker;
    struct leme_render_surface_tracker *tmp;

    if (view == NULL || view->render_state == NULL) {
        return;
    }
    wl_list_for_each_safe(
            tracker, tmp, &view->render_state->roots, link) {
        leme_render_surface_tracker_finish(tracker);
    }
    free(view->render_state);
    view->render_state = NULL;
}

void
leme_render_view_set_activated(struct leme_view *view, bool activated)
{
    const struct leme_config *config;
    const float *color;
    struct leme_view_rules rules;
    size_t index;

    if (view == NULL || view->render_tree == NULL ||
            view->server == NULL || view->server->config == NULL) {
        return;
    }
    config = view->server->config;
    rules = leme_view_rules_match(config,
        leme_view_identity(view), leme_view_title(view));
    view->render_opacity = leme_render_view_opacity(
        config, activated, view->fullscreen, &rules);
    color = activated ? config->border_active : config->border_inactive;
    for (index = 0; index < LEME_ARRAY_LENGTH(view->border); index++) {
        if (view->border[index] != NULL) {
            wlr_scene_rect_set_color(view->border[index], color);
        }
    }
    leme_render_view_sync_presentation(view);
}

float
leme_render_view_opacity(const struct leme_config *config, bool activated,
    bool fullscreen, const struct leme_view_rules *rules)
{
    if (config == NULL || fullscreen) {
        return 1.0f;
    }
    if (rules != NULL && (rules->fields & LEME_WINDOW_RULE_OPACITY) != 0) {
        return (float)rules->opacity;
    }
    return (float)(activated ?
        config->opacity_active : config->opacity_inactive);
}

/*
 * Um scratchpad exposto não tem etiqueta e tem de ficar acima da composição
 * flutuante e de etiquetas na saída onde aparece. A raiz flutuante continua
 * abaixo das raízes top e overlay do compositor.
 */
void
leme_render_workspace_transition_restack_scratchpad(
    struct leme_output *output)
{
    struct leme_scratchpad_manager *manager;
    struct leme_view *view;

    if (output == NULL || output->server == NULL) {
        return;
    }
    manager = &output->server->scratchpads;
    view = manager->shown;
    if (manager->server != output->server || manager->shown_output != output ||
            view == NULL || view->scratchpad_state != LEME_SCRATCHPAD_SHOWN ||
            view->render_tree == NULL) {
        return;
    }
    wlr_scene_node_raise_to_top(&view->render_tree->node);
}

void
leme_render_view_focus(struct leme_view *view)
{
    struct leme_output *output;

    if (view == NULL || view->render_tree == NULL) {
        return;
    }
    wlr_scene_node_raise_to_top(&view->render_tree->node);
    output = view->tag == NULL || view->tag->owner == NULL ? NULL :
        view->tag->owner->output;
    leme_render_workspace_transition_restack_scratchpad(output);
}

void
leme_render_view_update_layer(struct leme_view *view)
{
    struct wlr_scene_tree *parent = leme_render_view_parent(view);

    if (view->render_tree != NULL && view->render_tree->node.parent != parent) {
        wlr_scene_node_reparent(&view->render_tree->node, parent);
    }
}

/* A espessura nunca é escalada; só cede quando a caixa não a comporta. */
static int
leme_render_view_clamp_border(int border_width, struct leme_box frame)
{
    int maximum = frame.width < frame.height ? frame.width : frame.height;

    maximum = maximum > 0 ? (maximum - 1) / 2 : 0;
    return border_width < maximum ? border_width : maximum;
}

static int
leme_render_view_border_width(const struct leme_view *view,
    struct leme_box frame)
{
    if (view->fullscreen || view->unmanaged || view->server->config == NULL) {
        return 0;
    }
    return leme_render_view_clamp_border(
        view->server->config->border_width, frame);
}

/*
 * O raio nunca passa de metade do lado mais curto: acima disso a caixa com
 * sinal inverte-se e o canto deixa de fechar.
 */
int
leme_render_view_corner_radius(const struct leme_view *view,
    struct leme_box frame)
{
    int limit;

    /* A animação de fecho sobrevive à vista e continua a pedir o raio. */
    if (view == NULL || view->fullscreen || view->unmanaged ||
            view->server == NULL || view->server->config == NULL) {
        return 0;
    }
    limit = frame.width < frame.height ? frame.width : frame.height;
    limit /= 2;
    return view->server->config->corner_radius < limit ?
        view->server->config->corner_radius : limit;
}

/*
 * Só decora janelas geridas, tal como o raio. Se o desfoque chega a ver-se
 * decide-se na cena, a partir da região opaca do buffer: uma janela a toda a
 * opacidade cujo cliente desenhe a sua própria transparência ainda o quer, e
 * daqui não se sabe isso.
 */
int
leme_render_view_blur(const struct leme_view *view)
{
    /* A animação de fecho sobrevive à vista e continua a pedir o desfoque. */
    if (view == NULL || view->fullscreen || view->unmanaged ||
            view->server == NULL || view->server->config == NULL) {
        return 0;
    }
    return view->server->config->blur;
}

struct leme_box
leme_render_view_content_box(const struct leme_view *view,
    struct leme_box frame)
{
    int border_width = leme_render_view_border_width(view, frame);

    frame.x += border_width;
    frame.y += border_width;
    frame.width -= border_width * 2;
    frame.height -= border_width * 2;
    return frame;
}

/*
 * Cada nó do conteúdo guarda a sua medida final: o rácio de cada frame é
 * aplicado sempre à base e nunca ao que sobrou do frame anterior.
 */
static bool
leme_render_view_collect_content(struct wl_array *bases,
    struct wlr_scene_tree *tree)
{
    struct wlr_scene_node *node;

    if (tree == NULL) {
        return true;
    }
    wl_list_for_each(node, &tree->children, link) {
        struct leme_render_view_content_base *base =
            wl_array_add(bases, sizeof(*base));

        if (base == NULL) {
            return false;
        }
        *base = (struct leme_render_view_content_base){
            .node = node, .x = node->x, .y = node->y, .opacity = 1.0f,
        };
        if (node->type == WLR_SCENE_NODE_RECT) {
            struct wlr_scene_rect *rect = wl_container_of(node, rect, node);

            base->width = rect->width;
            base->height = rect->height;
        } else if (node->type == WLR_SCENE_NODE_BUFFER) {
            struct wlr_scene_buffer *buffer =
                wl_container_of(node, buffer, node);

            base->width = buffer->dst_width;
            base->height = buffer->dst_height;
            base->opacity = buffer->opacity;
        }
        /* A recursão fica no fim: wl_array_add pode realocar o vector. */
        if (node->type == WLR_SCENE_NODE_TREE) {
            struct wlr_scene_tree *branch = wl_container_of(node, branch, node);

            if (!leme_render_view_collect_content(bases, branch)) {
                return false;
            }
        }
    }
    return true;
}

static int
leme_render_view_scaled_length(int base, double ratio)
{
    int scaled;

    if (base <= 0) {
        return base;
    }
    scaled = (int)((double)base * ratio);
    return scaled < 1 ? 1 : scaled;
}

/*
 * Cada eixo leva o seu rácio: a moldura é uma constante subtraída às duas
 * dimensões, não uma proporção, por isso os rácios não coincidem.
 */
static void
leme_render_view_apply_content(struct leme_render_view_animation *state,
    double ratio_x, double ratio_y)
{
    struct leme_render_view_content_base *base;

    wl_array_for_each(base, &state->content_bases) {
        wlr_scene_node_set_position(base->node,
            (int)((double)base->x * ratio_x),
            (int)((double)base->y * ratio_y));
        if (base->node->type == WLR_SCENE_NODE_RECT) {
            struct wlr_scene_rect *rect =
                wl_container_of(base->node, rect, node);

            wlr_scene_rect_set_size(rect,
                leme_render_view_scaled_length(base->width, ratio_x),
                leme_render_view_scaled_length(base->height, ratio_y));
        } else if (base->node->type == WLR_SCENE_NODE_BUFFER) {
            struct wlr_scene_buffer *buffer =
                wl_container_of(base->node, buffer, node);

            wlr_scene_buffer_set_dest_size(buffer,
                leme_render_view_scaled_length(base->width, ratio_x),
                leme_render_view_scaled_length(base->height, ratio_y));
        }
    }
}

/* wlr_render_color é pré-multiplicada: o alfa entra nos quatro canais. */
static void
leme_render_view_animation_fade(struct leme_render_view_animation *state,
    float alpha)
{
    struct leme_render_view_content_base *base;
    size_t index;

    wl_array_for_each(base, &state->content_bases) {
        if (base->node->type == WLR_SCENE_NODE_BUFFER) {
            struct wlr_scene_buffer *buffer =
                wl_container_of(base->node, buffer, node);

            wlr_scene_buffer_set_opacity(buffer, base->opacity * alpha);
        }
    }
    for (index = 0; index < LEME_ARRAY_LENGTH(state->nodes.border); index++) {
        const float color[4] = {
            state->border_color[index][0] * alpha,
            state->border_color[index][1] * alpha,
            state->border_color[index][2] * alpha,
            state->border_color[index][3] * alpha,
        };

        if (state->nodes.border[index] == NULL) {
            break;
        }
        wlr_scene_rect_set_color(state->nodes.border[index], color);
    }
}

static void
leme_render_view_animation_apply(void *data,
    const struct leme_animation_frame *frame)
{
    struct leme_render_view_animation *state = data;
    struct leme_box box = frame->box;
    int border_width = leme_render_view_clamp_border(state->border_width, box);
    double ratio_x = 1.0;
    double ratio_y = 1.0;

    wlr_scene_node_set_position(&state->root->node, box.x, box.y);
    leme_render_view_layout_frame(&state->nodes, box, border_width,
        leme_render_view_corner_radius(state->view, box));
    leme_render_view_apply_surface_effects(state->view,
        leme_render_view_corner_radius(state->view, box), border_width,
        leme_render_view_blur(state->view));
    if (state->final_content_width > 0) {
        ratio_x = (double)(box.width - border_width * 2) /
            (double)state->final_content_width;
    }
    if (state->final_content_height > 0) {
        ratio_y = (double)(box.height - border_width * 2) /
            (double)state->final_content_height;
    }
    leme_render_view_apply_content(state, ratio_x, ratio_y);
    leme_render_view_animation_fade(state, (float)frame->opacity);
}

static void
leme_render_view_animation_done(void *data)
{
    struct leme_render_view_animation *state = data;

    wl_array_release(&state->content_bases);
    if (state->view != NULL) {
        if (state->restore_on_done && state->view->render_tree != NULL) {
            wlr_scene_node_set_enabled(&state->view->render_tree->node, true);
        }
        state->view->animation_state = NULL;
    }
    free(state);
}

/*
 * A cópia preserva a ordem dos filhos: as quatro molduras vêm primeiro e a
 * árvore do conteúdo a seguir.
 */
static void
leme_render_view_frame_from_snapshot(struct wlr_scene_tree *snapshot,
    struct leme_render_view_frame_nodes *nodes)
{
    struct wlr_scene_node *node;
    size_t borders = 0;

    *nodes = (struct leme_render_view_frame_nodes){0};
    wl_list_for_each(node, &snapshot->children, link) {
        if (node->type == WLR_SCENE_NODE_RECT &&
                borders < LEME_ARRAY_LENGTH(nodes->border)) {
            struct wlr_scene_rect *rect = wl_container_of(node, rect, node);

            nodes->border[borders++] = rect;
        } else if (node->type == WLR_SCENE_NODE_TREE &&
                nodes->content == NULL) {
            struct wlr_scene_tree *tree = wl_container_of(node, tree, node);

            nodes->content = tree;
        }
    }
}

void
leme_render_view_apply_active_snapshot(
    const struct leme_view *view, struct wlr_scene_tree *snapshot)
{
    struct leme_render_view_frame_nodes nodes;
    const struct leme_config *config;
    struct leme_view_rules rules;
    float opacity;
    size_t index;

    if (view == NULL || snapshot == NULL || view->server == NULL ||
            view->server->config == NULL) {
        return;
    }
    config = view->server->config;
    rules = leme_view_rules_match(config,
        leme_view_identity(view), leme_view_title(view));
    opacity = leme_render_view_opacity(
        config, true, view->fullscreen, &rules);
    wlr_scene_node_for_each_buffer(&snapshot->node,
        leme_render_view_apply_opacity, &opacity);

    leme_render_view_frame_from_snapshot(snapshot, &nodes);
    for (index = 0; index < LEME_ARRAY_LENGTH(nodes.border); index++) {
        if (nodes.border[index] != NULL) {
            wlr_scene_rect_set_color(
                nodes.border[index], config->border_active);
        }
    }
}

static struct leme_box
leme_render_view_shrunk(struct leme_box box, double scale_from)
{
    struct leme_box small = box;

    small.width = (int)((double)box.width * scale_from);
    small.height = (int)((double)box.height * scale_from);
    small.x = box.x + (box.width - small.width) / 2;
    small.y = box.y + (box.height - small.height) / 2;
    return small;
}

/*
 * Abrir e fechar são a mesma animação com os extremos trocados; o efeito
 * escolhe que campos do frame se movem e os outros ficam no valor final.
 */
static struct leme_animation_spec
leme_render_view_animation_spec(struct leme_box final,
    const struct leme_animation_settings *settings, bool opening)
{
    bool scale = (settings->effects &
        (uint32_t)LEME_ANIMATION_EFFECT_SCALE) != 0;
    bool fade = (settings->effects &
        (uint32_t)LEME_ANIMATION_EFFECT_FADE) != 0;
    struct leme_box small = scale ?
        leme_render_view_shrunk(final, settings->scale_from) : final;

    return (struct leme_animation_spec){
        .from = opening ? small : final,
        .to = opening ? final : small,
        .from_opacity = fade && opening ? 0.0 : 1.0,
        .to_opacity = fade && !opening ? 0.0 : 1.0,
        .duration_ms = settings->duration_ms,
        .curve = settings->curve,
        .opacity_curve = settings->opacity_curve,
    };
}

static void
leme_render_view_animation_begin(struct leme_view *view,
    const struct leme_animation_settings *settings, bool opening)
{
    struct leme_render_view_animation *state;
    struct leme_animation_spec spec;
    struct leme_animation_subject subject;
    struct leme_box content;
    size_t index;

    if (settings->effects == 0) {
        goto abandon;
    }
    state = calloc(1, sizeof(*state));
    if (state == NULL) {
        goto abandon;
    }
    content = leme_render_view_content_box(view, view->box);
    wl_array_init(&state->content_bases);
    state->view = view;
    state->output = view->scratchpad_state == LEME_SCRATCHPAD_SHOWN &&
        view->server->scratchpads.shown == view ?
        view->server->scratchpads.shown_output :
        (view->tag == NULL || view->tag->owner == NULL ? NULL :
            view->tag->owner->output);
    state->restore_on_done = opening;
    state->border_width = leme_render_view_border_width(view, view->box);
    state->final_content_width = content.width;
    state->final_content_height = content.height;
    state->root = leme_animation_snapshot(view->render_tree,
        leme_render_view_parent(view));
    if (state->root != NULL) {
        leme_render_view_frame_from_snapshot(state->root, &state->nodes);
        if (leme_render_view_collect_content(&state->content_bases,
                state->nodes.content)) {
            for (index = 0;
                    index < LEME_ARRAY_LENGTH(state->nodes.border); index++) {
                if (state->nodes.border[index] != NULL) {
                    memcpy(state->border_color[index],
                        state->nodes.border[index]->color,
                        sizeof(state->border_color[index]));
                }
            }
        } else {
            leme_animation_snapshot_destroy(state->root);
            state->root = NULL;
        }
    }
    view->animation_state = state;
    spec = leme_render_view_animation_spec(view->box, settings, opening);
    subject = (struct leme_animation_subject){
        .data = state,
        .owner = state->output,
        .apply = leme_render_view_animation_apply,
        .done = leme_render_view_animation_done,
    };
    /* Um instantâneo nulo não impede nada: run() abandona e corre done(). */
    leme_animation_run(&view->server->animations, state->root, &spec,
        &subject);
    return;

abandon:
    if (opening && view->render_tree != NULL) {
        wlr_scene_node_set_enabled(&view->render_tree->node, true);
    }
}

struct leme_box
leme_render_view_frame_box(const struct leme_view *view,
    struct leme_box content)
{
    int border_width;

    if (view->fullscreen || view->unmanaged || view->server->config == NULL) {
        return content;
    }
    border_width = view->server->config->border_width;
    content.x -= border_width;
    content.y -= border_width;
    content.width += border_width * 2;
    content.height += border_width * 2;
    return content;
}

struct leme_box
leme_render_view_local_box(
    const struct leme_view *view, struct leme_box global)
{
    if (view == NULL) {
        return (struct leme_box){0};
    }
    global.x -= view->box.x;
    global.y -= view->box.y;
    return global;
}

bool
leme_render_view_show_drop_preview(
    struct leme_view *view, struct leme_box global)
{
    static const float color[4] = {0.16f, 0.42f, 0.72f, 0.30f};
    struct leme_box local;

    if (view == NULL || view->render_tree == NULL ||
            global.width <= 0 || global.height <= 0) {
        return false;
    }
    local = leme_render_view_local_box(view, global);
    if (view->drop_preview == NULL) {
        view->drop_preview = wlr_scene_rect_create(
            view->render_tree, local.width, local.height, color);
        if (view->drop_preview == NULL) {
            return false;
        }
    } else {
        wlr_scene_rect_set_size(
            view->drop_preview, local.width, local.height);
    }
    wlr_scene_node_set_position(
        &view->drop_preview->node, local.x, local.y);
    wlr_scene_node_raise_to_top(&view->drop_preview->node);
    wlr_scene_node_set_enabled(&view->drop_preview->node, true);
    return true;
}

void
leme_render_view_hide_drop_preview(struct leme_view *view)
{
    if (view != NULL && view->drop_preview != NULL) {
        wlr_scene_node_set_enabled(&view->drop_preview->node, false);
    }
}

#ifdef LEME_HAVE_EFFECTS
struct leme_render_surface_effects {
    struct wlr_surface *surface;
    float radius;
    float blur;
};

static void
leme_render_view_apply_surface_effects_to_buffer(
    struct wlr_scene_buffer *buffer, int sx, int sy, void *data)
{
    const struct leme_render_surface_effects *target = data;
    struct wlr_scene_surface *scene_surface =
        wlr_scene_surface_try_from_buffer(buffer);

    (void)sx;
    (void)sy;
    /*
     * Só a superfície de topo. Arredondar cada subsuperfície recortava-as
     * uma a uma, em vez de arredondar a janela. O desfoque segue a mesma
     * regra: cada subsuperfície desfocaria o que a de topo já desenhou.
     */
    if (scene_surface != NULL && scene_surface->surface == target->surface) {
        wlr_scene_buffer_set_corner_radius(buffer, target->radius);
        wlr_scene_buffer_set_backdrop_blur(buffer, target->blur);
    }
}
#endif

/*
 * O conteúdo segue a curva interior do anel, para que as duas se encontrem
 * sem costura. O desfoque assenta na mesma superfície, porque é atrás dela
 * que o fundo tem de ser lido.
 */
static void
leme_render_view_apply_surface_effects(struct leme_view *view,
    int corner_radius, int border_width, int blur)
{
#ifdef LEME_HAVE_EFFECTS
    int inner = corner_radius - border_width;

    if (view == NULL) {
        return;
    }
    /*
     * A superfície é lida aqui em vez de vir de leme_view_surface: os testes
     * de geometria ligam só este ficheiro, e chamar a camada shell
     * obrigava-os a arrastar o resto.
     */
    struct wlr_surface *surface = view->kind == LEME_VIEW_XDG ?
        (view->xdg_toplevel != NULL ?
            view->xdg_toplevel->base->surface : NULL) :
        (view->xwayland_surface != NULL ?
            view->xwayland_surface->surface : NULL);
    struct leme_render_surface_effects target = {
        .surface = surface,
        .radius = (float)(inner > 0 ? inner : 0),
        .blur = (float)(blur > 0 ? blur : 0),
    };

    if (view->scene_tree != NULL && target.surface != NULL) {
        wlr_scene_node_for_each_buffer(&view->scene_tree->node,
            leme_render_view_apply_surface_effects_to_buffer, &target);
    }
#else
    (void)view;
    (void)corner_radius;
    (void)border_width;
    (void)blur;
#endif
}

/* A moldura é desenhada a partir de uma caixa, seja ela a viva ou a animada. */
void
leme_render_view_layout_frame(
    const struct leme_render_view_frame_nodes *nodes,
    struct leme_box box, int border_width, int corner_radius)
{
    size_t index;

#ifndef LEME_HAVE_EFFECTS
    (void)corner_radius;
#endif

    if (nodes->content != NULL) {
        wlr_scene_node_set_position(&nodes->content->node,
            border_width, border_width);
    }
    for (index = 0; index < LEME_ARRAY_LENGTH(nodes->border); index++) {
        if (nodes->border[index] == NULL) {
            return;
        }
    }
    if (border_width > 0) {
#ifdef LEME_HAVE_EFFECTS
        /*
         * Um só rectângulo em anel cobre a moldura toda. O meio fica vago,
         * para que uma janela translúcida continue a mostrar o que está por
         * trás e não a cor da moldura.
         */
        wlr_scene_rect_set_size(nodes->border[0], box.width, box.height);
        wlr_scene_node_set_position(&nodes->border[0]->node, 0, 0);
        wlr_scene_rect_set_corner_radius(nodes->border[0],
            (float)corner_radius, (float)border_width);
#else
        int content_height = box.height - border_width * 2;

        wlr_scene_rect_set_size(nodes->border[0], box.width, border_width);
        wlr_scene_rect_set_size(nodes->border[1], box.width, border_width);
        wlr_scene_rect_set_size(nodes->border[2],
            border_width, content_height);
        wlr_scene_rect_set_size(nodes->border[3],
            border_width, content_height);
        wlr_scene_node_set_position(&nodes->border[0]->node, 0, 0);
        wlr_scene_node_set_position(&nodes->border[1]->node,
            0, box.height - border_width);
        wlr_scene_node_set_position(&nodes->border[2]->node,
            0, border_width);
        wlr_scene_node_set_position(&nodes->border[3]->node,
            box.width - border_width, border_width);
#endif
    }
    for (index = 0; index < LEME_ARRAY_LENGTH(nodes->border); index++) {
#ifdef LEME_HAVE_EFFECTS
        bool shown = index == 0 && border_width > 0;
#else
        bool shown = border_width > 0;
#endif

        wlr_scene_node_set_enabled(&nodes->border[index]->node, shown);
    }
}

void
leme_render_view_set_box(struct leme_view *view, struct leme_box box)
{
    struct leme_render_view_frame_nodes nodes;
    struct leme_box content;
    int border_width;
    size_t index;

    if (view->render_tree == NULL || view->scene_tree == NULL) {
        return;
    }
    content = leme_render_view_content_box(view, box);
    border_width = content.x - box.x;
    wlr_scene_node_set_position(&view->render_tree->node, box.x, box.y);
    for (index = 0; index < LEME_ARRAY_LENGTH(view->border); index++) {
        nodes.border[index] = view->border[index];
    }
    nodes.content = view->scene_tree;
    leme_render_view_layout_frame(&nodes, box, border_width,
        leme_render_view_corner_radius(view, box));
    leme_render_view_apply_surface_effects(view,
        leme_render_view_corner_radius(view, box), border_width,
        leme_render_view_blur(view));
    if (view->kind == LEME_VIEW_XWAYLAND) {
        leme_render_xwayland_set_geometry(view);
    }
    if ((!view->floating && !view->detached) || view->fullscreen ||
            view->kind == LEME_VIEW_XWAYLAND) {
        if (view->configure_deferred) {
            view->deferred_configure_box = content;
            view->configure_dirty = true;
            return;
        }
        leme_view_configure(view, content);
    }
}

void
leme_render_refresh_views(struct leme_server *server)
{
    struct leme_view *view;

    wl_list_for_each(view, &server->views, link) {
        if (view->mapped) {
            leme_render_view_set_box(view, view->box);
            leme_render_view_set_activated(view,
                view == server->focused_view);
        }
    }
}
