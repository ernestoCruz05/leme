#include "input/input.h"
#include "ipc/ipc.h"
#include "input/internal.h"

#include "config/config.h"
#include "core/command.h"
#include "core/server.h"
#include "protocols/input.h"
#include "protocols/session.h"

#include <stdlib.h>
#include <string.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

struct leme_keyboard {
    struct leme_server *server;
    struct wlr_keyboard *keyboard;
    bool handled[256];
    struct wl_listener key;
    struct wl_listener modifiers;
    struct wl_listener destroy;
    struct wl_list link;
};

static char *
leme_input_join_layouts(const struct leme_config *config, bool variants)
{
    char *result;
    char *cursor;
    size_t length = config->keyboard_layout_count;
    size_t index;

    for (index = 0; index < config->keyboard_layout_count; index++) {
        const char *value = variants ?
            config->keyboard_layouts[index].variant :
            config->keyboard_layouts[index].name;

        if (value != NULL) {
            length += strlen(value);
        }
    }
    result = malloc(length);
    if (result == NULL) {
        return NULL;
    }
    cursor = result;
    for (index = 0; index < config->keyboard_layout_count; index++) {
        const char *value = variants ?
            config->keyboard_layouts[index].variant :
            config->keyboard_layouts[index].name;
        size_t value_length = value == NULL ? 0 : strlen(value);

        if (index > 0) {
            *cursor++ = ',';
        }
        if (value_length > 0) {
            memcpy(cursor, value, value_length);
            cursor += value_length;
        }
    }
    *cursor = '\0';
    return result;
}

struct xkb_keymap *
leme_input_compile_keymap(const struct leme_config *config)
{
    struct xkb_context *context;
    struct xkb_keymap *keymap;
    char *layouts;
    char *variants = NULL;
    bool have_variant = false;
    size_t index;

    if (config == NULL || config->keyboard_layout_count == 0) {
        return NULL;
    }
    layouts = leme_input_join_layouts(config, false);
    for (index = 0; index < config->keyboard_layout_count; index++) {
        have_variant = have_variant ||
            config->keyboard_layouts[index].variant != NULL;
    }
    if (have_variant) {
        variants = leme_input_join_layouts(config, true);
    }
    if (layouts == NULL || (have_variant && variants == NULL)) {
        free(layouts);
        free(variants);
        return NULL;
    }
    context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (context == NULL) {
        free(layouts);
        free(variants);
        return NULL;
    }
    const struct xkb_rule_names names = {
        .layout = layouts,
        .variant = variants,
    };
    keymap = xkb_keymap_new_from_names(
        context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    xkb_context_unref(context);
    free(layouts);
    free(variants);
    if (keymap != NULL && xkb_keymap_num_layouts(keymap) !=
            config->keyboard_layout_count) {
        xkb_keymap_unref(keymap);
        keymap = NULL;
    }
    return keymap;
}

bool
leme_input_apply_keymap(struct leme_server *server,
    struct xkb_keymap *keymap)
{
    struct leme_keyboard *keyboard;

    if (server == NULL || keymap == NULL) {
        return false;
    }
    server->keyboard_layout = 0;
    if (server->cursor == NULL) {
        return true;
    }
    wl_list_for_each(keyboard, &server->keyboards, link) {
        if (!wlr_keyboard_set_keymap(keyboard->keyboard, keymap)) {
            wlr_log(WLR_ERROR, "%s", "leme: failed to apply keyboard keymap");
            return false;
        }
    }
    wl_list_for_each(keyboard, &server->keyboards, link) {
        struct wlr_keyboard_modifiers *modifiers =
            &keyboard->keyboard->modifiers;

        wlr_keyboard_notify_modifiers(keyboard->keyboard,
            modifiers->depressed, modifiers->latched,
            modifiers->locked, server->keyboard_layout);
    }
    return true;
}

bool
leme_input_cycle_keyboard_layout(struct leme_server *server)
{
    struct leme_keyboard *keyboard;
    size_t count;

    if (server == NULL || server->config == NULL ||
            server->config->keyboard_layout_count == 0) {
        wlr_log(WLR_ERROR, "%s", "leme: no keyboard layout is configured");
        return false;
    }
    count = server->config->keyboard_layout_count;
    server->keyboard_layout =
        (xkb_layout_index_t)((server->keyboard_layout + 1) % count);
    wl_list_for_each(keyboard, &server->keyboards, link) {
        struct wlr_keyboard_modifiers *modifiers =
            &keyboard->keyboard->modifiers;

        wlr_keyboard_notify_modifiers(keyboard->keyboard,
            modifiers->depressed, modifiers->latched,
            modifiers->locked, server->keyboard_layout);
    }
    wlr_log(WLR_INFO, "leme: keyboard layout=%s",
        server->config->keyboard_layouts[server->keyboard_layout].name);
    leme_ipc_invalidate(server);
    return true;
}

bool
leme_input_set_mode(struct leme_server *server, const char *name)
{
    size_t index;

    if (name == NULL) {
        return false;
    }
    for (index = 0; index < server->mode_count; index++) {
        if (strcmp(server->modes[index].name, name) == 0) {
            server->active_mode = &server->modes[index];
            leme_ipc_invalidate(server);
            return true;
        }
    }
    return false;
}

void
leme_input_replace_modes(struct leme_server *server,
    struct leme_mode *modes, size_t mode_count)
{
    const char *active_name = server->active_mode == NULL ?
        "common" : server->active_mode->name;

    server->modes = modes;
    server->mode_count = mode_count;
    server->active_mode = NULL;
    if (!leme_input_set_mode(server, active_name)) {
        leme_input_set_mode(server, "common");
    }
}

static struct leme_binding *
leme_input_find_binding(struct leme_server *server,
    uint32_t modifiers, const xkb_keysym_t *syms, int count)
{
    size_t binding_index;
    int symbol_index;

    if (server->active_mode == NULL) {
        return NULL;
    }
    modifiers &= WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL |
        WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO;
    for (binding_index = 0;
            binding_index < server->active_mode->binding_count;
            binding_index++) {
        struct leme_binding *binding =
            &server->active_mode->bindings[binding_index];
        if (binding->modifiers != modifiers) {
            continue;
        }
        for (symbol_index = 0; symbol_index < count; symbol_index++) {
            if (binding->keysym == syms[symbol_index]) {
                return binding;
            }
        }
    }
    return NULL;
}

static void
leme_input_handle_modifiers(struct wl_listener *listener, void *data)
{
    struct leme_keyboard *keyboard =
        wl_container_of(listener, keyboard, modifiers);

    (void)data;
    leme_session_notify_activity(keyboard->server);
    wlr_seat_set_keyboard(keyboard->server->seat, keyboard->keyboard);
    wlr_seat_keyboard_notify_modifiers(keyboard->server->seat,
        &keyboard->keyboard->modifiers);
}

static bool
leme_input_binding_reserved(const struct leme_binding *binding)
{
    return binding->command.type == LEME_COMMAND_SWITCH_VT ||
        binding->command.type == LEME_COMMAND_QUIT;
}

static void
leme_input_handle_key(struct wl_listener *listener, void *data)
{
    struct leme_keyboard *keyboard =
        wl_container_of(listener, keyboard, key);
    struct wlr_keyboard_key_event *event = data;
    struct leme_binding *binding = NULL;
    const xkb_keysym_t *syms;
    xkb_keycode_t keycode;
    xkb_layout_index_t layout;
    uint32_t modifiers;
    int count;
    int index;
    bool handled = false;
    bool inhibited;

    leme_session_notify_activity(keyboard->server);
    keycode = event->keycode + 8;
    layout = xkb_state_key_get_layout(
        keyboard->keyboard->xkb_state, keycode);
    count = layout == XKB_LAYOUT_INVALID ? 0 :
        xkb_keymap_key_get_syms_by_level(keyboard->keyboard->keymap,
            keycode, layout, 0, &syms);
    modifiers = wlr_keyboard_get_modifiers(keyboard->keyboard);
    inhibited = leme_input_protocols_shortcuts_inhibited(keyboard->server);
    if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        if (!inhibited && !leme_session_locked(keyboard->server) &&
                (keyboard->server->active_mode == NULL ||
                    keyboard->server->active_mode->escape_exits)) {
            for (index = 0; index < count; index++) {
                if (syms[index] == XKB_KEY_Escape) {
                    leme_input_set_mode(keyboard->server, "common");
                    break;
                }
            }
        }
        binding = leme_input_find_binding(
            keyboard->server, modifiers, syms, count);
        if (binding != NULL && inhibited &&
                !leme_input_binding_reserved(binding)) {
            binding = NULL;
        }
        if (binding != NULL && !leme_session_command_allowed(
                keyboard->server, &binding->command)) {
            binding = NULL;
        }
        if (binding != NULL) {
            handled = true;
            leme_input_protocols_cancel_constraint(keyboard->server);
            leme_command_execute(keyboard->server, &binding->command);
            if (event->keycode < LEME_ARRAY_LENGTH(keyboard->handled)) {
                keyboard->handled[event->keycode] = true;
            }
        }
    } else if (event->keycode < LEME_ARRAY_LENGTH(keyboard->handled) &&
            keyboard->handled[event->keycode]) {
        keyboard->handled[event->keycode] = false;
        handled = true;
    }
    if (!handled) {
        wlr_seat_set_keyboard(keyboard->server->seat, keyboard->keyboard);
        wlr_seat_keyboard_notify_key(keyboard->server->seat,
            event->time_msec, event->keycode, event->state);
    }
}

static void
leme_input_select_keyboard(struct leme_server *server)
{
    struct leme_keyboard *keyboard;

    if (wl_list_empty(&server->keyboards)) {
        wlr_seat_set_keyboard(server->seat, NULL);
        return;
    }
    keyboard = wl_container_of(server->keyboards.next, keyboard, link);
    wlr_seat_set_keyboard(server->seat, keyboard->keyboard);
}

static void
leme_input_handle_keyboard_destroy(struct wl_listener *listener, void *data)
{
    struct leme_keyboard *keyboard =
        wl_container_of(listener, keyboard, destroy);
    struct leme_server *server = keyboard->server;

    (void)data;
    wl_list_remove(&keyboard->key.link);
    wl_list_remove(&keyboard->modifiers.link);
    wl_list_remove(&keyboard->destroy.link);
    wl_list_remove(&keyboard->link);
    free(keyboard);
    leme_input_select_keyboard(server);
    leme_input_update_capabilities(server);
}

void
leme_input_keyboard_add(struct leme_server *server,
    struct wlr_input_device *device)
{
    struct leme_keyboard *keyboard = calloc(1, sizeof(*keyboard));
    struct xkb_keymap *keymap;

    if (keyboard == NULL) {
        return;
    }
    keyboard->server = server;
    keyboard->keyboard = wlr_keyboard_from_input_device(device);
    keymap = leme_input_compile_keymap(server->config);
    if (keymap == NULL ||
            !wlr_keyboard_set_keymap(keyboard->keyboard, keymap)) {
        wlr_log(WLR_ERROR, "%s", "leme: failed to configure keyboard");
        xkb_keymap_unref(keymap);
        free(keyboard);
        return;
    }
    xkb_keymap_unref(keymap);
    keyboard->key.notify = leme_input_handle_key;
    wl_signal_add(&keyboard->keyboard->events.key, &keyboard->key);
    keyboard->modifiers.notify = leme_input_handle_modifiers;
    wl_signal_add(&keyboard->keyboard->events.modifiers,
        &keyboard->modifiers);
    keyboard->destroy.notify = leme_input_handle_keyboard_destroy;
    wl_signal_add(&device->events.destroy, &keyboard->destroy);
    wl_list_insert(&server->keyboards, &keyboard->link);
    wlr_keyboard_notify_modifiers(keyboard->keyboard,
        keyboard->keyboard->modifiers.depressed,
        keyboard->keyboard->modifiers.latched,
        keyboard->keyboard->modifiers.locked,
        server->keyboard_layout);
    wlr_seat_set_keyboard(server->seat, keyboard->keyboard);
}


void
leme_input_keyboards_finish(struct leme_server *server)
{
    struct leme_keyboard *keyboard;
    struct leme_keyboard *temporary;

    wl_list_for_each_safe(keyboard, temporary, &server->keyboards, link) {
        wl_list_remove(&keyboard->key.link);
        wl_list_remove(&keyboard->modifiers.link);
        wl_list_remove(&keyboard->destroy.link);
        wl_list_remove(&keyboard->link);
        free(keyboard);
    }
}
