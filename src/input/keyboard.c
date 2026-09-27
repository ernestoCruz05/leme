#include "input/input.h"
#include "ipc/ipc.h"
#include "input/internal.h"
#include "input/public.h"
#include "public/server.h"

#include "config/config.h"
#include "config/live.h"
#include "core/command.h"
#include "core/server.h"
#include "protocols/input.h"
#include "protocols/session.h"

#include <libinput.h>
#include <wlr/backend/libinput.h>
#include <stdint.h>
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
  struct leme_public_id public_id;
  bool public_keymap_configured;
  bool is_virtual;
  bool handled[256];
  struct wl_listener key;
  struct wl_listener keymap;
  struct wl_listener modifiers;
  struct wl_listener destroy;
  struct wl_list link;
};

static char *leme_input_join_layouts(const struct leme_config *config,
                                     bool variants) {
  char *result;
  char *cursor;
  size_t length = 1;
  size_t index;

  for (index = 0; index < config->keyboard_layout_count; index++) {
    const char *value = variants ? config->keyboard_layouts[index].variant
                                 : config->keyboard_layouts[index].name;
    const size_t value_length = value == NULL ? 0 : strlen(value);

    if (index > 0) {
      if (length == SIZE_MAX) {
        return NULL;
      }
      length++;
    }
    if (value_length > SIZE_MAX - length) {
      return NULL;
    }
    length += value_length;
  }
  result = malloc(length);
  if (result == NULL) {
    return NULL;
  }
  cursor = result;
  for (index = 0; index < config->keyboard_layout_count; index++) {
    const char *value = variants ? config->keyboard_layouts[index].variant
                                 : config->keyboard_layouts[index].name;
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

struct xkb_keymap *leme_input_compile_keymap(const struct leme_config *config) {
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
    have_variant =
        have_variant || config->keyboard_layouts[index].variant != NULL;
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
  keymap =
      xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
  xkb_context_unref(context);
  free(layouts);
  free(variants);
  if (keymap != NULL &&
      xkb_keymap_num_layouts(keymap) != config->keyboard_layout_count) {
    xkb_keymap_unref(keymap);
    keymap = NULL;
  }
  return keymap;
}

bool leme_input_apply_keymap(struct leme_server *server,
                             struct xkb_keymap *keymap) {
  struct leme_keyboard *keyboard;

  if (server == NULL || keymap == NULL) {
    return false;
  }
  server->keyboard_layout = 0;
  leme_public_server_invalidate(server);
  if (server->cursor == NULL) {
    return true;
  }
  wl_list_for_each(keyboard, &server->keyboards, link) {
    if (keyboard->is_virtual) {
      continue;
    }
    if (!wlr_keyboard_set_keymap(keyboard->keyboard, keymap)) {
      wlr_log(WLR_ERROR, "%s", "leme: failed to apply keyboard keymap");
      return false;
    }
  }
  wl_list_for_each(keyboard, &server->keyboards, link) {
    struct wlr_keyboard_modifiers *modifiers = &keyboard->keyboard->modifiers;

    if (keyboard->is_virtual) {
      continue;
    }
    wlr_keyboard_notify_modifiers(keyboard->keyboard, modifiers->depressed,
                                  modifiers->latched, modifiers->locked,
                                  server->keyboard_layout);
  }
  return true;
}

static void leme_input_apply_keyboard_layout_group(struct leme_server *server,
                                                   size_t group) {
  struct leme_keyboard *keyboard;
  server->keyboard_layout = (xkb_layout_index_t)group;
  wl_list_for_each(keyboard, &server->keyboards, link) {
    struct wlr_keyboard_modifiers *modifiers = &keyboard->keyboard->modifiers;
    if (keyboard->is_virtual) {
      continue;
    }
    wlr_keyboard_notify_modifiers(keyboard->keyboard, modifiers->depressed,
                                  modifiers->latched, modifiers->locked,
                                  server->keyboard_layout);
  }
  if (server->config != NULL &&
      server->keyboard_layout < server->config->keyboard_layout_count) {
    wlr_log(WLR_INFO, "leme: keyboard layout=%s",
            server->config->keyboard_layouts[server->keyboard_layout].name);
  }
  leme_public_server_invalidate(server);
  leme_ipc_invalidate(server);
}

static bool format_layout_label(const struct leme_keyboard_layout *layout,
                                char *buffer, size_t capacity) {
  if (layout == NULL || layout->name == NULL) {
    return false;
  }
  if (layout->variant == NULL || layout->variant[0] == '\0') {
    int w = snprintf(buffer, capacity, "%s", layout->name);
    return w > 0 && (size_t)w < capacity;
  }
  int w = snprintf(buffer, capacity, "%s(%s)", layout->name, layout->variant);
  return w > 0 && (size_t)w < capacity;
}

bool leme_input_find_keyboard_layout(const struct leme_server *server,
                                     const char *label, size_t *out_index,
                                     bool *out_ambiguous) {
  if (out_index != NULL) {
    *out_index = 0;
  }
  if (out_ambiguous != NULL) {
    *out_ambiguous = false;
  }
  if (server == NULL || server->config == NULL || label == NULL ||
      server->config->keyboard_layout_count == 0) {
    return false;
  }
  size_t match_count = 0;
  size_t found_idx = 0;
  char buf[256];
  for (size_t i = 0; i < server->config->keyboard_layout_count; i++) {
    if (format_layout_label(&server->config->keyboard_layouts[i], buf,
                            sizeof(buf))) {
      if (strcmp(buf, label) == 0) {
        match_count++;
        found_idx = i;
      }
    }
  }
  if (match_count == 0) {
    return false;
  }
  if (match_count > 1) {
    if (out_ambiguous != NULL) {
      *out_ambiguous = true;
    }
    return false;
  }
  if (out_index != NULL) {
    *out_index = found_idx;
  }
  return true;
}

bool leme_input_select_keyboard_layout(struct leme_server *server,
                                       const char *label) {
  size_t index = 0;
  bool ambiguous = false;
  if (!leme_input_find_keyboard_layout(server, label, &index, &ambiguous)) {
    return false;
  }
  leme_input_apply_keyboard_layout_group(server, index);
  return true;
}

bool leme_input_cycle_keyboard_layout(struct leme_server *server) {
  if (server == NULL || server->config == NULL ||
      server->config->keyboard_layout_count == 0) {
    wlr_log(WLR_ERROR, "%s", "leme: no keyboard layout is configured");
    return false;
  }
  size_t count = server->config->keyboard_layout_count;
  size_t next_idx = (size_t)((server->keyboard_layout + 1) % count);
  leme_input_apply_keyboard_layout_group(server, next_idx);
  return true;
}

bool leme_input_set_mode(struct leme_server *server, const char *name) {
  size_t index;

  if (name == NULL) {
    return false;
  }
  for (index = 0; index < server->mode_count; index++) {
    if (strcmp(server->modes[index].name, name) == 0) {
      server->active_mode = &server->modes[index];
      leme_public_server_invalidate(server);
      leme_ipc_invalidate(server);
      return true;
    }
  }
  return false;
}

void leme_input_replace_modes(struct leme_server *server,
                              struct leme_mode *modes, size_t mode_count) {
  const char *active_name =
      server->active_mode == NULL ? "common" : server->active_mode->name;

  server->modes = modes;
  server->mode_count = mode_count;
  server->active_mode = NULL;
  if (!leme_input_set_mode(server, active_name)) {
    leme_input_set_mode(server, "common");
  }
}

static struct leme_binding *leme_input_find_binding(struct leme_server *server,
                                                    uint32_t modifiers,
                                                    const xkb_keysym_t *syms,
                                                    int count) {
  size_t binding_index;
  int symbol_index;

  if (server->active_mode == NULL) {
    return NULL;
  }
  modifiers &= WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT |
               WLR_MODIFIER_LOGO;
  for (binding_index = 0; binding_index < server->active_mode->binding_count;
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

static void leme_input_handle_keymap(struct wl_listener *listener, void *data) {
  struct leme_keyboard *keyboard = wl_container_of(listener, keyboard, keymap);
  (void)data;
  keyboard->public_keymap_configured = false;
  leme_public_server_invalidate(keyboard->server);
}

static void leme_input_handle_modifiers(struct wl_listener *listener,
                                        void *data) {
  struct leme_keyboard *keyboard =
      wl_container_of(listener, keyboard, modifiers);

  (void)data;
  leme_session_notify_activity(keyboard->server);
  wlr_seat_set_keyboard(keyboard->server->seat, keyboard->keyboard);
  wlr_seat_keyboard_notify_modifiers(keyboard->server->seat,
                                     &keyboard->keyboard->modifiers);
  leme_public_server_invalidate(keyboard->server);
}

static bool leme_input_binding_reserved(const struct leme_binding *binding) {
  return binding->command.type == LEME_COMMAND_SWITCH_VT ||
         binding->command.type == LEME_COMMAND_QUIT;
}

static void leme_input_handle_key(struct wl_listener *listener, void *data) {
  struct leme_keyboard *keyboard = wl_container_of(listener, keyboard, key);
  struct wlr_keyboard_key_event *event = data;
  struct leme_binding *binding = NULL;
  const xkb_keysym_t *syms = NULL;
  xkb_keycode_t keycode;
  xkb_layout_index_t layout;
  uint32_t modifiers;
  int count;
  int index;
  bool handled = false;
  bool inhibited;

  leme_session_notify_activity(keyboard->server);
  keycode = event->keycode + 8;
  layout = xkb_state_key_get_layout(keyboard->keyboard->xkb_state, keycode);
  count = layout == XKB_LAYOUT_INVALID
              ? 0
              : xkb_keymap_key_get_syms_by_level(keyboard->keyboard->keymap,
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
    binding = leme_input_find_binding(keyboard->server, modifiers, syms, count);
    if (binding != NULL && inhibited && !leme_input_binding_reserved(binding)) {
      binding = NULL;
    }
    if (binding != NULL &&
        !leme_session_command_allowed(keyboard->server, &binding->command)) {
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
    wlr_seat_keyboard_notify_key(keyboard->server->seat, event->time_msec,
                                 event->keycode, event->state);
  }
}

static void leme_input_select_keyboard(struct leme_server *server) {
  struct leme_keyboard *keyboard;

  if (wl_list_empty(&server->keyboards)) {
    wlr_seat_set_keyboard(server->seat, NULL);
    return;
  }
  wl_list_for_each(keyboard, &server->keyboards, link) {
    if (!keyboard->is_virtual) {
      wlr_seat_set_keyboard(server->seat, keyboard->keyboard);
      return;
    }
  }
  keyboard = wl_container_of(server->keyboards.next, keyboard, link);
  wlr_seat_set_keyboard(server->seat, keyboard->keyboard);
}

static void leme_input_handle_keyboard_destroy(struct wl_listener *listener,
                                               void *data) {
  struct leme_keyboard *keyboard = wl_container_of(listener, keyboard, destroy);
  struct leme_server *server = keyboard->server;

  (void)data;
  wl_list_remove(&keyboard->keymap.link);
  wl_list_remove(&keyboard->key.link);
  wl_list_remove(&keyboard->modifiers.link);
  wl_list_remove(&keyboard->destroy.link);
  wl_list_remove(&keyboard->link);
  if (server->config_store != NULL) {
    struct leme_control_target target = {
        .kind = LEME_PUBLIC_INPUT,
        .id = keyboard->public_id,
    };
    leme_config_store_drop_target(server->config_store, &target);
  }
  free(keyboard);
  leme_input_select_keyboard(server);
  leme_input_update_capabilities(server);
  leme_public_server_invalidate(server);
}

static void leme_input_keyboard_attach(struct leme_server *server,
                                       struct wlr_keyboard *wlr_keyboard,
                                       bool is_virtual) {
  struct leme_keyboard *keyboard = calloc(1, sizeof(*keyboard));

  if (keyboard == NULL) {
    return;
  }
  keyboard->server = server;
  keyboard->keyboard = wlr_keyboard;
  keyboard->is_virtual = is_virtual;
  if (!is_virtual) {
    struct xkb_keymap *keymap = leme_input_compile_keymap(server->config);

    if (keymap == NULL || !wlr_keyboard_set_keymap(wlr_keyboard, keymap)) {
      wlr_log(WLR_ERROR, "%s", "leme: failed to configure keyboard");
      xkb_keymap_unref(keymap);
      free(keyboard);
      return;
    }
    xkb_keymap_unref(keymap);
    keyboard->public_keymap_configured = true;
  }
  if (leme_public_model_available(server->public_model) &&
      leme_public_model_issue_id(server->public_model, &keyboard->public_id) !=
          LEME_PUBLIC_OK) {
    leme_public_model_disable(server->public_model);
  }
  keyboard->key.notify = leme_input_handle_key;
  wl_signal_add(&keyboard->keyboard->events.key, &keyboard->key);
  keyboard->keymap.notify = leme_input_handle_keymap;
  wl_signal_add(&keyboard->keyboard->events.keymap, &keyboard->keymap);
  keyboard->modifiers.notify = leme_input_handle_modifiers;
  wl_signal_add(&keyboard->keyboard->events.modifiers, &keyboard->modifiers);
  keyboard->destroy.notify = leme_input_handle_keyboard_destroy;
  wl_signal_add(&wlr_keyboard->base.events.destroy, &keyboard->destroy);
  wl_list_insert(&server->keyboards, &keyboard->link);
  if (!is_virtual) {
    wlr_keyboard_notify_modifiers(
        keyboard->keyboard, keyboard->keyboard->modifiers.depressed,
        keyboard->keyboard->modifiers.latched,
        keyboard->keyboard->modifiers.locked, server->keyboard_layout);
    wlr_seat_set_keyboard(server->seat, keyboard->keyboard);
  }
  leme_public_server_invalidate(server);
}

void leme_input_keyboard_add(struct leme_server *server,
                             struct wlr_input_device *device) {
  leme_input_keyboard_attach(server, wlr_keyboard_from_input_device(device),
                             false);
}

void leme_input_virtual_keyboard_add(struct leme_server *server,
                                     struct wlr_keyboard *keyboard) {
  leme_input_keyboard_attach(server, keyboard, true);
}

void leme_input_public_keymap_committed(struct leme_server *server) {
  if (server == NULL || server->keyboards.next == NULL)
    return;
  struct leme_keyboard *keyboard = NULL;
  wl_list_for_each(keyboard, &server->keyboards, link)
      keyboard->public_keymap_configured = !keyboard->is_virtual;
  leme_public_server_invalidate(server);
}

enum leme_public_status
leme_input_public_keyboards(const struct leme_server *server,
                            leme_public_input_visitor visit, void *context) {
  if (server == NULL || visit == NULL || server->keyboards.next == NULL)
    return LEME_PUBLIC_INVALID;
  if (leme_session_locked(server))
    return LEME_PUBLIC_LOCKED;
  if (!leme_public_model_available(server->public_model))
    return LEME_PUBLIC_UNAVAILABLE;
  const struct leme_keyboard *keyboard = NULL;
  wl_list_for_each(keyboard, &server->keyboards, link) {
    struct wlr_input_device *base = &keyboard->keyboard->base;
    struct leme_public_input_info info = {
        .id = keyboard->public_id,
        .name = base->name,
        .seat = server->seat == NULL ? NULL : server->seat->name,
        .keyboard = true,
        .libinput = wlr_input_device_is_libinput(base)};
    if (info.libinput) {
      struct libinput_device *device = wlr_libinput_get_device_handle(base);
      if (device != NULL) {
        info.has_ids = true;
        info.vendor = libinput_device_get_id_vendor(device);
        info.product = libinput_device_get_id_product(device);
      }
    }
    if (keyboard->public_keymap_configured &&
        keyboard->keyboard->xkb_state != NULL &&
        keyboard->keyboard->keymap != NULL && server->config != NULL &&
        server->config->keyboard_layouts != NULL) {
      const xkb_layout_index_t group = xkb_state_serialize_layout(
          keyboard->keyboard->xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);
      if (group != XKB_LAYOUT_INVALID &&
          group < xkb_keymap_num_layouts(keyboard->keyboard->keymap) &&
          group < server->config->keyboard_layout_count) {
        info.keyboard_active = server->config->keyboard_layouts[group].name;
        info.keyboard_variant = server->config->keyboard_layouts[group].variant;
      }
    }
    const enum leme_public_status status = visit(context, &info);
    if (status != LEME_PUBLIC_OK)
      return status;
  }
  return LEME_PUBLIC_OK;
}

void leme_input_keyboards_finish(struct leme_server *server) {
  leme_public_server_invalidate(server);
  struct leme_keyboard *keyboard;
  struct leme_keyboard *temporary;

  wl_list_for_each_safe(keyboard, temporary, &server->keyboards, link) {
    wl_list_remove(&keyboard->keymap.link);
    wl_list_remove(&keyboard->key.link);
    wl_list_remove(&keyboard->modifiers.link);
    wl_list_remove(&keyboard->destroy.link);
    wl_list_remove(&keyboard->link);
    free(keyboard);
  }
}

bool leme_input_keyboard_resolve_id(const struct leme_server *server,
                                    struct leme_public_id id,
                                    struct leme_input_target_info *info) {
  if (server == NULL || info == NULL || server->keyboards.next == NULL) {
    return false;
  }
  const struct leme_keyboard *keyboard = NULL;
  wl_list_for_each(keyboard, &server->keyboards, link) {
    if (keyboard->public_id.serial == id.serial) {
      info->exists = true;
      info->is_keyboard = true;
      info->is_libinput =
          wlr_input_device_is_libinput(&keyboard->keyboard->base);
      return true;
    }
  }
  return false;
}
