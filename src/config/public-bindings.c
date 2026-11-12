#include "config/public-internal.h"
#include "public/value-internal.h"
#include "workspace/public.h"

#include <inttypes.h>
#include <stdio.h>
#include <wlr/types/wlr_keyboard.h>

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

static enum leme_public_status command_args(struct leme_public_builder *b,
                                            const struct leme_command *command,
                                            struct leme_public_value **out) {
  if (command->type == LEME_COMMAND_SPAWN)
    return leme_config_public_redacted(b, out);
  struct leme_command_word words[LEME_COMMAND_WORDS_MAX] = {0};
  size_t count = 0;
  if (!leme_command_words(command, words, &count))
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  struct leme_public_value *array = NULL;
  if (leme_public_array(b, count, &array) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  for (size_t i = 0; i < count; ++i) {
    struct leme_public_value *argument = NULL;
    if ((words[i].is_number
             ? integer_text(b, words[i].number, &argument)
             : leme_config_public_text(b, words[i].text, &argument)) !=
            LEME_PUBLIC_OK ||
        leme_public_array_set(b, array, i, argument) != LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
  }
  *out = array;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status command_value(struct leme_public_builder *b,
                                             const struct leme_command *command,
                                             struct leme_public_value **out) {
  const char *name = leme_command_spec(command->type)->name;
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
  static const char *const kinds[] = {
      [LEME_BINDING_SWIPE] = "swipe",
      [LEME_BINDING_PINCH] = "pinch",
      [LEME_BINDING_HOLD] = "hold",
      [LEME_BINDING_EDGE] = "edge",
  };
  static const char *const edges[] = {
      [LEME_EDGE_LEFT] = "left",
      [LEME_EDGE_RIGHT] = "right",
      [LEME_EDGE_TOP] = "top",
      [LEME_EDGE_BOTTOM] = "bottom",
  };
  static const char *const directions[] = {
      [LEME_GESTURE_UP] = "up",     [LEME_GESTURE_DOWN] = "down",
      [LEME_GESTURE_LEFT] = "left", [LEME_GESTURE_RIGHT] = "right",
      [LEME_GESTURE_IN] = "in",     [LEME_GESTURE_OUT] = "out",
  };
  const bool key = binding->trigger == LEME_BINDING_KEY;
  char keysym[128] = {0};
  int written = 0;
  if (key) {
    written = xkb_keysym_get_name(binding->keysym, keysym, sizeof(keysym));
    if (written <= 0 || (size_t)written >= sizeof(keysym))
      return leme_public_fail(b, LEME_PUBLIC_INVALID);
  }
  const bool lever = binding->trigger == LEME_BINDING_SWITCH;
  struct leme_public_value *record = NULL, *modifiers = NULL, *command = NULL,
                           *gesture = NULL, *toggle = NULL;
  if (!lever) {
    if (leme_public_null(b, &toggle) != LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
  } else if (leme_public_object(b, 2, &toggle) != LEME_PUBLIC_OK ||
             leme_public_put_cstr(b, toggle, LEME_PUBLIC_TEXT("kind"),
                                  binding->switch_kind == LEME_SWITCH_LID
                                      ? "lid"
                                      : "tablet") != LEME_PUBLIC_OK ||
             leme_public_put_cstr(b, toggle, LEME_PUBLIC_TEXT("state"),
                                  binding->switch_on ? "on" : "off") !=
                 LEME_PUBLIC_OK) {
    return leme_public_builder_status(b);
  }
  if (key || lever) {
    if (leme_public_null(b, &gesture) != LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
  } else if (leme_public_object(b, 4, &gesture) != LEME_PUBLIC_OK ||
             leme_public_put_cstr(b, gesture, LEME_PUBLIC_TEXT("kind"),
                                  kinds[binding->trigger]) != LEME_PUBLIC_OK ||
             leme_public_put_int(b, gesture, LEME_PUBLIC_TEXT("fingers"),
                                 binding->fingers) != LEME_PUBLIC_OK ||
             leme_public_put_cstr(b, gesture, LEME_PUBLIC_TEXT("direction"),
                                  directions[binding->direction]) !=
                 LEME_PUBLIC_OK ||
             leme_public_put_cstr(b, gesture, LEME_PUBLIC_TEXT("edge"),
                                  edges[binding->edge]) != LEME_PUBLIC_OK) {
    return leme_public_builder_status(b);
  }
  if (leme_public_object(b, 6, &record) != LEME_PUBLIC_OK ||
      modifiers_value(b, binding->modifiers, &modifiers) != LEME_PUBLIC_OK ||
      command_value(b, &binding->command, &command) != LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("modifiers"),
                             modifiers) != LEME_PUBLIC_OK ||
      (key ? leme_public_put_text(
                 b, record, LEME_PUBLIC_TEXT("keysym"),
                 (struct leme_public_text){keysym, (size_t)written}, false)
           : leme_public_put_null(b, record, LEME_PUBLIC_TEXT("keysym"))) !=
          LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("gesture"), gesture) !=
          LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("switch"), toggle) !=
          LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("command"), command) !=
          LEME_PUBLIC_OK ||
      leme_public_put_bool(b, record, LEME_PUBLIC_TEXT("repeat"),
                           binding->repeat) != LEME_PUBLIC_OK)
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
