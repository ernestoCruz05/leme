#include "shell/scratchpad.h"

#include "config/config.h"
#include "core/process.h"
#include "core/server.h"
#include "input/input.h"
#include "output/output.h"
#include "protocols/capture.h"
#include "protocols/publication.h"
#include "render/render.h"
#include "render/workspace_transition.h"
#include "shell/policy.h"
#include "shell/view.h"
#include "workspace/tag.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/util/log.h>
#include <wlr/xwayland/xwayland.h>

#define LEME_SCRATCHPAD_PENDING_TIMEOUT_MS 10000

struct leme_scratchpad_pending {
    struct wl_list link;
    struct leme_scratchpad_manager *manager;
    char *name;
    char *identity;
    struct leme_output *destination;
    struct wl_event_source *timer;
};

static void
leme_scratchpad_pending_destroy(struct leme_scratchpad_pending *pending)
{
    if (pending == NULL) {
        return;
    }
    if (pending->timer != NULL) {
        wl_event_source_remove(pending->timer);
    }
    if (pending->link.next != NULL) {
        wl_list_remove(&pending->link);
    }
    free(pending->name);
    free(pending->identity);
    free(pending);
}

static int
leme_scratchpad_pending_timeout(void *data)
{
    struct leme_scratchpad_pending *pending = data;

    wlr_log(WLR_ERROR, "leme: scratchpad %s spawn timed out", pending->name);
    leme_scratchpad_pending_destroy(pending);
    return 0;
}

static struct leme_scratchpad_pending *
leme_scratchpad_pending_by_name(const struct leme_scratchpad_manager *manager,
    const char *name)
{
    struct leme_scratchpad_pending *pending;

    wl_list_for_each(pending, &manager->pending, link) {
        if (strcmp(pending->name, name) == 0) {
            return pending;
        }
    }
    return NULL;
}

static struct leme_scratchpad_pending *
leme_scratchpad_pending_by_identity(const struct leme_scratchpad_manager *manager,
    const char *identity)
{
    struct leme_scratchpad_pending *pending;

    wl_list_for_each(pending, &manager->pending, link) {
        if (strcmp(pending->identity, identity) == 0) {
            return pending;
        }
    }
    return NULL;
}

static bool
leme_scratchpad_pending_spawn(struct leme_scratchpad_manager *manager,
    char *const *argv)
{
    if (manager->spawn_observer != NULL) {
        return manager->spawn_observer(manager->server, argv,
            manager->spawn_observer_data);
    }
    return leme_process_spawn_detached(manager->server, argv);
}

static int
leme_scratchpad_pending_arm(struct leme_scratchpad_manager *manager,
    struct wl_event_source *timer)
{
    if (manager->timer_arm_observer != NULL) {
        return manager->timer_arm_observer(LEME_SCRATCHPAD_PENDING_TIMEOUT_MS,
            manager->timer_arm_observer_data);
    }
    return wl_event_source_timer_update(timer,
        LEME_SCRATCHPAD_PENDING_TIMEOUT_MS);
}

static bool
leme_scratchpad_pending_create(struct leme_scratchpad_manager *manager,
    const struct leme_scratchpad_config *scratchpad,
    struct leme_output *destination)
{
    struct leme_scratchpad_pending *pending;
    struct wl_event_loop *loop;

    if (manager->server == NULL || manager->server->display == NULL) {
        return false;
    }
    pending = calloc(1, sizeof(*pending));
    if (pending == NULL) {
        return false;
    }
    pending->manager = manager;
    pending->name = strdup(scratchpad->name);
    pending->identity = strdup(scratchpad->identity);
    if (pending->name == NULL || pending->identity == NULL) {
        leme_scratchpad_pending_destroy(pending);
        return false;
    }
    loop = wl_display_get_event_loop(manager->server->display);
    pending->timer = wl_event_loop_add_timer(loop,
        leme_scratchpad_pending_timeout, pending);
    if (pending->timer == NULL) {
        leme_scratchpad_pending_destroy(pending);
        return false;
    }
    pending->destination = destination;
    if (leme_scratchpad_pending_arm(manager, pending->timer) < 0 ||
            !leme_scratchpad_pending_spawn(manager, scratchpad->spawn)) {
        leme_scratchpad_pending_destroy(pending);
        return false;
    }
    wl_list_insert(&manager->pending, &pending->link);
    return true;
}

static bool
leme_scratchpad_pool_linked(const struct leme_view *view)
{
    return view->scratchpad_link.next != NULL &&
        view->scratchpad_link.next != &view->scratchpad_link;
}

static void
leme_scratchpad_promote(struct leme_scratchpad_manager *manager,
    struct leme_view *view)
{
    if (leme_scratchpad_pool_linked(view)) {
        wl_list_remove(&view->scratchpad_link);
    }
    wl_list_insert(&manager->pool, &view->scratchpad_link);
}

static void
leme_scratchpad_unlink(struct leme_view *view)
{
    if (leme_scratchpad_pool_linked(view)) {
        wl_list_remove(&view->scratchpad_link);
    }
    if (view->scratchpad_link.next != NULL) {
        wl_list_init(&view->scratchpad_link);
    }
}

static void
leme_scratchpad_transition_reset(struct leme_server *server)
{
    server->scratchpads.commit_started = false;
}

static void
leme_scratchpad_transition_commit_started(struct leme_server *server)
{
    server->scratchpads.commit_started = true;
    if (server->scratchpads.commit_observer != NULL) {
        server->scratchpads.commit_observer(server,
            server->scratchpads.commit_observer_data);
    }
}

static bool
leme_scratchpad_output_valid(const struct leme_server *server,
    const struct leme_output *output)
{
    const struct leme_output *candidate;

    if (server == NULL || output == NULL || output->server != server ||
            output->wlr_output == NULL || !output->wlr_output->enabled ||
            !output->power_on) {
        return false;
    }
    wl_list_for_each(candidate, &server->outputs, link) {
        if (candidate == output) {
            return true;
        }
    }
    return false;
}

static struct leme_view *
leme_scratchpad_newest_unnamed(const struct leme_scratchpad_manager *manager)
{
    struct leme_view *view;

    wl_list_for_each(view, &manager->pool, scratchpad_link) {
        if (view->scratchpad_name == NULL) {
            return view;
        }
    }
    return NULL;
}

static bool
leme_scratchpad_claim_matches(const struct leme_view *view, const char *name)
{
    return view->scratchpad_name != NULL && strcmp(view->scratchpad_name, name) == 0;
}

static struct leme_view *
leme_scratchpad_hidden_claim(const struct leme_scratchpad_manager *manager,
    const char *name)
{
    struct leme_view *view;

    wl_list_for_each(view, &manager->pool, scratchpad_link) {
        if (view->scratchpad_state == LEME_SCRATCHPAD_HIDDEN &&
                leme_scratchpad_claim_matches(view, name)) {
            return view;
        }
    }
    return NULL;
}

static struct leme_view *
leme_scratchpad_hidden_identity(const struct leme_scratchpad_manager *manager,
    const char *identity)
{
    struct leme_view *view;

    wl_list_for_each(view, &manager->pool, scratchpad_link) {
        const char *candidate = leme_view_identity(view);

        if (view->scratchpad_state == LEME_SCRATCHPAD_HIDDEN &&
                view->scratchpad_name == NULL && candidate != NULL &&
                strcmp(candidate, identity) == 0) {
            return view;
        }
    }
    return NULL;
}

static struct leme_view *
leme_scratchpad_focused_tagged_identity(const struct leme_server *server,
    const char *identity)
{
    struct leme_view *view;

    wl_list_for_each(view, &server->focus_history, focus_link) {
        const char *candidate = leme_view_identity(view);

        if (view->mapped && !view->unmanaged && view->tag != NULL &&
                view->scratchpad_state == LEME_SCRATCHPAD_NONE &&
                view->scratchpad_name == NULL && candidate != NULL &&
                strcmp(candidate, identity) == 0) {
            return view;
        }
    }
    return NULL;
}

static int
leme_scratchpad_saturate_int(int64_t value)
{
    if (value > INT32_MAX) {
        return INT32_MAX;
    }
    if (value < INT32_MIN) {
        return INT32_MIN;
    }
    return (int)value;
}

static struct leme_box
leme_scratchpad_reanchor_box(const struct leme_view *view,
    struct leme_output *output)
{
    const struct leme_box area = leme_output_usable_box(output);
    struct leme_box box = view->box;
    double x_fraction = 0.0;
    double y_fraction = 0.0;
    const int64_t old_x = (int64_t)view->scratchpad_anchor_area.x;
    const int64_t old_y = (int64_t)view->scratchpad_anchor_area.y;

    if (view->scratchpad_anchor_area.width > 0) {
        x_fraction = (double)((int64_t)box.x - old_x) /
            (double)view->scratchpad_anchor_area.width;
    }
    if (view->scratchpad_anchor_area.height > 0) {
        y_fraction = (double)((int64_t)box.y - old_y) /
            (double)view->scratchpad_anchor_area.height;
    }
    box.x = leme_scratchpad_saturate_int((int64_t)area.x +
        (int64_t)llround(x_fraction * (double)area.width));
    box.y = leme_scratchpad_saturate_int((int64_t)area.y +
        (int64_t)llround(y_fraction * (double)area.height));
    return leme_view_policy_clamp_box(box, area);
}

static void
leme_scratchpad_set_box(struct leme_view *view, struct leme_box box,
    bool configure)
{
    const bool size_changed = view->box.width != box.width ||
        view->box.height != box.height;

    view->box = box;
    leme_render_view_set_box(view, box);
    if ((size_changed || configure) && view->kind == LEME_VIEW_XDG) {
        leme_view_configure(view, leme_render_view_content_box(view, box));
        if (view->server->scratchpads.geometry_configure_count != NULL) {
            (*view->server->scratchpads.geometry_configure_count)++;
        }
    }
}

static void
leme_scratchpad_finish_outputs(struct leme_output *first,
    struct leme_output *second)
{
    if (first != NULL) {
        leme_render_output_animations_finish(first);
    }
    if (second != NULL && second != first) {
        leme_render_output_animations_finish(second);
    }
}

static void
leme_scratchpad_restore_tag_focus(struct leme_server *server)
{
    struct leme_tags *tags = leme_focused_tags(server);
    struct leme_tag *current;
    struct leme_view *fullscreen = NULL;
    struct leme_view *view;

    if (server->seat == NULL) {
        return;
    }
    if (tags == NULL) {
        leme_view_clear_focus(server);
        return;
    }
    current = tags->focused_is_candidate ? NULL :
        tags->table[tags->focused_id];
    if (current != NULL) {
        wl_list_for_each(view, &current->views, tag_link) {
            if (view->mapped && view->fullscreen) {
                fullscreen = view;
                break;
            }
        }
        for (struct wl_list *link = server->focus_history.next;
                link != &server->focus_history; link = link->next) {
            view = wl_container_of(link, view, focus_link);
            if (view->mapped && !view->unmanaged && view->tag == current &&
                    (fullscreen == NULL || view == fullscreen)) {
                current->focused_view = view;
                break;
            }
        }
    }
    leme_tags_refresh_visibility(tags);
}

static void
leme_scratchpad_hide(struct leme_scratchpad_manager *manager,
    struct leme_view *view, bool promote)
{
    const bool focused = manager->server->focused_view == view;
    struct leme_output *output;

    if (view->scratchpad_state != LEME_SCRATCHPAD_SHOWN ||
            manager->shown != view) {
        return;
    }
    output = manager->shown_output;
    leme_scratchpad_finish_outputs(output, NULL);
    leme_render_view_finish_animation(view);
    leme_capture_invalidate_view(view);
    manager->shown = NULL;
    manager->shown_output = NULL;
    view->scratchpad_state = LEME_SCRATCHPAD_HIDDEN;
    if (promote) {
        leme_scratchpad_promote(manager, view);
    }
    leme_render_set_view_visible(view, false);
    leme_view_focus_history_remove(view);
    if (focused) {
        leme_scratchpad_restore_tag_focus(manager->server);
    }
}

static struct leme_box
leme_scratchpad_named_box(const struct leme_scratchpad_config *scratchpad,
    struct leme_output *output)
{
    const struct leme_box area = leme_output_usable_box(output);
    const int width = leme_scratchpad_saturate_int((int64_t)lround(
        scratchpad->width * (double)area.width));
    const int height = leme_scratchpad_saturate_int((int64_t)lround(
        scratchpad->height * (double)area.height));

    return (struct leme_box){
        .x = leme_scratchpad_saturate_int((int64_t)area.x +
            ((int64_t)area.width - width) / 2),
        .y = leme_scratchpad_saturate_int((int64_t)area.y +
            ((int64_t)area.height - height) / 2),
        .width = width,
        .height = height,
    };
}

static void
leme_scratchpad_show(struct leme_scratchpad_manager *manager,
    struct leme_view *view, struct leme_output *output,
    const struct leme_scratchpad_config *scratchpad)
{
    struct leme_output *previous = manager->shown == NULL ? NULL :
        manager->shown_output;

    if (manager->shown != NULL && manager->shown != view) {
        leme_scratchpad_hide(manager, manager->shown, false);
    } else if (previous != NULL) {
        leme_scratchpad_finish_outputs(previous, NULL);
    }
    if (previous == NULL || previous != output) {
        leme_scratchpad_finish_outputs(output, NULL);
    }
    leme_render_view_finish_animation(view);
    const struct leme_box box = scratchpad == NULL ?
        leme_scratchpad_reanchor_box(view, output) :
        leme_scratchpad_named_box(scratchpad, output);

    view->floating = true;
    view->fullscreen = false;
    view->scratchpad_state = LEME_SCRATCHPAD_SHOWN;
    manager->shown = view;
    manager->shown_output = output;
    leme_scratchpad_promote(manager, view);
    leme_render_view_update_layer(view);
    leme_scratchpad_set_box(view, box, scratchpad != NULL);
    view->scratchpad_anchor_area = leme_output_usable_box(output);
    leme_render_set_view_visible(view, true);
    if (manager->server->seat != NULL) {
        leme_view_focus(view);
    }
}

static bool
leme_scratchpad_direct_prepare(struct leme_scratchpad_manager *manager,
    struct leme_view *view, struct leme_output *output,
    const struct leme_scratchpad_config *scratchpad)
{
    const struct leme_box box = output == NULL ? (struct leme_box){0} :
        leme_scratchpad_named_box(scratchpad, output);

    if (output != NULL) {
        leme_scratchpad_finish_outputs(manager->shown_output, output);
        leme_render_view_finish_animation(view);
        leme_scratchpad_set_box(view, box, true);
        view->scratchpad_anchor_area = leme_output_usable_box(output);
    }
    if (manager->direct_prepare_observer != NULL &&
            !manager->direct_prepare_observer(view,
                manager->direct_prepare_observer_data)) {
        return false;
    }
    return true;
}

static void
leme_scratchpad_direct_abort_map(struct leme_view *view)
{
    leme_render_view_destroy(view);
    view->mapped = false;
    view->floating = false;
    view->unmanaged = false;
}

static void
leme_scratchpad_direct_commit(struct leme_scratchpad_manager *manager,
    struct leme_view *view, char *claim, struct leme_output *output)
{
    if (output == NULL) {
        view->scratchpad_name = claim;
        view->scratchpad_state = LEME_SCRATCHPAD_HIDDEN;
        leme_scratchpad_promote(manager, view);
        leme_render_set_view_visible(view, false);
        return;
    }
    if (manager->shown != NULL) {
        leme_scratchpad_hide(manager, manager->shown, false);
    }
    view->scratchpad_name = claim;
    view->scratchpad_state = LEME_SCRATCHPAD_SHOWN;
    manager->shown = view;
    manager->shown_output = output;
    leme_scratchpad_promote(manager, view);
    leme_render_view_update_layer(view);
    leme_render_set_view_visible(view, true);
    if (manager->server->seat != NULL) {
        leme_view_focus(view);
    }
}

bool
leme_scratchpad_init(struct leme_server *server)
{
    if (server == NULL) {
        return false;
    }
    server->scratchpads = (struct leme_scratchpad_manager){
        .server = server,
    };
    wl_list_init(&server->scratchpads.pool);
    wl_list_init(&server->scratchpads.pending);
    return true;
}

void
leme_scratchpad_finish(struct leme_server *server)
{
    struct leme_scratchpad_manager *manager;
    struct leme_view *view;
    struct leme_view *next;
    struct leme_scratchpad_pending *pending;
    struct leme_scratchpad_pending *pending_next;

    if (server == NULL) {
        return;
    }
    manager = &server->scratchpads;
    if (manager->pending.next != NULL) {
        wl_list_for_each_safe(pending, pending_next, &manager->pending, link) {
            leme_scratchpad_pending_destroy(pending);
        }
    }
    if (manager->pool.next != NULL) {
        wl_list_for_each_safe(view, next, &manager->pool, scratchpad_link) {
            leme_scratchpad_unlink(view);
            free(view->scratchpad_name);
            view->scratchpad_name = NULL;
            view->scratchpad_state = LEME_SCRATCHPAD_NONE;
        }
    }
    manager->shown = NULL;
    manager->shown_output = NULL;
    wl_list_init(&manager->pool);
    wl_list_init(&manager->pending);
    manager->server = NULL;
}

bool
leme_view_is_scratchpad(const struct leme_view *view)
{
    return view != NULL && view->scratchpad_state != LEME_SCRATCHPAD_NONE;
}

bool
leme_view_is_shown_scratchpad(const struct leme_view *view)
{
    return view != NULL && view->scratchpad_state == LEME_SCRATCHPAD_SHOWN;
}

bool
leme_view_protocol_eligible(const struct leme_view *view)
{
    const struct leme_scratchpad_manager *manager;

    if (view == NULL || !view->mapped || view->unmanaged) {
        return false;
    }
    if (view->tag != NULL && !leme_view_is_scratchpad(view)) {
        return true;
    }
    if (!leme_view_is_shown_scratchpad(view) || view->server == NULL) {
        return false;
    }
    manager = &view->server->scratchpads;
    return manager->server == view->server && manager->shown == view &&
        manager->shown_output != NULL;
}

bool
leme_scratchpad_send(struct leme_server *server, struct leme_view *view)
{
    struct leme_tag_detach *detach = NULL;
    struct leme_output *source;
    struct leme_scratchpad_manager *manager;
    struct leme_box anchor;

    if (server == NULL || view == NULL || view->server != server ||
            !view->mapped || view->unmanaged) {
        return false;
    }
    manager = &server->scratchpads;
    if (view->scratchpad_state == LEME_SCRATCHPAD_SHOWN) {
        if (manager->shown != view) {
            return false;
        }
        leme_scratchpad_hide(manager, view, true);
        leme_publication_invalidate(server);
        return true;
    }
    if (view->scratchpad_state != LEME_SCRATCHPAD_NONE ||
            view->scratchpad_name != NULL || view->tag == NULL) {
        return false;
    }
    source = leme_view_output(view);
    if (source == NULL) {
        return false;
    }
    anchor = leme_output_usable_box(source);
    leme_scratchpad_transition_reset(server);
    if (!leme_tags_prepare_detach(view, &detach)) {
        return false;
    }

    leme_render_output_animations_finish(source);
    leme_render_view_finish_animation(view);
    leme_input_pointer_grab_cancel_view(view);
    view->scratchpad_anchor_area = anchor;
    if (view->fullscreen) {
        view->fullscreen = false;
        view->box = view->saved_box;
        leme_view_ack_fullscreen(view, false);
    }
    /* commit_started: a cauda de posse e desenho não pode falhar. */
    leme_scratchpad_transition_commit_started(server);
    leme_capture_invalidate_view(view);
    leme_tags_commit_detach(view, &detach);
    view->floating = true;
    view->scratchpad_state = LEME_SCRATCHPAD_HIDDEN;
    leme_scratchpad_promote(manager, view);
    leme_render_view_update_layer(view);
    leme_render_set_view_visible(view, false);
    leme_view_focus_history_remove(view);
    leme_scratchpad_restore_tag_focus(server);
    leme_publication_invalidate(server);
    return true;
}

static bool
leme_scratchpad_claim_tagged(struct leme_scratchpad_manager *manager,
    struct leme_view *view, const char *name)
{
    struct leme_server *server = manager->server;
    struct leme_tag_detach *detach = NULL;
    struct leme_output *source;
    struct leme_box anchor;
    char *claim;

    if (view == NULL || view->server != server || !view->mapped ||
            view->unmanaged || view->scratchpad_state != LEME_SCRATCHPAD_NONE ||
            view->scratchpad_name != NULL || view->tag == NULL) {
        return false;
    }
    source = leme_view_output(view);
    if (source == NULL) {
        return false;
    }
    claim = strdup(name);
    if (claim == NULL) {
        return false;
    }
    anchor = leme_output_usable_box(source);
    leme_scratchpad_transition_reset(server);
    if (!leme_tags_prepare_detach(view, &detach)) {
        free(claim);
        return false;
    }

    leme_render_output_animations_finish(source);
    leme_render_view_finish_animation(view);
    leme_input_pointer_grab_cancel_view(view);
    view->scratchpad_anchor_area = anchor;
    if (view->fullscreen) {
        view->fullscreen = false;
        view->box = view->saved_box;
        leme_view_ack_fullscreen(view, false);
    }
    /* commit_started: a reclamação e toda a preparação do desligar estão
     * concluídas. */
    leme_scratchpad_transition_commit_started(server);
    leme_tags_commit_detach(view, &detach);
    view->scratchpad_name = claim;
    view->floating = true;
    view->scratchpad_state = LEME_SCRATCHPAD_HIDDEN;
    leme_scratchpad_promote(manager, view);
    leme_render_view_update_layer(view);
    leme_render_set_view_visible(view, false);
    leme_view_focus_history_remove(view);
    leme_scratchpad_restore_tag_focus(server);
    return true;
}

bool
leme_scratchpad_toggle_named(struct leme_server *server, const char *name,
    struct leme_output *output)
{
    const struct leme_scratchpad_config *scratchpad;
    struct leme_scratchpad_manager *manager;
    struct leme_view *view;

    if (server == NULL || name == NULL || name[0] == '\0' ||
            server->config == NULL) {
        return false;
    }
    scratchpad = leme_config_scratchpad(server->config, name);
    if (scratchpad == NULL) {
        wlr_log(WLR_ERROR, "leme: scratchpad %s is not configured", name);
        return false;
    }
    if (!leme_scratchpad_output_valid(server, output)) {
        return false;
    }
    manager = &server->scratchpads;
    {
        struct leme_scratchpad_pending *pending =
            leme_scratchpad_pending_by_name(manager, name);

        if (pending != NULL) {
            pending->destination = output;
            return true;
        }
    }
    if (manager->shown != NULL &&
            leme_scratchpad_claim_matches(manager->shown, name)) {
        if (manager->shown_output == output) {
            leme_scratchpad_hide(manager, manager->shown, false);
        } else {
            leme_scratchpad_show(manager, manager->shown, output, scratchpad);
        }
        leme_publication_invalidate(server);
        return true;
    }
    view = leme_scratchpad_hidden_claim(manager, name);
    if (view == NULL) {
        view = leme_scratchpad_hidden_identity(manager, scratchpad->identity);
        if (view != NULL) {
            char *claim = strdup(name);

            if (claim == NULL) {
                return false;
            }
            view->scratchpad_name = claim;
        }
    }
    if (view == NULL) {
        view = leme_scratchpad_focused_tagged_identity(server,
            scratchpad->identity);
        if (view != NULL && !leme_scratchpad_claim_tagged(manager, view, name)) {
            return false;
        }
    }
    if (view == NULL) {
        return leme_scratchpad_pending_create(manager, scratchpad, output);
    }
    leme_scratchpad_show(manager, view, output, scratchpad);
    leme_publication_invalidate(server);
    return true;
}

bool
leme_scratchpad_toggle_unnamed(struct leme_server *server,
    struct leme_output *output)
{
    struct leme_scratchpad_manager *manager;
    struct leme_view *view;

    if (!leme_scratchpad_output_valid(server, output)) {
        return false;
    }
    manager = &server->scratchpads;
    if (manager->shown != NULL && manager->shown->scratchpad_name == NULL) {
        if (manager->shown_output == output) {
            leme_scratchpad_hide(manager, manager->shown, false);
        } else {
            leme_scratchpad_show(manager, manager->shown, output, NULL);
        }
        leme_publication_invalidate(server);
        return true;
    }
    view = leme_scratchpad_newest_unnamed(manager);
    if (view == NULL) {
        return false;
    }
    leme_scratchpad_show(manager, view, output, NULL);
    leme_publication_invalidate(server);
    return true;
}

bool
leme_scratchpad_retrieve(struct leme_server *server,
    struct leme_tags *destination)
{
    struct leme_scratchpad_manager *manager;
    struct leme_view *view;
    const uint16_t id = destination == NULL ? 0 : destination->focused_id;

    if (server == NULL || destination == NULL || destination->server != server ||
            destination->output == NULL ||
            &destination->output->tags != destination ||
            !leme_scratchpad_output_valid(server, destination->output) ||
            destination->focused_is_candidate || id == 0 ||
            id > destination->max_tags) {
        return false;
    }
    manager = &server->scratchpads;
    view = manager->shown == NULL ? leme_scratchpad_newest_unnamed(manager) :
        manager->shown;
    leme_scratchpad_transition_reset(server);
    if (view == NULL || !leme_tags_prepare_floating_attach(destination, id)) {
        return false;
    }

    leme_scratchpad_finish_outputs(manager->shown == view ?
        manager->shown_output : NULL, destination->output);
    leme_render_view_finish_animation(view);
    leme_input_pointer_grab_cancel_view(view);
    /* commit_started: ligar ao flutuante não aloca nem reporta falhas. */
    leme_scratchpad_transition_commit_started(server);
    leme_scratchpad_unlink(view);
    if (manager->shown == view) {
        manager->shown = NULL;
        manager->shown_output = NULL;
    }
    view->scratchpad_state = LEME_SCRATCHPAD_NONE;
    free(view->scratchpad_name);
    view->scratchpad_name = NULL;
    view->floating = true;
    leme_tags_attach_floating(destination, view, id);
    leme_render_view_update_layer(view);
    leme_render_set_view_visible(view, true);
    leme_scratchpad_set_box(view, view->box, false);
    if (server->seat != NULL) {
        leme_view_focus(view);
    }
    leme_publication_invalidate(server);
    return true;
}

enum leme_scratchpad_map_result
leme_scratchpad_try_adopt_map(struct leme_view *view)
{
    struct leme_scratchpad_manager *manager;
    struct leme_scratchpad_pending *pending;
    const struct leme_scratchpad_config *scratchpad;
    const char *identity;
    struct leme_output *output;
    char *claim;
    const struct leme_view_map_options options = {
        .floating = true,
        .scratchpad_direct = true,
    };

    if (view == NULL || view->server == NULL || view->mapped ||
            view->unmanaged || (view->kind == LEME_VIEW_XWAYLAND &&
                view->xwayland_surface != NULL &&
                view->xwayland_surface->override_redirect)) {
        return LEME_SCRATCHPAD_MAP_NO_MATCH;
    }
    identity = leme_view_identity(view);
    if (identity == NULL) {
        return LEME_SCRATCHPAD_MAP_NO_MATCH;
    }
    manager = &view->server->scratchpads;
    if (manager->pending.next == NULL) {
        return LEME_SCRATCHPAD_MAP_NO_MATCH;
    }
    pending = leme_scratchpad_pending_by_identity(manager, identity);
    if (pending == NULL) {
        return LEME_SCRATCHPAD_MAP_NO_MATCH;
    }
    scratchpad = view->server->config == NULL ? NULL :
        leme_config_scratchpad(view->server->config, pending->name);
    if (scratchpad == NULL || strcmp(scratchpad->identity, pending->identity) != 0) {
        return LEME_SCRATCHPAD_MAP_FAILED;
    }
    claim = strdup(pending->name);
    if (claim == NULL || !leme_view_map(view, &options)) {
        free(claim);
        return LEME_SCRATCHPAD_MAP_FAILED;
    }
    output = leme_scratchpad_output_valid(view->server, pending->destination) ?
        pending->destination : leme_output_focused(view->server);
    if (!leme_scratchpad_output_valid(view->server, output)) {
        output = NULL;
    }
    if (!leme_scratchpad_direct_prepare(manager, view, output, scratchpad)) {
        leme_scratchpad_direct_abort_map(view);
        free(claim);
        return LEME_SCRATCHPAD_MAP_FAILED;
    }
    leme_scratchpad_direct_commit(manager, view, claim, output);
    leme_scratchpad_pending_destroy(pending);
    if (output != NULL) {
        leme_render_view_animate(view, LEME_ANIMATION_OPEN);
    }
    leme_publication_invalidate(view->server);
    return LEME_SCRATCHPAD_MAP_ADOPTED;
}

void
leme_scratchpad_handle_identity_change(struct leme_view *view)
{
    const struct leme_scratchpad_config *scratchpad;
    const char *identity;
    struct leme_scratchpad_pending *pending;
    struct leme_scratchpad_manager *manager;
    struct leme_output *output;

    if (view == NULL || view->server == NULL) {
        return;
    }
    identity = leme_view_identity(view);
    if (view->scratchpad_name != NULL) {
        scratchpad = view->server->config == NULL ? NULL :
            leme_config_scratchpad(view->server->config, view->scratchpad_name);
        if (scratchpad == NULL || identity == NULL ||
                strcmp(identity, scratchpad->identity) != 0) {
            free(view->scratchpad_name);
            view->scratchpad_name = NULL;
        }
    }
    if (!view->mapped || view->scratchpad_state != LEME_SCRATCHPAD_NONE ||
            view->tag == NULL || identity == NULL) {
        return;
    }
    manager = &view->server->scratchpads;
    if (manager->pending.next == NULL) {
        return;
    }
    pending = leme_scratchpad_pending_by_identity(manager, identity);
    if (pending == NULL || view->server->config == NULL) {
        return;
    }
    scratchpad = leme_config_scratchpad(view->server->config, pending->name);
    if (scratchpad == NULL || strcmp(scratchpad->identity, pending->identity) != 0 ||
            !leme_scratchpad_claim_tagged(manager, view, pending->name)) {
        return;
    }
    output = leme_scratchpad_output_valid(view->server, pending->destination) ?
        pending->destination : leme_output_focused(view->server);
    if (leme_scratchpad_output_valid(view->server, output)) {
        leme_scratchpad_show(manager, view, output, scratchpad);
    }
    leme_scratchpad_pending_destroy(pending);
    leme_publication_invalidate(view->server);
}

void
leme_scratchpad_handle_output_destroy(struct leme_server *server,
    struct leme_output *output)
{
    struct leme_scratchpad_manager *manager;
    struct leme_scratchpad_pending *pending;
    bool hidden = false;

    if (server == NULL || output == NULL) {
        return;
    }
    manager = &server->scratchpads;
    if (manager->server != server) {
        return;
    }
    if (manager->shown_output == output) {
        if (manager->shown != NULL) {
            leme_scratchpad_hide(manager, manager->shown, false);
            hidden = true;
        } else {
            manager->shown_output = NULL;
        }
    }
    if (manager->pending.next != NULL) {
        wl_list_for_each(pending, &manager->pending, link) {
            if (pending->destination == output) {
                pending->destination = NULL;
            }
        }
    }
    if (hidden) {
        leme_publication_invalidate(server);
    }
}

void
leme_scratchpad_reconcile_outputs(struct leme_server *server)
{
    struct leme_scratchpad_manager *manager;

    if (server == NULL) {
        return;
    }
    manager = &server->scratchpads;
    if (manager->server != server || manager->shown == NULL ||
            manager->shown_output == NULL) {
        return;
    }
    if (!leme_scratchpad_output_valid(server, manager->shown_output)) {
        leme_scratchpad_hide(manager, manager->shown, false);
        leme_publication_invalidate(server);
    }
}

void
leme_scratchpad_handle_usable_area(struct leme_output *output)
{
    struct leme_scratchpad_manager *manager;
    struct leme_view *view;
    struct leme_box box;

    if (output == NULL || output->server == NULL) {
        return;
    }
    manager = &output->server->scratchpads;
    view = manager->shown;
    if (manager->server != output->server || view == NULL ||
            manager->shown_output != output ||
            view->scratchpad_state != LEME_SCRATCHPAD_SHOWN) {
        return;
    }
    box = leme_view_policy_clamp_box(view->box, leme_output_usable_box(output));
    leme_scratchpad_set_box(view, box, false);
    view->scratchpad_anchor_area = leme_output_usable_box(output);
}

bool
leme_scratchpad_apply_shown_box(struct leme_view *view,
    struct leme_box box, bool resizing)
{
    struct leme_scratchpad_manager *manager;
    struct leme_output *output;

    if (view == NULL || view->server == NULL) {
        return false;
    }
    manager = &view->server->scratchpads;
    output = manager->shown_output;
    if (manager->server != view->server || manager->shown != view ||
            view->scratchpad_state != LEME_SCRATCHPAD_SHOWN ||
            !leme_scratchpad_output_valid(view->server, output)) {
        return false;
    }
    box = leme_view_policy_clamp_box(box, leme_output_usable_box(output));
    leme_scratchpad_set_box(view, box, resizing);
    view->scratchpad_anchor_area = leme_output_usable_box(output);
    return true;
}

bool
leme_scratchpad_move_shown(struct leme_view *view,
    struct leme_output *output)
{
    struct leme_scratchpad_manager *manager;
    struct leme_output *previous;

    if (view == NULL || view->server == NULL) {
        return false;
    }
    manager = &view->server->scratchpads;
    previous = manager->shown_output;
    if (manager->server != view->server || manager->shown != view ||
            view->scratchpad_state != LEME_SCRATCHPAD_SHOWN ||
            !leme_scratchpad_output_valid(view->server, output)) {
        return false;
    }
    leme_scratchpad_finish_outputs(previous, output);
    leme_render_view_finish_animation(view);
    manager->shown_output = output;
    if (!leme_scratchpad_apply_shown_box(view, view->box, false)) {
        manager->shown_output = previous;
        return false;
    }
    leme_output_set_focused(view->server, output, false);
    leme_view_focus(view);
    leme_publication_invalidate(view->server);
    return true;
}

static bool
leme_scratchpad_config_matches(const struct leme_config *previous,
    const struct leme_config *next, const char *name, const char *identity)
{
    const struct leme_scratchpad_config *old_definition;
    const struct leme_scratchpad_config *new_definition;

    if (previous == NULL || next == NULL || name == NULL || identity == NULL) {
        return false;
    }
    old_definition = leme_config_scratchpad(previous, name);
    new_definition = leme_config_scratchpad(next, name);
    return old_definition != NULL && new_definition != NULL &&
        old_definition->identity != NULL && new_definition->identity != NULL &&
        strcmp(old_definition->identity, identity) == 0 &&
        strcmp(new_definition->identity, identity) == 0;
}

void
leme_scratchpad_reconcile_config(struct leme_server *server,
    const struct leme_config *previous, const struct leme_config *next)
{
    struct leme_scratchpad_manager *manager;
    struct leme_view *view;
    struct leme_view *view_next;
    struct leme_scratchpad_pending *pending;
    struct leme_scratchpad_pending *pending_next;

    if (server == NULL) {
        return;
    }
    manager = &server->scratchpads;
    if (manager->server != server) {
        return;
    }
    if (manager->pool.next != NULL) {
        wl_list_for_each_safe(view, view_next, &manager->pool, scratchpad_link) {
            const struct leme_scratchpad_config *old_definition =
                view->scratchpad_name == NULL || previous == NULL ? NULL :
                leme_config_scratchpad(previous, view->scratchpad_name);

            if (view->scratchpad_name != NULL && (old_definition == NULL ||
                    !leme_scratchpad_config_matches(previous, next,
                        view->scratchpad_name, old_definition->identity))) {
                free(view->scratchpad_name);
                view->scratchpad_name = NULL;
            }
        }
    }
    if (manager->pending.next != NULL) {
        wl_list_for_each_safe(pending, pending_next, &manager->pending, link) {
            if (!leme_scratchpad_config_matches(previous, next, pending->name,
                    pending->identity)) {
                leme_scratchpad_pending_destroy(pending);
            }
        }
    }
}

void
leme_scratchpad_handle_unmap(struct leme_view *view)
{
    struct leme_scratchpad_manager *manager;

    if (view == NULL || view->server == NULL) {
        return;
    }
    leme_capture_invalidate_view(view);
    if (view->scratchpad_state == LEME_SCRATCHPAD_NONE) {
        return;
    }
    manager = &view->server->scratchpads;
    if (manager->shown == view) {
        manager->shown = NULL;
        manager->shown_output = NULL;
    }
    leme_scratchpad_unlink(view);
    free(view->scratchpad_name);
    view->scratchpad_name = NULL;
    view->scratchpad_state = LEME_SCRATCHPAD_NONE;
}

size_t
leme_scratchpad_pending_count(const struct leme_server *server)
{
    const struct leme_scratchpad_pending *pending;
    size_t count = 0;

    if (server == NULL || server->scratchpads.pending.next == NULL) {
        return 0;
    }
    wl_list_for_each(pending, &server->scratchpads.pending, link) {
        count++;
    }
    return count;
}

bool
leme_scratchpad_pending_matches(const struct leme_server *server,
    const char *name, const char *identity)
{
    const struct leme_scratchpad_pending *pending;

    if (server == NULL || name == NULL || identity == NULL) {
        return false;
    }
    pending = leme_scratchpad_pending_by_name(&server->scratchpads, name);
    return pending != NULL && strcmp(pending->identity, identity) == 0;
}

struct leme_output *
leme_scratchpad_pending_destination(const struct leme_server *server,
    const char *name)
{
    const struct leme_scratchpad_pending *pending;

    if (server == NULL || name == NULL) {
        return NULL;
    }
    pending = leme_scratchpad_pending_by_name(&server->scratchpads, name);
    return pending == NULL ? NULL : pending->destination;
}

bool
leme_scratchpad_expire_pending(struct leme_server *server,
    const char *name)
{
    struct leme_scratchpad_pending *pending;

    if (server == NULL || name == NULL) {
        return false;
    }
    pending = leme_scratchpad_pending_by_name(&server->scratchpads, name);
    if (pending == NULL) {
        return false;
    }
    return leme_scratchpad_pending_timeout(pending) == 0;
}
