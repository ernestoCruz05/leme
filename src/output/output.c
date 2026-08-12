#include "core/gate.h"
#include "output/output.h"

#include "config/config.h"
#include "shell/layer.h"
#include "shell/layer_layout.h"
#include "shell/scratchpad.h"
#include "render/render.h"
#include "render/workspace_transition.h"
#include "core/server.h"
#include "input/input.h"
#include "protocols/desktop.h"
#include "protocols/capture.h"
#include "protocols/publication.h"
#include "protocols/session.h"
#include "protocols/workspace.h"
#include "shell/view.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/multi.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/util/log.h>

static void
leme_output_handle_frame(struct wl_listener *listener, void *data)
{
    struct leme_output *output =
        wl_container_of(listener, output, frame);

    (void)data;
    leme_render_output_frame(output);
}

static struct leme_output *
leme_output_first(struct leme_server *server)
{
    struct leme_output *output;

    if (wl_list_empty(&server->outputs)) {
        return NULL;
    }
    output = wl_container_of(server->outputs.next, output, link);
    return output;
}

static struct leme_output *
leme_output_from_wlr(struct leme_server *server,
    const struct wlr_output *wlr_output)
{
    struct leme_output *output;

    wl_list_for_each(output, &server->outputs, link) {
        if (output->wlr_output == wlr_output) {
            return output;
        }
    }
    return NULL;
}

static struct leme_output *
leme_output_find(struct leme_server *server, const char *name)
{
    struct leme_output *output;

    wl_list_for_each(output, &server->outputs, link) {
        if (name != NULL && output->wlr_output->name != NULL &&
                strcmp(output->wlr_output->name, name) == 0) {
            return output;
        }
    }
    return NULL;
}

static enum wl_output_transform
leme_output_transform(enum leme_output_transform transform)
{
    switch (transform) {
    case LEME_OUTPUT_TRANSFORM_90:
        return WL_OUTPUT_TRANSFORM_90;
    case LEME_OUTPUT_TRANSFORM_180:
        return WL_OUTPUT_TRANSFORM_180;
    case LEME_OUTPUT_TRANSFORM_270:
        return WL_OUTPUT_TRANSFORM_270;
    case LEME_OUTPUT_TRANSFORM_NORMAL:
        return WL_OUTPUT_TRANSFORM_NORMAL;
    }
    return WL_OUTPUT_TRANSFORM_NORMAL;
}

static struct wlr_output_mode *
leme_output_select_mode(struct wlr_output *output,
    const struct leme_output_config *config)
{
    struct wlr_output_mode *mode;
    struct wlr_output_mode *best = NULL;
    long long best_difference = LLONG_MAX;

    if (!config->has_mode) {
        return wlr_output_preferred_mode(output);
    }
    wl_list_for_each(mode, &output->modes, link) {
        long long difference;

        if (mode->width != config->width || mode->height != config->height) {
            continue;
        }
        if (!config->has_refresh) {
            if (best == NULL || mode->refresh > best->refresh) {
                best = mode;
            }
            continue;
        }
        difference = llabs((long long)mode->refresh - config->refresh_mhz);
        if (difference < best_difference) {
            best = mode;
            best_difference = difference;
        }
    }
    if (config->has_refresh && best_difference > 1000) {
        return NULL;
    }
    return best;
}

static const struct leme_output_config *
leme_output_config_for(const struct leme_config *config, const char *name)
{
    size_t index;

    if (name == NULL) {
        return NULL;
    }
    for (index = 0; index < config->output_count; index++) {
        if (strcmp(config->outputs[index].name, name) == 0) {
            return &config->outputs[index];
        }
    }
    return NULL;
}

static struct leme_output *
leme_output_target(struct leme_server *server,
    const struct leme_config *config, bool startup)
{
    struct leme_output *target = NULL;
    size_t index;

    for (index = 0; index < config->output_count && target == NULL; index++) {
        target = leme_output_find(server, config->outputs[index].name);
    }
    if (target == NULL && config->output_count > 0 && !startup) {
        return NULL;
    }
    if (target == NULL) {
        target = server->focused_output == NULL ?
            leme_output_first(server) : server->focused_output;
    }
    return target;
}

static bool
leme_output_logical_size(int physical_width, int physical_height,
    float configured_scale, enum wl_output_transform transform,
    int *width, int *height)
{
    double scale = (double)configured_scale;
    double logical_width;
    double logical_height;

    if (physical_width <= 0 || physical_height <= 0 ||
            !isfinite(scale) || scale <= 0.0) {
        return false;
    }
    if ((transform & WL_OUTPUT_TRANSFORM_90) != 0) {
        int swap = physical_width;

        physical_width = physical_height;
        physical_height = swap;
    }
    logical_width = round((double)physical_width / scale);
    logical_height = round((double)physical_height / scale);
    if (!isfinite(logical_width) || !isfinite(logical_height) ||
            logical_width < 1.0 || logical_width > INT_MAX ||
            logical_height < 1.0 || logical_height > INT_MAX) {
        return false;
    }
    *width = (int)logical_width;
    *height = (int)logical_height;
    return true;
}

struct leme_output_plan {
    struct leme_output *output;
    struct leme_output_config entry;
    struct wlr_output_mode *mode;
    int x;
    int y;
    bool enabled;
};

static size_t
leme_output_count(const struct leme_server *server)
{
    const struct leme_output *output;
    size_t count = 0;

    wl_list_for_each(output, &server->outputs, link) {
        count++;
    }
    return count;
}

static bool
leme_output_build_plans(struct leme_server *server,
    const struct leme_config *config, bool startup,
    struct leme_output_plan *plans, size_t count, bool *used_mode_fallback)
{
    struct leme_output *output;
    size_t index = 0;

    wl_list_for_each(output, &server->outputs, link) {
        if (index >= count) {
            break;
        }
        const struct leme_output_config *entry =
            leme_output_config_for(config, output->wlr_output->name);
        struct leme_output_plan *plan = &plans[index++];

        plan->output = output;
        plan->entry = entry != NULL ? *entry : (struct leme_output_config){
            .scale = 1.0f,
            .transform = LEME_OUTPUT_TRANSFORM_NORMAL,
        };
        plan->enabled = output->power_on;
        plan->mode = leme_output_select_mode(
            output->wlr_output, &plan->entry);
        if (plan->entry.has_mode && plan->mode == NULL) {
            if (!startup) {
                return false;
            }
            plan->mode = wlr_output_preferred_mode(output->wlr_output);
            *used_mode_fallback = true;
        }
        if (plan->mode == NULL) {
            plan->mode = wlr_output_preferred_mode(output->wlr_output);
        }
    }
    return true;
}

static bool
leme_output_resolve_plans(struct leme_output_plan *plans, size_t count)
{
    struct leme_placement_request *requests =
        calloc(count, sizeof(*requests));
    struct leme_placement *placements = calloc(count, sizeof(*placements));
    size_t index;

    if (requests == NULL || placements == NULL) {
        free(requests);
        free(placements);
        return false;
    }
    for (index = 0; index < count; index++) {
        const struct wlr_output *wlr_output;
        const struct wlr_output_mode *mode = plans[index].mode;
        int width = 0;
        int height = 0;

        if (plans[index].output == NULL) {
            free(requests);
            free(placements);
            return false;
        }
        wlr_output = plans[index].output->wlr_output;
        if (!leme_output_logical_size(
                mode != NULL ? mode->width : wlr_output->width,
                mode != NULL ? mode->height : wlr_output->height,
                plans[index].entry.scale,
                leme_output_transform(plans[index].entry.transform),
                &width, &height)) {
            free(requests);
            free(placements);
            return false;
        }
        requests[index] = (struct leme_placement_request){
            .name = plans[index].output->wlr_output->name,
            .width = width,
            .height = height,
            .x = plans[index].entry.x,
            .y = plans[index].entry.y,
            .relative_to = plans[index].entry.relative_to,
            .relation = plans[index].entry.relation,
            .has_position = plans[index].entry.has_position,
        };
    }
    leme_placement_resolve(requests, count, placements);
    for (index = 0; index < count; index++) {
        plans[index].x = placements[index].x;
        plans[index].y = placements[index].y;
    }
    free(requests);
    free(placements);
    return true;
}

static bool leme_output_heads_overlap(
    const struct wlr_output_configuration_v1 *configuration);

static struct wlr_output_configuration_v1 *
leme_output_build_persistent(struct leme_server *server,
    const struct leme_config *config, bool startup, bool *used_mode_fallback)
{
    struct wlr_output_configuration_v1 *configuration = NULL;
    struct leme_output_plan *plans;
    size_t count = leme_output_count(server);
    size_t index;

    *used_mode_fallback = false;
    if (count == 0) {
        return NULL;
    }
    plans = calloc(count, sizeof(*plans));
    if (plans == NULL) {
        return NULL;
    }
    if (!leme_output_build_plans(server, config, startup, plans, count,
            used_mode_fallback)) {
        goto cleanup;
    }
    if (!leme_output_resolve_plans(plans, count)) {
        goto cleanup;
    }
    configuration = wlr_output_configuration_v1_create();
    if (configuration == NULL) {
        goto cleanup;
    }
    for (index = 0; index < count; index++) {
        struct leme_output_plan *plan = &plans[index];
        struct wlr_output_configuration_head_v1 *head =
            wlr_output_configuration_head_v1_create(
                configuration, plan->output->wlr_output);

        if (head == NULL) {
            wlr_output_configuration_v1_destroy(configuration);
            configuration = NULL;
            goto cleanup;
        }
        head->state.enabled = plan->enabled;
        head->state.x = plan->x;
        head->state.y = plan->y;
        if (plan->enabled) {
            head->state.mode = plan->mode;
            head->state.scale = plan->entry.scale;
            head->state.transform =
                leme_output_transform(plan->entry.transform);
        }
    }
    if (leme_output_heads_overlap(configuration)) {
        wlr_output_configuration_v1_destroy(configuration);
        configuration = NULL;
    }

cleanup:
    free(plans);
    return configuration;
}

static bool
leme_output_build_power_on_plan(struct leme_output *target,
    struct leme_output_plan *result)
{
    struct leme_server *server = target->server;
    struct leme_output_plan *plans;
    size_t count = leme_output_count(server);
    size_t index;
    bool used_mode_fallback = false;
    bool found = false;

    if (count == 0) {
        return false;
    }
    plans = calloc(count, sizeof(*plans));
    if (plans == NULL) {
        return false;
    }
    if (!leme_output_build_plans(server, server->config, false,
            plans, count, &used_mode_fallback)) {
        goto cleanup;
    }
    for (index = 0; index < count; index++) {
        if (plans[index].output == target) {
            plans[index].enabled = true;
        }
    }
    if (!leme_output_resolve_plans(plans, count)) {
        goto cleanup;
    }
    for (index = 0; index < count; index++) {
        if (plans[index].output == target) {
            *result = plans[index];
            found = true;
            break;
        }
    }

cleanup:
    free(plans);
    return found;
}

static bool
leme_output_box_edges(const struct wlr_box *box,
    int64_t *right, int64_t *bottom)
{
    int64_t box_right;
    int64_t box_bottom;

    if (box->width <= 0 || box->height <= 0) {
        return false;
    }
    box_right = (int64_t)box->x + (int64_t)box->width;
    box_bottom = (int64_t)box->y + (int64_t)box->height;
    if (box_right < INT_MIN || box_right > INT_MAX ||
            box_bottom < INT_MIN || box_bottom > INT_MAX) {
        return false;
    }
    if (right != NULL) {
        *right = box_right;
    }
    if (bottom != NULL) {
        *bottom = box_bottom;
    }
    return true;
}

static bool
leme_output_plan_box(
    const struct leme_output_plan *plan, struct wlr_box *box)
{
    *box = (struct wlr_box){
        .x = plan->x,
        .y = plan->y,
    };
    return leme_output_logical_size(
            plan->mode != NULL ? plan->mode->width :
                plan->output->wlr_output->width,
            plan->mode != NULL ? plan->mode->height :
                plan->output->wlr_output->height,
            plan->entry.scale,
            leme_output_transform(plan->entry.transform),
            &box->width, &box->height) &&
        leme_output_box_edges(box, NULL, NULL);
}

static bool
leme_output_boxes_overlap(
    const struct wlr_box *first, const struct wlr_box *second)
{
    int64_t first_right;
    int64_t first_bottom;
    int64_t second_right;
    int64_t second_bottom;

    if (!leme_output_box_edges(first, &first_right, &first_bottom) ||
            !leme_output_box_edges(second, &second_right, &second_bottom)) {
        return true;
    }
    return (int64_t)first->x < second_right &&
        (int64_t)second->x < first_right &&
        (int64_t)first->y < second_bottom &&
        (int64_t)second->y < first_bottom;
}

static bool
leme_output_boxes_fit_layout(
    const struct wlr_box *first, const struct wlr_box *second)
{
    int64_t first_right;
    int64_t first_bottom;
    int64_t second_right;
    int64_t second_bottom;
    int64_t left;
    int64_t top;
    int64_t right;
    int64_t bottom;

    if (!leme_output_box_edges(first, &first_right, &first_bottom) ||
            !leme_output_box_edges(second, &second_right, &second_bottom)) {
        return false;
    }
    left = first->x < second->x ? first->x : second->x;
    top = first->y < second->y ? first->y : second->y;
    right = first_right > second_right ? first_right : second_right;
    bottom = first_bottom > second_bottom ? first_bottom : second_bottom;
    return right - left <= INT_MAX && bottom - top <= INT_MAX;
}

static bool
leme_output_power_plan_overlaps(struct leme_output *target,
    const struct wlr_box *target_box, const char **conflict)
{
    struct leme_output *other;

    wl_list_for_each(other, &target->server->outputs, link) {
        struct leme_box box;
        struct wlr_box other_box;

        if (other == target || !other->wlr_output->enabled) {
            continue;
        }
        box = leme_output_full_box(other);
        other_box = (struct wlr_box){
            .x = box.x,
            .y = box.y,
            .width = box.width,
            .height = box.height,
        };
        if (!leme_output_boxes_fit_layout(target_box, &other_box) ||
                leme_output_boxes_overlap(target_box, &other_box)) {
            *conflict = other->wlr_output->name;
            return true;
        }
    }
    return false;
}

static void
leme_output_states_finish(struct wlr_backend_output_state *states,
    size_t states_len)
{
    size_t index;

    for (index = 0; index < states_len; index++) {
        wlr_output_state_finish(&states[index].base);
    }
    free(states);
}

static bool
leme_output_states_prepare(struct leme_server *server,
    struct wlr_backend_output_state *states, size_t states_len, bool testing)
{
    size_t index;

    for (index = 0; index < states_len; index++) {
        struct leme_output *output = leme_output_from_wlr(
            server, states[index].output);

        if (output == NULL || !leme_render_prepare_output_state(
                output, &states[index].base, testing)) {
            return false;
        }
    }
    return true;
}

static bool
leme_output_head_changes_lifecycle(
    const struct wlr_output_configuration_head_v1 *head,
    const struct leme_output *output)
{
    const struct wlr_output *wlr_output = output->wlr_output;

    if (head->state.enabled != wlr_output->enabled) {
        return true;
    }
    if (!head->state.enabled) {
        return false;
    }
    if (head->state.x != output->layout_x ||
            head->state.y != output->layout_y ||
            head->state.scale != wlr_output->scale ||
            head->state.transform != wlr_output->transform ||
            head->state.mode != wlr_output->current_mode) {
        return true;
    }
    return head->state.mode == NULL &&
        (head->state.custom_mode.width != wlr_output->width ||
            head->state.custom_mode.height != wlr_output->height ||
            head->state.custom_mode.refresh != wlr_output->refresh);
}

static void
leme_output_finish_reconfigured_animations(
    struct leme_server *server,
    const struct wlr_output_configuration_v1 *configuration)
{
    struct wlr_output_configuration_head_v1 *head;

    wl_list_for_each(head, &configuration->heads, link) {
        struct leme_output *output = leme_output_from_wlr(
            server, head->state.output);

        if (output != NULL &&
                leme_output_head_changes_lifecycle(head, output)) {
            leme_render_output_animations_finish(output);
        }
    }
}

static bool
leme_output_configuration_run(struct leme_server *server,
    struct wlr_output_configuration_v1 *configuration, bool commit)
{
    struct wlr_backend_output_state *states;
    size_t states_len = 0;
    bool valid;

    states = wlr_output_configuration_v1_build_state(
        configuration, &states_len);
    if (states == NULL) {
        return false;
    }
    valid = leme_output_states_prepare(
            server, states, states_len, true) &&
        wlr_backend_test(server->backend, states, states_len);
    if (!valid || !commit) {
        leme_output_states_finish(states, states_len);
        return valid;
    }

    leme_output_finish_reconfigured_animations(server, configuration);
    leme_output_states_finish(states, states_len);
    states = wlr_output_configuration_v1_build_state(
        configuration, &states_len);
    if (states == NULL) {
        return false;
    }
    valid = leme_output_states_prepare(
            server, states, states_len, false) &&
        wlr_backend_commit(server->backend, states, states_len);
    leme_output_states_finish(states, states_len);
    return valid;
}

static bool
leme_output_configuration_matches_current(
    struct leme_server *server,
    const struct wlr_output_configuration_v1 *configuration)
{
    struct wlr_output_configuration_head_v1 *head;

    wl_list_for_each(head, &configuration->heads, link) {
        struct wlr_output *wlr_output = head->state.output;
        const struct leme_output *output =
            leme_output_from_wlr(server, wlr_output);
        const bool adaptive = wlr_output->adaptive_sync_status ==
            WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED;

        if (output == NULL ||
                head->state.enabled != wlr_output->enabled ||
                head->state.x != output->layout_x ||
                head->state.y != output->layout_y) {
            return false;
        }
        if (head->state.enabled &&
                (head->state.mode != wlr_output->current_mode ||
                    head->state.scale != wlr_output->scale ||
                    head->state.transform != wlr_output->transform ||
                    head->state.adaptive_sync_enabled != adaptive)) {
            return false;
        }
    }
    return true;
}

static bool
leme_output_configuration_test(struct leme_server *server,
    struct wlr_output_configuration_v1 *configuration)
{
    return leme_output_configuration_run(server, configuration, false);
}

static bool
leme_output_configuration_commit(struct leme_server *server,
    struct wlr_output_configuration_v1 *configuration)
{
    return leme_output_configuration_run(server, configuration, true);
}

static bool
leme_output_reconcile(struct leme_server *server)
{
    struct leme_output *first = NULL;
    struct leme_output *output;
    struct leme_output *old = server->focused_output;
    bool layer_focus_lost = server->focused_layer != NULL &&
        (server->focused_layer->output == NULL ||
            !server->focused_layer->output->wlr_output->enabled);

    if (server->output_layout == NULL) {
        return false;
    }
    if (leme_gate_output_reconcile != NULL && !leme_gate_output_reconcile()) {
        return false;
    }
    wl_list_for_each(output, &server->outputs, link) {
        if (!output->wlr_output->enabled) {
            leme_render_output_animations_finish(output);
            leme_render_detach_output(output);
            wlr_output_layout_remove(
                server->output_layout, output->wlr_output);
            continue;
        }
        if (first == NULL) {
            first = output;
        }
        if (wlr_output_layout_add(server->output_layout, output->wlr_output,
                output->layout_x, output->layout_y) == NULL ||
                !leme_render_attach_output(output)) {
            return false;
        }
        leme_render_position_output(output);
    }
    /* Fica de propósito depois de tudo o que pode falhar acima. A
     * reconciliação também corre no desfazer, por isso o estado de ciclo de
     * vida só pode ver uma topologia de saídas completa. */
    leme_scratchpad_reconcile_outputs(server);
    leme_capture_reconcile_outputs(server);
    if (server->focused_output == NULL ||
            !server->focused_output->wlr_output->enabled) {
        server->focused_output = first;
    }
    wl_list_for_each(output, &server->outputs, link) {
        if (output->wlr_output->enabled) {
            leme_output_refresh_geometry(output);
        }
    }
    leme_desktop_output_changed(server);
    leme_session_output_changed(server);
    leme_render_layers_refresh_output(server);
    leme_layer_arrange(server);
    if (old != server->focused_output || layer_focus_lost) {
        leme_layer_restore_keyboard_focus(server);
    }
    leme_view_arrange(server);
    leme_session_refresh_idle_inhibitors(server);
    return true;
}

static struct wlr_output_configuration_v1 *
leme_output_build_current_configuration(struct leme_server *server)
{
    struct wlr_output_configuration_v1 *configuration =
        wlr_output_configuration_v1_create();
    struct leme_output *output;

    if (configuration == NULL) {
        return NULL;
    }
    wl_list_for_each(output, &server->outputs, link) {
        struct wlr_output_configuration_head_v1 *head =
            wlr_output_configuration_head_v1_create(
                configuration, output->wlr_output);

        if (head == NULL) {
            wlr_output_configuration_v1_destroy(configuration);
            return NULL;
        }
        head->state.x = output->layout_x;
        head->state.y = output->layout_y;
    }
    return configuration;
}

static void
leme_output_track_configuration(struct leme_server *server,
    const struct wlr_output_configuration_v1 *configuration)
{
    struct wlr_output_configuration_head_v1 *head;

    wl_list_for_each(head, &configuration->heads, link) {
        struct leme_output *output =
            leme_output_from_wlr(server, head->state.output);

        if (output != NULL) {
            output->layout_x = head->state.x;
            output->layout_y = head->state.y;
        }
    }
}

static void
leme_output_detach_all_scenes(struct leme_server *server)
{
    struct leme_output *output;

    wl_list_for_each(output, &server->outputs, link) {
        leme_render_detach_output(output);
    }
}

static bool
leme_output_commit_and_reconcile(struct leme_server *server,
    struct wlr_output_configuration_v1 *configuration)
{
    struct wlr_output_configuration_v1 *previous =
        leme_output_build_current_configuration(server);
    bool restored;

    if (previous == NULL) {
        return false;
    }
    if (!leme_output_configuration_commit(server, configuration)) {
        wlr_output_configuration_v1_destroy(previous);
        return false;
    }
    leme_output_track_configuration(server, configuration);
    if (leme_output_reconcile(server)) {
        wlr_output_configuration_v1_destroy(previous);
        return true;
    }

    restored = leme_output_configuration_commit(server, previous);
    if (restored) {
        leme_output_track_configuration(server, previous);
        restored = leme_output_reconcile(server);
    }
    if (!restored) {
        leme_output_detach_all_scenes(server);
    }
    wlr_log(WLR_ERROR,
        "leme: output reconciliation failed after backend commit%s",
        restored ? "; previous state restored" :
            "; previous state restoration also failed");
    wlr_output_configuration_v1_destroy(previous);
    return false;
}

static void
leme_output_power_warp_focus(struct leme_server *server,
    const struct leme_output *previous)
{
    struct leme_box box;

    if (server->focused_output == previous ||
            server->focused_output == NULL || server->cursor == NULL ||
            server->config == NULL ||
            !server->config->output_policy.warp_cursor) {
        return;
    }
    box = leme_output_usable_box(server->focused_output);
    wlr_cursor_warp(server->cursor, NULL,
        box.x + box.width / 2.0, box.y + box.height / 2.0);
    leme_input_refresh_pointer_focus(server, 0);
}

bool
leme_output_set_power(struct leme_output *output, bool on)
{
    struct leme_server *server;
    struct leme_output *previous;
    struct leme_output_plan plan = {0};
    struct wlr_box target_box = {0};
    struct leme_box wake_box = {0};
    struct wlr_output_state state;
    struct wlr_scene_output *scene_output_before;
    const char *name;
    const char *conflict = NULL;
    const char *failure_stage = NULL;
    int previous_x;
    int previous_y;
    bool attachment_created = false;

    if (output == NULL || output->server == NULL ||
            output->wlr_output == NULL) {
        return false;
    }
    server = output->server;
    name = output->wlr_output->name == NULL ? "unknown" :
        output->wlr_output->name;
    if (!on && server->headless_backend != NULL &&
            output->wlr_output->backend == server->headless_backend) {
        wlr_log(WLR_ERROR,
            "leme: refusing to power off fallback output %s", name);
        return false;
    }
    if (output->wlr_output->enabled == on) {
        output->power_on = on;
        return true;
    }
    if (on && !leme_output_build_power_on_plan(output, &plan)) {
        wlr_log(WLR_ERROR,
            "leme: cannot build power-on plan for output %s", name);
        return false;
    }
    if (on && plan.mode == NULL &&
            (output->wlr_output->width <= 0 ||
                output->wlr_output->height <= 0)) {
        wlr_log(WLR_ERROR,
            "leme: output %s has no available power-on mode", name);
        return false;
    }
    if (on && !leme_output_plan_box(&plan, &target_box)) {
        wlr_log(WLR_ERROR,
            "leme: output %s has unsafe power-on geometry", name);
        return false;
    }
    if (on && leme_output_power_plan_overlaps(
            output, &target_box, &conflict)) {
        wlr_log(WLR_ERROR,
            "leme: output %s power-on conflicts with enabled output %s",
            name, conflict == NULL ? "unknown" : conflict);
        return false;
    }

    scene_output_before = output->scene_output;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, on);
    if (on) {
        if (plan.mode != NULL) {
            wlr_output_state_set_mode(&state, plan.mode);
        } else {
            wlr_output_state_set_custom_mode(&state,
                output->wlr_output->width, output->wlr_output->height,
                output->wlr_output->refresh);
        }
        wlr_output_state_set_scale(&state, plan.entry.scale);
        wlr_output_state_set_transform(&state,
            leme_output_transform(plan.entry.transform));
        wake_box = (struct leme_box){
            .x = target_box.x,
            .y = target_box.y,
            .width = target_box.width,
            .height = target_box.height,
        };
        if (!leme_session_prepare_output_wake(server, &wake_box)) {
            failure_stage = "locked wake preparation";
            goto failed;
        }
        if (!leme_render_prepare_output_state(output, &state, false)) {
            failure_stage = "isolated render preparation";
            goto failed;
        }
        if (!leme_render_prepare_output_attachment(output)) {
            failure_stage = "persistent scene allocation";
            goto failed;
        }
        attachment_created = output->scene_output != scene_output_before;
    }
    if (!wlr_output_test_state(output->wlr_output, &state)) {
        failure_stage = "backend test";
        goto failed;
    }
    if (!on) {
        leme_render_output_animations_finish(output);
    }
    if (!wlr_output_commit_state(output->wlr_output, &state)) {
        failure_stage = "backend commit";
        goto failed;
    }
    wlr_output_state_finish(&state);

    previous = server->focused_output;
    previous_x = output->layout_x;
    previous_y = output->layout_y;
    if (on) {
        output->layout_x = plan.x;
        output->layout_y = plan.y;
    }
    if (!leme_output_reconcile(server)) {
        bool rolled_back = true;

        output->layout_x = previous_x;
        output->layout_y = previous_y;
        if (server->output_layout != NULL) {
            wlr_output_layout_remove(
                server->output_layout, output->wlr_output);
        }
        leme_render_detach_output(output);
        if (on) {
            struct wlr_output_state rollback;

            wlr_output_state_init(&rollback);
            wlr_output_state_set_enabled(&rollback, false);
            rolled_back = wlr_output_test_state(
                    output->wlr_output, &rollback) &&
                wlr_output_commit_state(output->wlr_output, &rollback);
            wlr_output_state_finish(&rollback);
            leme_session_restore_output_wake(server);
        } else {
            struct wlr_output_state rollback;

            /* Desligar pode falhar a meio de reconstruir a ligação à cena
             * de outra saída activa. Repõe-se o backend antes de o gestor de
             * ciclo de vida ver a tentativa, e só depois se reconstrói a
             * topologia anterior inteira. */
            wlr_output_state_init(&rollback);
            wlr_output_state_set_enabled(&rollback, true);
            rolled_back = wlr_output_test_state(
                    output->wlr_output, &rollback) &&
                wlr_output_commit_state(output->wlr_output, &rollback);
            wlr_output_state_finish(&rollback);
            if (rolled_back) {
                rolled_back = leme_output_reconcile(server);
            }
        }
        leme_output_publish_configuration(server);
        wlr_log(WLR_ERROR,
            "leme: output reconciliation failed while powering %s output %s%s",
            on ? "on" : "off", name,
            rolled_back ? "" : "; backend rollback also failed");
        return false;
    }
    output->power_on = on;
    leme_output_power_warp_focus(server, previous);
    leme_output_publish_configuration(server);
    if (on) {
        wlr_output_schedule_frame(output->wlr_output);
    }
    return true;

failed:
    wlr_output_state_finish(&state);
    if (attachment_created) {
        leme_render_detach_output(output);
    }
    if (on) {
        leme_session_restore_output_wake(server);
    }
    wlr_log(WLR_ERROR, "leme: %s failed while powering %s output %s",
        failure_stage == NULL ? "unknown stage" : failure_stage,
        on ? "on" : "off", name);
    return false;
}

bool
leme_output_test_config(struct leme_server *server,
    const struct leme_config *config)
{
    struct wlr_output_configuration_v1 *configuration;
    bool mode_fallback;
    bool valid;

    if (server->outputs.next == NULL || wl_list_empty(&server->outputs)) {
        return true;
    }
    configuration = leme_output_build_persistent(
        server, config, false, &mode_fallback);
    if (configuration == NULL) {
        return false;
    }
    if (leme_output_configuration_matches_current(server, configuration)) {
        wlr_output_configuration_v1_destroy(configuration);
        return true;
    }
    valid = leme_output_configuration_test(server, configuration);
    wlr_output_configuration_v1_destroy(configuration);
    return valid;
}

bool
leme_output_apply_config(struct leme_server *server,
    const struct leme_config *config, bool startup)
{
    struct wlr_output_configuration_v1 *configuration;
    struct leme_output *target;
    bool mode_fallback;
    bool committed;

    if (server->outputs.next == NULL || wl_list_empty(&server->outputs)) {
        return true;
    }
    target = leme_output_target(server, config, startup);
    configuration = leme_output_build_persistent(
        server, config, startup, &mode_fallback);
    if (configuration == NULL || target == NULL) {
        if (configuration != NULL) {
            wlr_output_configuration_v1_destroy(configuration);
        }
        return false;
    }
    if (mode_fallback) {
        wlr_log(WLR_ERROR,
            "leme: configured mode unavailable on %s, using preferred mode",
            target->wlr_output->name);
    }
    if (!startup && leme_output_configuration_matches_current(
            server, configuration)) {
        wlr_output_configuration_v1_destroy(configuration);
        return true;
    }
    committed = leme_output_commit_and_reconcile(server, configuration);
    wlr_output_configuration_v1_destroy(configuration);
    if (!committed) {
        return false;
    }
    leme_output_publish_configuration(server);
    if (server->focused_output != NULL) {
        wlr_log(WLR_INFO, "leme: output %s active at %dx%d %.3f Hz scale %.2f",
            server->focused_output->wlr_output->name,
            server->focused_output->wlr_output->width,
            server->focused_output->wlr_output->height,
            server->focused_output->wlr_output->refresh / 1000.0,
            (double)server->focused_output->wlr_output->scale);
    }
    return true;
}

void
leme_output_publish_configuration(struct leme_server *server)
{
    struct wlr_output_configuration_v1 *configuration;
    struct leme_output *output;

    if (server->output_manager == NULL) {
        return;
    }
    configuration = wlr_output_configuration_v1_create();
    if (configuration == NULL) {
        return;
    }
    wl_list_for_each(output, &server->outputs, link) {
        struct wlr_output_configuration_head_v1 *head =
            wlr_output_configuration_head_v1_create(
                configuration, output->wlr_output);

        if (head == NULL) {
            wlr_output_configuration_v1_destroy(configuration);
            return;
        }
        head->state.x = output->layout_x;
        head->state.y = output->layout_y;
    }
    wlr_output_manager_v1_set_configuration(
        server->output_manager, configuration);
}

static bool
leme_output_mode_is_advertised(struct wlr_output *output,
    const struct wlr_output_mode *requested)
{
    struct wlr_output_mode *mode;

    wl_list_for_each(mode, &output->modes, link) {
        if (mode == requested) {
            return true;
        }
    }
    return false;
}

static bool
leme_output_head_box(
    const struct wlr_output_configuration_head_v1 *head, struct wlr_box *box)
{
    const struct wlr_output_mode *mode = head->state.mode;
    const struct wlr_output *wlr_output = head->state.output;

    *box = (struct wlr_box){
        .x = head->state.x,
        .y = head->state.y,
    };
    return leme_output_logical_size(
            mode != NULL ? mode->width : wlr_output->width,
            mode != NULL ? mode->height : wlr_output->height,
            head->state.scale, head->state.transform,
            &box->width, &box->height) &&
        leme_output_box_edges(box, NULL, NULL);
}

static bool
leme_output_heads_overlap(
    const struct wlr_output_configuration_v1 *configuration)
{
    struct wlr_output_configuration_head_v1 *head;
    struct wlr_output_configuration_head_v1 *other;

    wl_list_for_each(head, &configuration->heads, link) {
        struct wlr_box first;

        if (!head->state.enabled) {
            continue;
        }
        if (!leme_output_head_box(head, &first)) {
            return true;
        }
        wl_list_for_each(other, &configuration->heads, link) {
            struct wlr_box second;

            if (other == head || !other->state.enabled) {
                continue;
            }
            if (!leme_output_head_box(other, &second) ||
                    !leme_output_boxes_fit_layout(&first, &second) ||
                    leme_output_boxes_overlap(&first, &second)) {
                return true;
            }
        }
    }
    return false;
}

static const struct wlr_output_configuration_head_v1 *
leme_output_configuration_head_for(
    const struct wlr_output_configuration_v1 *configuration,
    const struct wlr_output *output)
{
    struct wlr_output_configuration_head_v1 *head;

    wl_list_for_each(head, &configuration->heads, link) {
        if (head->state.output == output) {
            return head;
        }
    }
    return NULL;
}

static bool
leme_output_request_box(
    const struct wlr_output_configuration_v1 *configuration,
    const struct leme_output *output, bool *enabled, struct wlr_box *box)
{
    const struct wlr_output_configuration_head_v1 *head =
        leme_output_configuration_head_for(
            configuration, output->wlr_output);

    if (head != NULL) {
        *enabled = head->state.enabled;
        return !*enabled || leme_output_head_box(head, box);
    }
    *enabled = output->wlr_output->enabled;
    if (*enabled) {
        struct leme_box current = leme_output_full_box(output);

        *box = (struct wlr_box){
            .x = current.x,
            .y = current.y,
            .width = current.width,
            .height = current.height,
        };
        return leme_output_box_edges(box, NULL, NULL);
    }
    return true;
}

static bool
leme_output_request_layout_valid(
    const struct leme_server *server,
    const struct wlr_output_configuration_v1 *configuration)
{
    struct leme_output *output;
    struct leme_output *other;

    wl_list_for_each(output, &server->outputs, link) {
        struct wlr_box first = {0};
        bool first_enabled;

        if (!leme_output_request_box(
                configuration, output, &first_enabled, &first)) {
            return false;
        }
        if (!first_enabled) {
            continue;
        }
        wl_list_for_each(other, &server->outputs, link) {
            struct wlr_box second = {0};
            bool second_enabled;

            if (other == output) {
                continue;
            }
            if (!leme_output_request_box(
                    configuration, other, &second_enabled, &second)) {
                return false;
            }
            if (second_enabled &&
                    (!leme_output_boxes_fit_layout(&first, &second) ||
                        leme_output_boxes_overlap(&first, &second))) {
                return false;
            }
        }
    }
    return true;
}

static bool
leme_output_request_valid(struct leme_server *server,
    const struct wlr_output_configuration_v1 *configuration)
{
    struct wlr_output_configuration_head_v1 *head;
    struct wlr_output_configuration_head_v1 *previous;
    struct leme_output *output;
    size_t enabled = 0;

    wl_list_for_each(output, &server->outputs, link) {
        enabled += output->wlr_output->enabled ? 1 : 0;
    }
    wl_list_for_each(head, &configuration->heads, link) {
        struct wlr_output *wlr_output = head->state.output;
        bool current_adaptive;

        output = leme_output_from_wlr(server, wlr_output);
        if (output == NULL ||
                !isfinite(head->state.scale) ||
                head->state.scale < 0.5f || head->state.scale > 4.0f ||
                head->state.transform < WL_OUTPUT_TRANSFORM_NORMAL ||
                head->state.transform > WL_OUTPUT_TRANSFORM_FLIPPED_270) {
            return false;
        }
        wl_list_for_each(previous, &configuration->heads, link) {
            if (previous == head) {
                break;
            }
            if (previous->state.output == wlr_output) {
                return false;
            }
        }
        if (wlr_output->enabled && !head->state.enabled) {
            enabled--;
        } else if (!wlr_output->enabled && head->state.enabled) {
            enabled++;
        }
        current_adaptive = wlr_output->adaptive_sync_status ==
            WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED;
        if (head->state.adaptive_sync_enabled != current_adaptive) {
            return false;
        }
        if (head->state.enabled &&
                (head->state.mode == NULL ||
                    !leme_output_mode_is_advertised(
                        wlr_output, head->state.mode))) {
            return false;
        }
    }
    return enabled >= 1 &&
        leme_output_request_layout_valid(server, configuration);
}

static void
leme_output_handle_manager_test(struct wl_listener *listener, void *data)
{
    struct leme_server *server =
        wl_container_of(listener, server, output_manager_test);
    struct wlr_output_configuration_v1 *configuration = data;
    bool valid = leme_output_request_valid(server, configuration) &&
        leme_output_configuration_test(server, configuration);

    if (valid) {
        wlr_output_configuration_v1_send_succeeded(configuration);
    } else {
        wlr_output_configuration_v1_send_failed(configuration);
    }
    wlr_output_configuration_v1_destroy(configuration);
}

static void
leme_output_handle_manager_apply(struct wl_listener *listener, void *data)
{
    struct leme_server *server =
        wl_container_of(listener, server, output_manager_apply);
    struct wlr_output_configuration_v1 *configuration = data;
    bool valid = leme_output_request_valid(server, configuration) &&
        leme_output_commit_and_reconcile(server, configuration);

    if (valid) {
        wlr_output_configuration_v1_send_succeeded(configuration);
    } else {
        wlr_output_configuration_v1_send_failed(configuration);
    }
    wlr_output_configuration_v1_destroy(configuration);
    if (valid) {
        leme_output_publish_configuration(server);
    }
}

static void
leme_output_handle_commit(struct wl_listener *listener, void *data)
{
    struct leme_output *output =
        wl_container_of(listener, output, commit);

    (void)data;
    if (output->server->focused_output == output && output->wlr_output->enabled) {
        leme_output_refresh_geometry(output);
        leme_desktop_output_changed(output->server);
        leme_session_output_changed(output->server);
    }
    leme_session_refresh_idle_inhibitors(output->server);
}

static struct leme_output *
leme_output_surviving(struct leme_server *server,
    const struct leme_output *dying)
{
    struct leme_output *output;

    wl_list_for_each(output, &server->outputs, link) {
        if (output != dying && output->wlr_output->enabled) {
            return output;
        }
    }
    wl_list_for_each(output, &server->outputs, link) {
        if (output != dying) {
            return output;
        }
    }
    return NULL;
}

static void
leme_output_migrate_views(struct leme_output *from, struct leme_output *to)
{
    struct leme_tags *source = leme_output_tags(from);
    struct leme_tags *destination = leme_output_tags(to);
    uint16_t id;

    if (source == NULL || destination == NULL || source == destination) {
        return;
    }
    for (id = 1; id <= source->max_tags; id++) {
        while (source->table[id] != NULL &&
                !wl_list_empty(&source->table[id]->views)) {
            struct leme_view *view = wl_container_of(
                source->table[id]->views.next, view, tag_link);

            if (!leme_tags_adopt_view(destination, view, id)) {
                break;
            }
        }
    }
}

static void
leme_output_ensure_fallback(struct leme_server *server)
{
    struct leme_output *output;

    wl_list_for_each(output, &server->outputs, link) {
        if (output->wlr_output->enabled) {
            return;
        }
    }
    if (server->headless_backend == NULL) {
        server->headless_backend = wlr_headless_backend_create(
            wl_display_get_event_loop(server->display));
        if (server->headless_backend == NULL) {
            wlr_log(WLR_ERROR, "%s",
                "leme: failed to create the fallback headless backend");
            return;
        }
        if (!wlr_multi_backend_add(server->backend,
                server->headless_backend)) {
            wlr_log(WLR_ERROR, "%s",
                "leme: failed to attach the fallback headless backend");
            wlr_backend_destroy(server->headless_backend);
            server->headless_backend = NULL;
            return;
        }
        if (server->started &&
                !wlr_backend_start(server->headless_backend)) {
            wlr_log(WLR_ERROR, "%s",
                "leme: failed to start the fallback headless backend");
            wlr_multi_backend_remove(server->backend,
                server->headless_backend);
            wlr_backend_destroy(server->headless_backend);
            server->headless_backend = NULL;
            return;
        }
    }
    if (wlr_headless_add_output(server->headless_backend, 1920, 1080) ==
            NULL) {
        wlr_log(WLR_ERROR, "%s",
            "leme: failed to create the fallback headless output");
        return;
    }
    wlr_log(WLR_INFO, "%s",
        "leme: no outputs remain, running on a fallback headless output");
}

static void
leme_output_retire_fallback(struct leme_server *server)
{
    struct leme_output *output;
    struct leme_output *fallback = NULL;

    if (server->headless_backend == NULL) {
        return;
    }
    wl_list_for_each(output, &server->outputs, link) {
        if (output->wlr_output->backend == server->headless_backend) {
            fallback = output;
            break;
        }
    }
    if (fallback == NULL) {
        return;
    }
    wlr_log(WLR_INFO, "%s",
        "leme: a real output returned, retiring the fallback headless output");
    wlr_output_destroy(fallback->wlr_output);
}

static void
leme_output_handle_destroy(struct wl_listener *listener, void *data)
{
    struct leme_output *output =
        wl_container_of(listener, output, destroy);
    struct leme_server *server = output->server;
    struct leme_output *successor;
    bool was_active = server->focused_output == output;

    (void)data;
    leme_scratchpad_handle_output_destroy(server, output);
    leme_input_pointer_grab_cancel(server);
    leme_render_output_animations_finish(output);
    leme_workspace_release_output(output);
    leme_layer_handle_output_destroy(server, output->wlr_output);
    leme_render_detach_output(output);
    wlr_output_layout_remove(server->output_layout, output->wlr_output);
    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->commit.link);
    wl_list_remove(&output->destroy.link);
    wl_list_remove(&output->link);
    successor = leme_output_surviving(server, output);
    if (successor == NULL) {
        leme_output_ensure_fallback(server);
        successor = leme_output_surviving(server, output);
    }
    if (successor != NULL) {
        leme_output_migrate_views(output, successor);
    }
    if (was_active) {
        server->focused_output = successor;
        leme_session_output_changed(server);
    }
    leme_tags_finish(&output->tags);
    free(output);
    if (!wl_list_empty(&server->outputs)) {
        if (!leme_output_apply_config(server, server->config, true)) {
            wlr_log(WLR_ERROR, "%s",
                "leme: failed to reconfigure the remaining outputs");
        }
    } else {
        leme_output_publish_configuration(server);
    }
}

static bool
leme_output_init_tags(struct leme_output *output)
{
    const struct leme_config *config = output->server->config;
    uint16_t initial = config == NULL ? 1 : config->initial_tags;
    uint16_t maximum = config == NULL ? 9 : config->max_tags;

    output->tags.server = output->server;
    output->tags.output = output;
    if (!leme_tags_init(&output->tags, initial, maximum)) {
        return false;
    }
    return true;
}

static void
leme_output_handle_new(struct wl_listener *listener, void *data)
{
    struct leme_server *server =
        wl_container_of(listener, server, new_output);
    struct wlr_output *wlr_output = data;
    struct leme_output *output;

    if (!wlr_output_init_render(
            wlr_output, server->allocator, server->renderer)) {
        wlr_log(WLR_ERROR,
            "leme: failed to initialize output %s", wlr_output->name);
        return;
    }
    output = calloc(1, sizeof(*output));
    if (output == NULL) {
        wlr_log(WLR_ERROR, "%s", "leme: failed to allocate output state");
        return;
    }
    output->server = server;
    output->wlr_output = wlr_output;
    output->power_on = true;
    if (!leme_output_init_tags(output)) {
        wlr_log(WLR_ERROR, "leme: failed to allocate tags for output %s",
            wlr_output->name);
        free(output);
        return;
    }
    wl_list_insert(server->outputs.prev, &output->link);
    output->frame.notify = leme_output_handle_frame;
    wl_signal_add(&wlr_output->events.frame, &output->frame);
    output->commit.notify = leme_output_handle_commit;
    wl_signal_add(&wlr_output->events.commit, &output->commit);
    output->destroy.notify = leme_output_handle_destroy;
    wl_signal_add(&wlr_output->events.destroy, &output->destroy);

    if (!leme_output_apply_config(server, server->config, true)) {
        wlr_log(WLR_ERROR, "leme: failed to configure output %s",
            wlr_output->name);
        leme_output_publish_configuration(server);
    }
    if (wlr_output->backend != server->headless_backend) {
        leme_output_retire_fallback(server);
    }
}

bool
leme_output_init(struct leme_server *server)
{
    wl_list_init(&server->outputs);
    server->xdg_output_manager = wlr_xdg_output_manager_v1_create(
        server->display, server->output_layout);
    server->output_manager =
        wlr_output_manager_v1_create(server->display);
    if (server->xdg_output_manager == NULL ||
            server->output_manager == NULL) {
        return false;
    }
    server->output_manager_apply.notify = leme_output_handle_manager_apply;
    wl_signal_add(&server->output_manager->events.apply,
        &server->output_manager_apply);
    server->output_manager_test.notify = leme_output_handle_manager_test;
    wl_signal_add(&server->output_manager->events.test,
        &server->output_manager_test);
    wl_list_init(&server->new_output.link);
    server->new_output.notify = leme_output_handle_new;
    wl_signal_add(&server->backend->events.new_output, &server->new_output);
    return true;
}

void
leme_output_finish(struct leme_server *server)
{
    struct leme_output *output;
    struct leme_output *temporary;

    if (server->output_manager_apply.link.next != NULL) {
        wl_list_remove(&server->output_manager_apply.link);
        server->output_manager_apply.link.next = NULL;
        server->output_manager_apply.link.prev = NULL;
    }
    if (server->output_manager_test.link.next != NULL) {
        wl_list_remove(&server->output_manager_test.link);
        server->output_manager_test.link.next = NULL;
        server->output_manager_test.link.prev = NULL;
    }
    server->output_manager = NULL;
    if (server->outputs.next == NULL) {
        return;
    }
    if (server->new_output.link.next != NULL) {
        wl_list_remove(&server->new_output.link);
        server->new_output.link.next = NULL;
        server->new_output.link.prev = NULL;
    }
    wl_list_for_each_safe(output, temporary, &server->outputs, link) {
        leme_render_output_animations_finish(output);
        leme_render_detach_output(output);
        wl_list_remove(&output->frame.link);
        wl_list_remove(&output->commit.link);
        wl_list_remove(&output->destroy.link);
        wl_list_remove(&output->link);
        leme_tags_finish(&output->tags);
        free(output);
    }
    server->focused_output = NULL;
}

struct leme_box
leme_output_usable_box(const struct leme_output *output)
{
    return output == NULL ? (struct leme_box){0} : output->usable_box;
}

struct leme_box
leme_output_full_box(const struct leme_output *output)
{
    return output == NULL ? (struct leme_box){0} : output->full_box;
}

static bool
leme_output_boxes_equal(struct leme_box left, struct leme_box right)
{
    return left.x == right.x && left.y == right.y &&
        left.width == right.width && left.height == right.height;
}

void
leme_output_refresh_geometry(struct leme_output *output)
{
    struct wlr_box box = {0};
    struct leme_box next;

    if (output == NULL) {
        return;
    }
    wlr_output_layout_get_box(output->server->output_layout,
        output->wlr_output, &box);
    if (wlr_box_empty(&box)) {
        box.x = 0;
        box.y = 0;
        wlr_output_effective_resolution(
            output->wlr_output, &box.width, &box.height);
    }
    next = (struct leme_box){
        .x = box.x,
        .y = box.y,
        .width = box.width,
        .height = box.height,
    };
    if (!leme_output_boxes_equal(next, output->full_box)) {
        leme_render_output_animations_finish(output);
    }
    output->full_box = next;
    output->usable_box = output->full_box;
}

struct leme_output *
leme_output_focused(const struct leme_server *server)
{
    return server == NULL ? NULL : server->focused_output;
}

struct leme_output *
leme_output_from_wlr_output(
    struct leme_server *server, const struct wlr_output *wlr_output)
{
    if (server == NULL || wlr_output == NULL ||
            server->outputs.next == NULL) {
        return NULL;
    }
    return leme_output_from_wlr(server, wlr_output);
}

struct leme_output *
leme_output_by_name(struct leme_server *server, const char *name)
{
    struct leme_output *output;

    if (server == NULL || name == NULL || server->outputs.next == NULL) {
        return NULL;
    }
    output = leme_output_find(server, name);
    return output != NULL && output->wlr_output->enabled ? output : NULL;
}

struct leme_output *
leme_output_at(struct leme_server *server, double layout_x, double layout_y)
{
    struct wlr_output *wlr_output;

    if (server == NULL || server->output_layout == NULL) {
        return NULL;
    }
    wlr_output = wlr_output_layout_output_at(
        server->output_layout, layout_x, layout_y);
    return leme_output_from_wlr_output(server, wlr_output);
}

struct leme_output *
leme_output_adjacent(struct leme_server *server,
    const struct leme_output *origin, enum leme_direction direction)
{
    static const enum wlr_direction directions[] = {
        [LEME_DIRECTION_LEFT] = WLR_DIRECTION_LEFT,
        [LEME_DIRECTION_RIGHT] = WLR_DIRECTION_RIGHT,
        [LEME_DIRECTION_UP] = WLR_DIRECTION_UP,
        [LEME_DIRECTION_DOWN] = WLR_DIRECTION_DOWN,
    };
    struct leme_box box;
    struct wlr_output *adjacent;

    if (server == NULL || origin == NULL ||
            direction < LEME_DIRECTION_LEFT ||
            direction > LEME_DIRECTION_DOWN) {
        return NULL;
    }
    box = leme_output_full_box(origin);
    adjacent = wlr_output_layout_adjacent_output(server->output_layout,
        directions[direction], origin->wlr_output,
        box.x + box.width / 2.0, box.y + box.height / 2.0);
    return leme_output_from_wlr_output(server, adjacent);
}

void
leme_output_set_focused(struct leme_server *server,
    struct leme_output *output, bool warp)
{
    struct leme_box box;
    struct leme_tags *tags;

    if (server == NULL || output == NULL ||
            !output->wlr_output->enabled) {
        return;
    }
    server->focused_output = output;
    if (warp && server->cursor != NULL) {
        box = leme_output_usable_box(output);
        wlr_cursor_warp(server->cursor, NULL,
            box.x + box.width / 2.0, box.y + box.height / 2.0);
    }
    tags = leme_output_tags(output);
    if (tags != NULL) {
        leme_tags_refresh_visibility(tags);
    }
    leme_desktop_output_changed(server);
    leme_publication_invalidate(server);
}

struct leme_output *
leme_view_output(const struct leme_view *view)
{
    if (view == NULL) {
        return NULL;
    }
    if (view->scratchpad_state == LEME_SCRATCHPAD_SHOWN &&
            view->server != NULL && view->server->scratchpads.shown == view) {
        return view->server->scratchpads.shown_output;
    }
    if (view->scratchpad_state != LEME_SCRATCHPAD_NONE || view->tag == NULL ||
            view->tag->owner == NULL) {
        return NULL;
    }
    return view->tag->owner->output;
}

struct leme_tags *
leme_output_tags(struct leme_output *output)
{
    return output == NULL ? NULL : &output->tags;
}

struct leme_tags *
leme_focused_tags(const struct leme_server *server)
{
    return leme_output_tags(leme_output_focused(server));
}
