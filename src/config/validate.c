#include "config/config.h"
#include "config/internal.h"

#include "core/command.h"
#include "core/server.h"
#include "core/session_environment.h"
#include "input/input.h"
#include "output/output.h"
#include "protocols/desktop.h"
#include "render/render.h"
#include "workspace/tag.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon-keysyms.h>

static bool
leme_config_same_text(const char *first, const char *second)
{
    if (first == NULL || second == NULL) {
        return first == second;
    }
    return strcmp(first, second) == 0;
}

static bool
leme_config_validate_outputs(const struct leme_config *config, char **error)
{
    size_t index;

    for (index = 0; index < config->output_count; index++) {
        const struct leme_output_config *entry = &config->outputs[index];
        size_t other;

        if (!isfinite(entry->scale) || entry->scale < 0.5f ||
                entry->scale > 4.0f ||
                entry->transform < LEME_OUTPUT_TRANSFORM_NORMAL ||
                entry->transform > LEME_OUTPUT_TRANSFORM_270 ||
                entry->name == NULL || entry->name[0] == '\0' ||
                (entry->has_mode &&
                    (entry->width <= 0 || entry->height <= 0)) ||
                (entry->has_refresh &&
                    (!entry->has_mode || entry->refresh_mhz <= 0))) {
            leme_config_set_error(error,
                "config: invalid output configuration");
            return false;
        }
        if (entry->relation != LEME_OUTPUT_RELATION_NONE &&
                (entry->relative_to == NULL ||
                    entry->relative_to[0] == '\0' ||
                    strcmp(entry->relative_to, entry->name) == 0)) {
            leme_config_set_error(error,
                "config: output %s has an invalid relative placement",
                entry->name);
            return false;
        }
        for (other = 0; other < index; other++) {
            if (strcmp(config->outputs[other].name, entry->name) == 0) {
                leme_config_set_error(error,
                    "config: duplicate output %s", entry->name);
                return false;
            }
        }
    }
    return true;
}

static bool
leme_config_validate_scratchpads(const struct leme_config *config,
    char **error)
{
    size_t index;

    if (config->scratchpad_count > 0 && config->scratchpads == NULL) {
        leme_config_set_error(error, "config: invalid scratchpad configuration");
        return false;
    }
    for (index = 0; index < config->scratchpad_count; index++) {
        const struct leme_scratchpad_config *scratchpad =
            &config->scratchpads[index];
        size_t previous;

        if (scratchpad->name == NULL || scratchpad->name[0] == '\0' ||
                strpbrk(scratchpad->name, " \t\n\r\f\v") != NULL ||
                scratchpad->identity == NULL || scratchpad->identity[0] == '\0' ||
                scratchpad->spawn == NULL || scratchpad->spawn[0] == NULL ||
                scratchpad->spawn[0][0] == '\0' ||
                !isfinite(scratchpad->width) ||
                !isfinite(scratchpad->height) ||
                scratchpad->width < 0.1 || scratchpad->width > 1.0 ||
                scratchpad->height < 0.1 || scratchpad->height > 1.0) {
            leme_config_set_error(error,
                "config: invalid scratchpad configuration");
            return false;
        }
        for (previous = 0; previous < index; previous++) {
            const struct leme_scratchpad_config *earlier =
                &config->scratchpads[previous];

            if (strcmp(earlier->name, scratchpad->name) == 0 ||
                    strcmp(earlier->identity, scratchpad->identity) == 0) {
                leme_config_set_error(error,
                    "config: duplicate scratchpad name or identity %s",
                    scratchpad->name);
                return false;
            }
        }
    }
    return true;
}

static bool
leme_config_validate_tag_rules(const struct leme_config *config, char **error)
{
    size_t rule_index;
    size_t id_index;
    size_t other_rule;
    size_t other_id;

    for (rule_index = 0; rule_index < config->tag_rule_count; rule_index++) {
        const struct leme_tag_rule *rule = &config->tag_rules[rule_index];

        for (id_index = 0; id_index < rule->id_count; id_index++) {
            uint16_t id = rule->ids[id_index];

            if (id == 0 || id > config->max_tags) {
                leme_config_set_error(error,
                    "config: tag selector id %u is outside 1..%u",
                    id, config->max_tags);
                return false;
            }
            for (other_rule = 0; other_rule < rule_index; other_rule++) {
                const struct leme_tag_rule *previous =
                    &config->tag_rules[other_rule];

                for (other_id = 0; other_id < previous->id_count;
                        other_id++) {
                    if (previous->ids[other_id] == id) {
                        leme_config_set_error(error,
                            "config: tag %u is covered by more than one "
                            "tag block", id);
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

static bool
leme_config_has_mode(const struct leme_config *config, const char *name)
{
    size_t index;

    for (index = 0; index < config->mode_count; index++) {
        if (strcmp(config->modes[index].name, name) == 0) {
            return true;
        }
    }
    return false;
}

bool
leme_config_validate_command(const struct leme_config *config,
    const struct leme_command *command)
{
    switch (command->type) {
    case LEME_COMMAND_MOVE_VIEW_TO_TAG:
        if (command->has_direction) {
            return command->direction == LEME_DIRECTION_LEFT ||
                command->direction == LEME_DIRECTION_RIGHT;
        }
        return command->tag_id > 0 && command->tag_id <= config->max_tags;
    case LEME_COMMAND_FOCUS_TAG:
    case LEME_COMMAND_REMOVE_EMPTY_TAG:
        return command->tag_id > 0 && command->tag_id <= config->max_tags;
    case LEME_COMMAND_FOCUS_DIRECTION:
        return command->direction >= LEME_DIRECTION_LEFT &&
            command->direction <= LEME_DIRECTION_DOWN;
    case LEME_COMMAND_SET_LAYOUT:
        return command->layout >= LEME_LAYOUT_DWINDLE &&
            command->layout <= LEME_LAYOUT_ACCORDION;
    case LEME_COMMAND_SWITCH_LAYOUT:
        return true;
    case LEME_COMMAND_FOCUS_OUTPUT:
    case LEME_COMMAND_MOVE_VIEW_TO_OUTPUT:
        return command->has_direction ?
            (command->direction >= LEME_DIRECTION_LEFT &&
                command->direction <= LEME_DIRECTION_DOWN) :
            (command->text != NULL && command->text[0] != '\0');
    case LEME_COMMAND_MOVE_DIRECTION:
        return command->direction >= LEME_DIRECTION_LEFT &&
            command->direction <= LEME_DIRECTION_DOWN && command->amount > 0;
    case LEME_COMMAND_RESIZE:
        return command->amount > 0 &&
            command->edge >= LEME_RESIZE_LEFT &&
            command->edge <= LEME_RESIZE_DOWN;
    case LEME_COMMAND_SPAWN:
        return command->argv != NULL && command->argv[0] != NULL;
    case LEME_COMMAND_SET_MODE:
        return command->text != NULL && command->text[0] != '\0' &&
            leme_config_has_mode(config, command->text);
    case LEME_COMMAND_SWITCH_VT:
        return command->vt > 0 && command->vt <= 12;
    case LEME_COMMAND_FOCUS_NEXT_TAG:
    case LEME_COMMAND_FOCUS_PREVIOUS_TAG:
    case LEME_COMMAND_FOCUS_LAST_TAG:
    case LEME_COMMAND_FOCUS_PREVIOUS_VIEW:
    case LEME_COMMAND_CYCLE_KEYBOARD_LAYOUT:
    case LEME_COMMAND_TOGGLE_FLOATING:
    case LEME_COMMAND_TOGGLE_STICKY:
    case LEME_COMMAND_TOGGLE_FULLSCREEN:
    case LEME_COMMAND_CLOSE_VIEW:
    case LEME_COMMAND_RELOAD_CONFIG:
    case LEME_COMMAND_QUIT:
    case LEME_COMMAND_SCRATCHPAD_TOGGLE:
        return command->text == NULL || (command->text[0] != '\0' &&
            leme_config_scratchpad(config, command->text) != NULL);
    case LEME_COMMAND_SCRATCHPAD_SEND:
    case LEME_COMMAND_SCRATCHPAD_RETRIEVE:
        return true;
    }
    return false;
}

static bool
leme_config_pointer_settings_valid(
    const struct leme_pointer_settings *settings)
{
    const uint32_t all_fields = LEME_POINTER_PROFILE | LEME_POINTER_SPEED |
        LEME_POINTER_NATURAL_SCROLL | LEME_POINTER_LEFT_HANDED |
        LEME_POINTER_TAP;

    return (settings->fields & ~all_fields) == 0 &&
        ((settings->fields & LEME_POINTER_PROFILE) == 0 ||
            settings->profile == LEME_POINTER_ACCEL_ADAPTIVE ||
            settings->profile == LEME_POINTER_ACCEL_FLAT) &&
        ((settings->fields & LEME_POINTER_SPEED) == 0 ||
            (isfinite(settings->speed) &&
                settings->speed >= -1.0 && settings->speed <= 1.0));
}

bool
leme_config_validate(const struct leme_config *config, char **error)
{
    size_t mode_index;
    size_t binding_index;
    size_t previous;
    bool have_common = false;

    if (config == NULL || config->initial_tags == 0 ||
            config->initial_tags > config->max_tags) {
        leme_config_set_error(error,
            "config: initial_tags must be between 1 and max_tags");
        return false;
    }
    if (config->gap < 0 || config->border_width < 0 ||
            config->corner_radius < 0 || config->blur < 0 ||
            config->blur > 64) {
        leme_config_set_error(error,
            "config: gap, border_width, corner_radius and blur are out "
            "of range");
        return false;
    }
    if (config->cursor.size <= 0 ||
            config->cursor.size > LEME_CURSOR_SIZE_MAX) {
        leme_config_set_error(error,
            "config: cursor size must be between 1 and %d",
            LEME_CURSOR_SIZE_MAX);
        return false;
    }
    if (!leme_config_validate_outputs(config, error)) {
        return false;
    }
    if (!leme_config_validate_tag_rules(config, error)) {
        return false;
    }
    if (!leme_config_validate_scratchpads(config, error)) {
        return false;
    }
    if (config->keyboard_layout_count == 0 ||
            config->keyboard_layout_count > 4) {
        leme_config_set_error(error,
            "config: between one and four keyboard layouts are required");
        return false;
    }
    if (config->pointer_defaults.fields !=
            (LEME_POINTER_PROFILE | LEME_POINTER_SPEED |
                LEME_POINTER_NATURAL_SCROLL | LEME_POINTER_LEFT_HANDED |
                LEME_POINTER_TAP) ||
            !leme_config_pointer_settings_valid(&config->pointer_defaults)) {
        leme_config_set_error(error, "config: invalid global pointer policy");
        return false;
    }
    for (previous = 0; previous < config->pointer_rule_count; previous++) {
        size_t earlier;

        if (config->pointer_rules[previous].name == NULL ||
                config->pointer_rules[previous].name[0] == '\0' ||
                !leme_config_pointer_settings_valid(
                    &config->pointer_rules[previous].settings)) {
            leme_config_set_error(error, "config: invalid pointer rule");
            return false;
        }
        for (earlier = 0; earlier < previous; earlier++) {
            if (strcmp(config->pointer_rules[earlier].name,
                    config->pointer_rules[previous].name) == 0) {
                leme_config_set_error(error,
                    "config: duplicate pointer device %s",
                    config->pointer_rules[previous].name);
                return false;
            }
        }
    }
    for (previous = 0; previous < config->keyboard_layout_count; previous++) {
        if (config->keyboard_layouts[previous].name == NULL ||
                config->keyboard_layouts[previous].name[0] == '\0') {
            leme_config_set_error(error,
                "config: invalid keyboard layout entry");
            return false;
        }
    }
    for (mode_index = 0; mode_index < config->mode_count; mode_index++) {
        const struct leme_mode *mode = &config->modes[mode_index];

        if (mode->name == NULL || mode->name[0] == '\0') {
            leme_config_set_error(error, "config: mode name cannot be empty");
            return false;
        }
        have_common = have_common || strcmp(mode->name, "common") == 0;
        for (previous = 0; previous < mode_index; previous++) {
            if (strcmp(config->modes[previous].name, mode->name) == 0) {
                leme_config_set_error(error,
                    "config: duplicate mode %s", mode->name);
                return false;
            }
        }
        for (binding_index = 0; binding_index < mode->binding_count;
                binding_index++) {
            const struct leme_binding *binding =
                &mode->bindings[binding_index];
            if (binding->keysym == XKB_KEY_NoSymbol ||
                    !leme_config_validate_command(config, &binding->command)) {
                leme_config_set_error(error,
                    "config: invalid binding in mode %s", mode->name);
                return false;
            }
            for (previous = 0; previous < binding_index; previous++) {
                if (mode->bindings[previous].modifiers == binding->modifiers &&
                        mode->bindings[previous].keysym == binding->keysym) {
                    leme_config_set_error(error,
                        "config: duplicate binding in mode %s", mode->name);
                    return false;
                }
            }
        }
    }
    if (!have_common) {
        leme_config_set_error(error, "config: mode common is required");
        return false;
    }
    for (previous = 0; previous < config->environment_count; previous++) {
        size_t earlier;

        if (config->environment[previous].name == NULL ||
                config->environment[previous].name[0] == '\0' ||
                strchr(config->environment[previous].name, '=') != NULL ||
                config->environment[previous].value == NULL) {
            leme_config_set_error(error,
                "config: invalid environment entry");
            return false;
        }
        for (earlier = 0; earlier < previous; earlier++) {
            if (strcmp(config->environment[earlier].name,
                    config->environment[previous].name) == 0) {
                leme_config_set_error(error,
                    "config: duplicate environment entry");
                return false;
            }
        }
    }
    for (previous = 0; previous < config->startup_count; previous++) {
        if (config->startup[previous].argv == NULL ||
                config->startup[previous].argv[0] == NULL) {
            leme_config_set_error(error, "config: invalid exec entry");
            return false;
        }
    }
    return true;
}

bool
leme_config_apply(struct leme_server *server,
    struct leme_config *next, char **error)
{
    struct leme_config *old;
    struct xkb_keymap *keymap;
    struct leme_output *output;
    struct leme_tags_resize *resizes = NULL;
    size_t output_count = 0;
    size_t index = 0;

    if (!leme_config_validate(next, error)) {
        return false;
    }
    if (server->outputs.next != NULL) {
        wl_list_for_each(output, &server->outputs, link) {
            output_count++;
        }
        if (output_count > 0) {
            if (output_count > SIZE_MAX / sizeof(*resizes)) {
                leme_config_set_error(error, "config: too many outputs");
                return false;
            }
            resizes = calloc(output_count, sizeof(*resizes));
            if (resizes == NULL) {
                leme_config_set_error(error, "config: failed to prepare tag tables");
                return false;
            }
            wl_list_for_each(output, &server->outputs, link) {
                if (!leme_tags_can_set_max(leme_output_tags(output),
                        next->max_tags)) {
                    leme_config_set_error(error,
                        "config: maximum is below a materialized tag on %s",
                        output->wlr_output->name);
                    goto discard_resizes;
                }
                if (!leme_tags_prepare_set_max(leme_output_tags(output),
                        next->max_tags, &resizes[index])) {
                    leme_config_set_error(error,
                        "config: failed to prepare tag table on %s",
                        output->wlr_output->name);
                    goto discard_resizes;
                }
                index++;
            }
        }
    }
    keymap = leme_input_compile_keymap(next);
    if (keymap == NULL) {
        leme_config_set_error(error,
            "config: invalid keyboard layout configuration");
        goto discard_resizes;
    }
    if (!leme_output_test_config(server, next)) {
        leme_config_set_error(error, "config: output configuration failed");
        xkb_keymap_unref(keymap);
        goto discard_resizes;
    }
    if (!leme_input_apply_keymap(server, keymap)) {
        leme_config_set_error(error, "config: failed to apply keyboard keymap");
        xkb_keymap_unref(keymap);
        goto discard_resizes;
    }
    xkb_keymap_unref(keymap);
    if (!leme_output_apply_config(server, next, false)) {
        leme_config_set_error(error, "config: output configuration failed");
        goto discard_resizes;
    }

    old = server->config;
    for (index = 0; index < output_count; index++) {
        leme_tags_commit_set_max(&resizes[index]);
    }
    if (server->outputs.next != NULL) {
        wl_list_for_each(output, &server->outputs, link) {
            leme_tags_apply_settings(leme_output_tags(output),
                server->config, next);
        }
    }
    leme_scratchpad_reconcile_config(server, old, next);
    server->config = next;
    if (old == NULL || old->cursor.size != next->cursor.size ||
            !leme_config_same_text(old->cursor.theme, next->cursor.theme)) {
        leme_session_environment_cursor(server);
        if (server->desktop != NULL &&
                !leme_desktop_apply_cursor_config(server, next)) {
            wlr_log(WLR_ERROR, "%s",
                "leme: cursor theme unavailable; keeping the previous one");
        }
    }
    leme_input_replace_modes(server, next->modes, next->mode_count);
    leme_input_apply_pointer_config(server, next);
    if (leme_output_focused(server) != NULL) {
        leme_tags_arrange_current(leme_focused_tags(server),
            leme_output_usable_box(leme_output_focused(server)), next->gap);
        leme_render_refresh_views(server);
        leme_tags_refresh_visibility(leme_focused_tags(server));
    }
    leme_config_destroy(old);
    free(resizes);
    return true;

discard_resizes:
    for (index = 0; index < output_count; index++) {
        leme_tags_discard_set_max(&resizes[index]);
    }
    free(resizes);
    return false;
}

void
leme_config_launch_startup(struct leme_server *server)
{
    size_t index;

    for (index = 0; index < server->config->startup_count; index++) {
        struct leme_command command = {
            .type = LEME_COMMAND_SPAWN,
            .argv = server->config->startup[index].argv,
        };
        leme_command_execute(server, &command);
    }
}
