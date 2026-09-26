#include "config/public-internal.h"
#include "public/value-internal.h"
#include "workspace/public.h"

#include <inttypes.h>
#include <stdio.h>
#include <wlr/types/wlr_keyboard.h>

static const char *command_name(enum leme_command_type type) {
  switch (type) {
  case LEME_COMMAND_FOCUS_NEXT_TAG:
    return "focus_next_tag";
  case LEME_COMMAND_FOCUS_PREVIOUS_TAG:
    return "focus_previous_tag";
  case LEME_COMMAND_FOCUS_TAG:
    return "focus_tag";
  case LEME_COMMAND_FOCUS_DIRECTION:
    return "focus";
  case LEME_COMMAND_FOCUS_LAST_TAG:
    return "focus_last_tag";
  case LEME_COMMAND_FOCUS_PREVIOUS_VIEW:
    return "focus_previous_view";
  case LEME_COMMAND_MOVE_DIRECTION:
    return "move";
  case LEME_COMMAND_MOVE_VIEW_TO_TAG:
    return "move_view_to_tag";
  case LEME_COMMAND_FOCUS_OUTPUT:
    return "focus_output";
  case LEME_COMMAND_MOVE_VIEW_TO_OUTPUT:
    return "move_view_to_output";
  case LEME_COMMAND_SET_LAYOUT:
    return "set_layout";
  case LEME_COMMAND_SWITCH_LAYOUT:
    return "switch_layout";
  case LEME_COMMAND_REMOVE_EMPTY_TAG:
    return "remove_empty_tag";
  case LEME_COMMAND_TOGGLE_FLOATING:
    return "toggle_floating";
  case LEME_COMMAND_TOGGLE_STICKY:
    return "toggle_sticky";
  case LEME_COMMAND_TOGGLE_FULLSCREEN:
    return "toggle_fullscreen";
  case LEME_COMMAND_RESIZE:
    return "resize";
  case LEME_COMMAND_CLOSE_VIEW:
    return "close_view";
  case LEME_COMMAND_SPAWN:
    return "spawn";
  case LEME_COMMAND_RELOAD_CONFIG:
    return "reload_config";
  case LEME_COMMAND_SET_MODE:
    return "mode";
  case LEME_COMMAND_CYCLE_KEYBOARD_LAYOUT:
    return "cycle_keyboard_layout";
  case LEME_COMMAND_SWITCH_VT:
    return "switch_vt";
  case LEME_COMMAND_QUIT:
    return "quit";
  case LEME_COMMAND_SCRATCHPAD_SEND:
    return "scratchpad_send";
  case LEME_COMMAND_SCRATCHPAD_TOGGLE:
    return "scratchpad_toggle";
  case LEME_COMMAND_SCRATCHPAD_RETRIEVE:
    return "scratchpad_retrieve";
  }
  return NULL;
}

static const char *direction_name(enum leme_direction direction) {
  switch (direction) {
  case LEME_DIRECTION_LEFT:
    return "left";
  case LEME_DIRECTION_RIGHT:
    return "right";
  case LEME_DIRECTION_UP:
    return "up";
  case LEME_DIRECTION_DOWN:
    return "down";
  }
  return NULL;
}

static const char *edge_name(enum leme_resize_edge edge) {
  switch (edge) {
  case LEME_RESIZE_LEFT:
    return "left";
  case LEME_RESIZE_RIGHT:
    return "right";
  case LEME_RESIZE_UP:
    return "up";
  case LEME_RESIZE_DOWN:
    return "down";
  }
  return NULL;
}

static enum leme_public_status integer_text(struct leme_public_builder *b,
                                            int64_t integer,
                                            struct leme_public_value **out) {
  char buffer[32] = {0};
  const int written = snprintf(buffer, sizeof(buffer), "%" PRId64, integer);
  if (written < 0 || (size_t)written >= sizeof(buffer))
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  return leme_public_string(
      b, (struct leme_public_text){buffer, (size_t)written}, false, out);
}

static enum leme_public_status argument_text(struct leme_public_builder *b,
                                             const char *text,
                                             struct leme_public_value **out) {
  if (text == NULL || text[0] == '\0')
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  return leme_config_public_text(b, text, out);
}

static enum leme_public_status command_args(struct leme_public_builder *b,
                                            const struct leme_command *command,
                                            struct leme_public_value **out) {
  if (command->type == LEME_COMMAND_SPAWN)
    return leme_config_public_redacted(b, out);
  struct leme_public_value *arguments[2] = {NULL, NULL};
  size_t count = 0;
  enum leme_public_status status = LEME_PUBLIC_OK;
  switch (command->type) {
  case LEME_COMMAND_FOCUS_TAG:
  case LEME_COMMAND_REMOVE_EMPTY_TAG:
    if (command->tag_id == 0)
      return leme_public_fail(b, LEME_PUBLIC_INVALID);
    status = integer_text(b, command->tag_id, &arguments[count++]);
    break;
  case LEME_COMMAND_FOCUS_DIRECTION:
    status = argument_text(b, direction_name(command->direction),
                           &arguments[count++]);
    break;
  case LEME_COMMAND_MOVE_DIRECTION:
    if (command->amount <= 0)
      return leme_public_fail(b, LEME_PUBLIC_INVALID);
    status = argument_text(b, direction_name(command->direction),
                           &arguments[count++]);
    if (status == LEME_PUBLIC_OK)
      status = integer_text(b, command->amount, &arguments[count++]);
    break;
  case LEME_COMMAND_MOVE_VIEW_TO_TAG:
    if (command->has_direction) {
      const char *relative = command->direction == LEME_DIRECTION_RIGHT ? "next"
                             : command->direction == LEME_DIRECTION_LEFT
                                 ? "previous"
                                 : NULL;
      status = argument_text(b, relative, &arguments[count++]);
    } else {
      if (command->tag_id == 0)
        return leme_public_fail(b, LEME_PUBLIC_INVALID);
      status = integer_text(b, command->tag_id, &arguments[count++]);
    }
    if (status == LEME_PUBLIC_OK && command->follow)
      status = argument_text(b, "follow", &arguments[count++]);
    break;
  case LEME_COMMAND_FOCUS_OUTPUT:
  case LEME_COMMAND_MOVE_VIEW_TO_OUTPUT:
    status = argument_text(b,
                           command->has_direction
                               ? direction_name(command->direction)
                               : command->text,
                           &arguments[count++]);
    if (status == LEME_PUBLIC_OK &&
        command->type == LEME_COMMAND_MOVE_VIEW_TO_OUTPUT && command->follow) {
      status = argument_text(b, "follow", &arguments[count++]);
    }
    break;
  case LEME_COMMAND_SET_LAYOUT:
    status = argument_text(b, leme_layout_public_name(command->layout),
                           &arguments[count++]);
    break;
  case LEME_COMMAND_RESIZE:
    if (command->amount <= 0)
      return leme_public_fail(b, LEME_PUBLIC_INVALID);
    status = argument_text(b, edge_name(command->edge), &arguments[count++]);
    if (status == LEME_PUBLIC_OK)
      status = integer_text(b, command->amount, &arguments[count++]);
    break;
  case LEME_COMMAND_SET_MODE:
    status = argument_text(b, command->text, &arguments[count++]);
    break;
  case LEME_COMMAND_SWITCH_VT:
    if (command->vt == 0 || command->vt > 12)
      return leme_public_fail(b, LEME_PUBLIC_INVALID);
    status = integer_text(b, command->vt, &arguments[count++]);
    break;
  case LEME_COMMAND_SCRATCHPAD_TOGGLE:
    if (command->text != NULL)
      status = argument_text(b, command->text, &arguments[count++]);
    break;
  case LEME_COMMAND_FOCUS_NEXT_TAG:
  case LEME_COMMAND_FOCUS_PREVIOUS_TAG:
  case LEME_COMMAND_FOCUS_LAST_TAG:
  case LEME_COMMAND_FOCUS_PREVIOUS_VIEW:
  case LEME_COMMAND_SWITCH_LAYOUT:
  case LEME_COMMAND_TOGGLE_FLOATING:
  case LEME_COMMAND_TOGGLE_STICKY:
  case LEME_COMMAND_TOGGLE_FULLSCREEN:
  case LEME_COMMAND_CLOSE_VIEW:
  case LEME_COMMAND_RELOAD_CONFIG:
  case LEME_COMMAND_CYCLE_KEYBOARD_LAYOUT:
  case LEME_COMMAND_QUIT:
  case LEME_COMMAND_SCRATCHPAD_SEND:
  case LEME_COMMAND_SCRATCHPAD_RETRIEVE:
    break;
  case LEME_COMMAND_SPAWN:
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  }
  if (status != LEME_PUBLIC_OK)
    return status;
  struct leme_public_value *array = NULL;
  if (leme_public_array(b, count, &array) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  for (size_t i = 0; i < count; ++i) {
    if (leme_public_array_set(b, array, i, arguments[i]) != LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
  }
  *out = array;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status command_value(struct leme_public_builder *b,
                                             const struct leme_command *command,
                                             struct leme_public_value **out) {
  const char *name = command_name(command->type);
  if (name == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  struct leme_public_value *record = NULL, *args = NULL;
  if (leme_public_object(b, 2, &record) != LEME_PUBLIC_OK ||
      command_args(b, command, &args) != LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("name"), name) !=
          LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("args"), args) !=
          LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  *out = record;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status modifiers_value(struct leme_public_builder *b,
                                               uint32_t modifiers,
                                               struct leme_public_value **out) {
  static const struct {
    uint32_t bit;
    const char *name;
  } names[] = {{WLR_MODIFIER_SHIFT, "SHIFT"},
               {WLR_MODIFIER_CTRL, "CTRL"},
               {WLR_MODIFIER_ALT, "ALT"},
               {WLR_MODIFIER_LOGO, "SUPER"}};
  const uint32_t known = WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL |
                         WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO;
  if ((modifiers & ~known) != 0)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  size_t count = 0;
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
    if ((modifiers & names[i].bit) != 0)
      ++count;
  struct leme_public_value *array = NULL;
  if (leme_public_array(b, count, &array) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  size_t index = 0;
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
    if ((modifiers & names[i].bit) == 0)
      continue;
    struct leme_public_value *name = NULL;
    if (leme_config_public_text(b, names[i].name, &name) != LEME_PUBLIC_OK ||
        leme_public_array_set(b, array, index++, name) != LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
  }
  *out = array;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status binding_value(struct leme_public_builder *b,
                                             const struct leme_binding *binding,
                                             struct leme_public_value **out) {
  char keysym[128] = {0};
  const int written =
      xkb_keysym_get_name(binding->keysym, keysym, sizeof(keysym));
  if (written <= 0 || (size_t)written >= sizeof(keysym))
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  struct leme_public_value *record = NULL, *modifiers = NULL, *command = NULL;
  if (leme_public_object(b, 3, &record) != LEME_PUBLIC_OK ||
      modifiers_value(b, binding->modifiers, &modifiers) != LEME_PUBLIC_OK ||
      command_value(b, &binding->command, &command) != LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("modifiers"),
                             modifiers) != LEME_PUBLIC_OK ||
      leme_public_put_text(b, record, LEME_PUBLIC_TEXT("keysym"),
                           (struct leme_public_text){keysym, (size_t)written},
                           false) != LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("command"), command) !=
          LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  *out = record;
  return LEME_PUBLIC_OK;
}

enum leme_public_status
leme_config_public_modes(struct leme_public_builder *b,
                         const struct leme_config *config,
                         struct leme_public_value **out) {
  if (out == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  *out = NULL;
  if (config == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  if (config->mode_count != 0 && config->modes == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  struct leme_public_value *array = NULL;
  if (leme_public_array(b, config->mode_count, &array) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  for (size_t i = 0; i < config->mode_count; ++i) {
    const struct leme_mode *mode = &config->modes[i];
    if (mode->binding_count != 0 && mode->bindings == NULL)
      return leme_public_fail(b, LEME_PUBLIC_INVALID);
    struct leme_public_value *record = NULL, *bindings = NULL;
    if (leme_public_object(b, 3, &record) != LEME_PUBLIC_OK ||
        leme_public_array(b, mode->binding_count, &bindings) !=
            LEME_PUBLIC_OK ||
        leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("name"), mode->name) !=
            LEME_PUBLIC_OK ||
        leme_public_put_bool(b, record, LEME_PUBLIC_TEXT("escape_exits"),
                             mode->escape_exits) != LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
    for (size_t n = 0; n < mode->binding_count; ++n) {
      struct leme_public_value *binding = NULL;
      if (binding_value(b, &mode->bindings[n], &binding) != LEME_PUBLIC_OK ||
          leme_public_array_set(b, bindings, n, binding) != LEME_PUBLIC_OK)
        return leme_public_builder_status(b);
    }
    if (leme_public_object_set(b, record, LEME_PUBLIC_TEXT("bindings"),
                               bindings) != LEME_PUBLIC_OK ||
        leme_public_array_set(b, array, i, record) != LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
  }
  *out = array;
  return LEME_PUBLIC_OK;
}
