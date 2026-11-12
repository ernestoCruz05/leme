#include "config/parse-internal.h"

#include "config/config.h"
#include "config/internal.h"
#include "config/option.h"
#include "config/scfg.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static enum leme_option_result
leme_config_parse_swipe_fingers(const struct leme_option_parse *parse) {
  struct leme_workspace_switch_gesture_settings *settings = parse->target;
  int value = 0;

  if (!leme_config_parse_nonnegative(parse->entry->params[0], &value)) {
    return leme_option_reject(parse, 0, NULL,
                              "`fingers` expects a nonnegative integer");
  }
  if (value == 2) {
    return leme_option_reject(parse, 0, NULL,
                              "libinput does not deliver two-finger swipes");
  }
  if (value == 1) {
    return leme_option_reject(parse, 0, NULL,
                              "fingers must be 0 or at least 3");
  }
  settings->fingers = (uint32_t)value;
  return LEME_OPTION_ACCEPTED;
}

_Static_assert(sizeof(enum leme_workspace_gesture_mode) == sizeof(unsigned),
               "choice options are stored as unsigned");

static const struct leme_option leme_workspace_switch_options[] = {
    {.name = "fingers",
     .kind = LEME_OPTION_CUSTOM,
     .parse = leme_config_parse_swipe_fingers},
    {.name = "distance",
     .kind = LEME_OPTION_DECIMAL,
     .offset =
         offsetof(struct leme_workspace_switch_gesture_settings, distance),
     .min = 0.0,
     .max = HUGE_VAL,
     .flags = LEME_OPTION_MIN_OPEN,
     .effect = LEME_OPTION_EFFECT_SWIPE},
    {.name = "threshold",
     .kind = LEME_OPTION_DECIMAL,
     .offset =
         offsetof(struct leme_workspace_switch_gesture_settings, threshold),
     .min = 0.0,
     .max = 1.0,
     .flags = LEME_OPTION_MIN_OPEN | LEME_OPTION_MAX_OPEN,
     .effect = LEME_OPTION_EFFECT_SWIPE},
    {.name = "deceleration",
     .kind = LEME_OPTION_DECIMAL,
     .offset =
         offsetof(struct leme_workspace_switch_gesture_settings, deceleration),
     .min = 0.0,
     .max = 1.0,
     .flags = LEME_OPTION_MIN_OPEN | LEME_OPTION_MAX_OPEN,
     .effect = LEME_OPTION_EFFECT_SWIPE},
    {.name = "velocity_window",
     .kind = LEME_OPTION_U32,
     .offset = offsetof(struct leme_workspace_switch_gesture_settings,
                        velocity_window_ms),
     .min = 1,
     .max = INT_MAX,
     .effect = LEME_OPTION_EFFECT_SWIPE},
    {.name = "mode",
     .kind = LEME_OPTION_CHOICE,
     .offset = offsetof(struct leme_workspace_switch_gesture_settings, mode),
     LEME_OPTION_CHOICES(leme_choice_gesture_mode),
     .effect = LEME_OPTION_EFFECT_SWIPE},
    {.name = "occupied",
     .kind = LEME_OPTION_BOOL,
     .offset =
         offsetof(struct leme_workspace_switch_gesture_settings, occupied),
     .effect = LEME_OPTION_EFFECT_SWIPE},
};

const struct leme_option_block leme_workspace_switch_block = {
    .name = "workspace_switch",
    .options = leme_workspace_switch_options,
    .count = LEME_OPTION_COUNT(leme_workspace_switch_options),
    .flags = LEME_OPTION_BLOCK_FIRST_WINS,
};

static enum leme_option_result
leme_config_parse_workspace_switch(const struct leme_option_parse *parse) {
  struct leme_gesture_config *gestures = parse->target;

  if (parse->entry->params_len != 0 &&
      !leme_config_reject(parse->config, parse->entry, 0,
                          "workspace_switch block takes no parameters")) {
    return LEME_OPTION_FATAL;
  }
  return leme_option_parse_block(parse->config, parse->entry,
                                 &leme_workspace_switch_block,
                                 &gestures->workspace_switch, NULL,
                                 parse->data) == LEME_OPTION_FATAL
             ? LEME_OPTION_FATAL
             : LEME_OPTION_ACCEPTED;
}

static const struct leme_option leme_edges_options[] = {
    {.name = "size",
     .kind = LEME_OPTION_DECIMAL,
     .offset = offsetof(struct leme_edge_gesture_settings, size),
     .min = LEME_EDGE_SIZE_MIN,
     .max = LEME_EDGE_SIZE_MAX,
     .unit = "mm",
     .effect = LEME_OPTION_EFFECT_STORE},
    {.name = "step",
     .kind = LEME_OPTION_DECIMAL,
     .offset = offsetof(struct leme_edge_gesture_settings, step),
     .min = LEME_EDGE_STEP_MIN,
     .max = LEME_EDGE_STEP_MAX,
     .unit = "mm",
     .effect = LEME_OPTION_EFFECT_STORE},
};

const struct leme_option_block leme_edges_block = {
    .name = "edges",
    .options = leme_edges_options,
    .count = LEME_OPTION_COUNT(leme_edges_options),
    .flags = LEME_OPTION_BLOCK_FIRST_WINS,
};

static enum leme_option_result
leme_config_parse_edges(const struct leme_option_parse *parse) {
  struct leme_gesture_config *gestures = parse->target;

  if (parse->entry->params_len != 0 &&
      !leme_config_reject(parse->config, parse->entry, 0,
                          "edges block takes no parameters")) {
    return LEME_OPTION_FATAL;
  }
  return leme_option_parse_block(parse->config, parse->entry, &leme_edges_block,
                                 &gestures->edges, NULL,
                                 parse->data) == LEME_OPTION_FATAL
             ? LEME_OPTION_FATAL
             : LEME_OPTION_ACCEPTED;
}

static const struct leme_option leme_gestures_options[] = {
    {.name = "workspace_switch",
     .kind = LEME_OPTION_CUSTOM,
     .flags = LEME_OPTION_MULTI,
     .parse = leme_config_parse_workspace_switch},
    {.name = "edges",
     .kind = LEME_OPTION_CUSTOM,
     .flags = LEME_OPTION_MULTI,
     .parse = leme_config_parse_edges},
};

static const struct leme_option_block leme_gestures_block = {
    .name = "gestures",
    .options = leme_gestures_options,
    .count = LEME_OPTION_COUNT(leme_gestures_options),
    .flags = LEME_OPTION_BLOCK_FIRST_WINS,
};

bool leme_config_parse_gestures(struct leme_config *config,
                                const struct leme_scfg_directive *directive,
                                const char *path, char **error) {
  if (directive->params_len != 0) {
    leme_config_set_error(error, "%s:%d: gestures block takes no parameters",
                          path, directive->lineno);
    return false;
  }
  return leme_config_parse_options(config, directive, &leme_gestures_block,
                                   &config->gestures, NULL, NULL, path, error);
}

static enum leme_option_result
leme_config_parse_focus_follows_mouse(const struct leme_option_parse *parse) {
  if (parse->target != &parse->config->pointer_defaults) {
    return leme_option_reject(parse, -1, NULL,
                              "focus_follows_mouse is only valid in the "
                              "unnamed pointer block");
  }
  if (!leme_config_parse_boolean(parse->entry->params[0],
                                 &parse->config->focus_follows_mouse)) {
    return leme_option_reject(
        parse, 0, NULL, "`focus_follows_mouse` expects `true` or `false`");
  }
  return LEME_OPTION_ACCEPTED;
}

static enum leme_option_result
leme_config_parse_drag_fingers(const struct leme_option_parse *parse) {
  struct leme_pointer_settings *settings = parse->target;
  int fingers = 0;

  if (!leme_config_parse_nonnegative(parse->entry->params[0], &fingers) ||
      (fingers != 0 && fingers != 3 && fingers != 4)) {
    return leme_option_reject(parse, 0, NULL,
                              "`drag_fingers` expects `0`, `3` or `4`");
  }
  settings->drag_fingers = (uint32_t)fingers;
  return LEME_OPTION_ACCEPTED;
}

_Static_assert(sizeof(enum leme_pointer_accel_profile) == sizeof(unsigned) &&
                   sizeof(enum leme_pointer_click_method) == sizeof(unsigned) &&
                   sizeof(enum leme_pointer_scroll_method) ==
                       sizeof(unsigned) &&
                   sizeof(enum leme_pointer_button) == sizeof(unsigned) &&
                   sizeof(enum leme_pointer_tap_button_map) ==
                       sizeof(unsigned) &&
                   sizeof(enum leme_pointer_drag_lock) == sizeof(unsigned),
               "choice options are stored as unsigned");

static const struct leme_option leme_pointer_options[] = {
    {.name = "accel_profile",
     .kind = LEME_OPTION_CHOICE,
     .offset = offsetof(struct leme_pointer_settings, profile),
     LEME_OPTION_CHOICES(leme_choice_accel_profile),
     .field = LEME_POINTER_PROFILE,
     .effect = LEME_OPTION_EFFECT_POINTERS},
    {.name = "accel_speed",
     .kind = LEME_OPTION_DECIMAL,
     .offset = offsetof(struct leme_pointer_settings, speed),
     .min = -1.0,
     .max = 1.0,
     .field = LEME_POINTER_SPEED,
     .effect = LEME_OPTION_EFFECT_POINTERS},
    {.name = "natural_scroll",
     .kind = LEME_OPTION_BOOL,
     .offset = offsetof(struct leme_pointer_settings, natural_scroll),
     .field = LEME_POINTER_NATURAL_SCROLL,
     .effect = LEME_OPTION_EFFECT_POINTERS},
    {.name = "left_handed",
     .kind = LEME_OPTION_BOOL,
     .offset = offsetof(struct leme_pointer_settings, left_handed),
     .field = LEME_POINTER_LEFT_HANDED,
     .effect = LEME_OPTION_EFFECT_POINTERS},
    {.name = "tap",
     .kind = LEME_OPTION_BOOL,
     .offset = offsetof(struct leme_pointer_settings, tap),
     .field = LEME_POINTER_TAP,
     .effect = LEME_OPTION_EFFECT_POINTERS},
    {.name = "focus_follows_mouse",
     .kind = LEME_OPTION_CUSTOM,
     .parse = leme_config_parse_focus_follows_mouse},
    {.name = "disable_while_typing",
     .kind = LEME_OPTION_BOOL,
     .offset = offsetof(struct leme_pointer_settings, disable_while_typing),
     .field = LEME_POINTER_DISABLE_WHILE_TYPING,
     .effect = LEME_OPTION_EFFECT_POINTERS},
    {.name = "click_method",
     .kind = LEME_OPTION_CHOICE,
     .offset = offsetof(struct leme_pointer_settings, click_method),
     LEME_OPTION_CHOICES(leme_pointer_click_names),
     .field = LEME_POINTER_CLICK_METHOD,
     .effect = LEME_OPTION_EFFECT_POINTERS},
    {.name = "scroll_method",
     .kind = LEME_OPTION_CHOICE,
     .offset = offsetof(struct leme_pointer_settings, scroll_method),
     LEME_OPTION_CHOICES(leme_pointer_scroll_names),
     .field = LEME_POINTER_SCROLL_METHOD,
     .effect = LEME_OPTION_EFFECT_POINTERS},
    {.name = "scroll_button",
     .kind = LEME_OPTION_CHOICE,
     .offset = offsetof(struct leme_pointer_settings, scroll_button),
     LEME_OPTION_CHOICES(leme_pointer_button_names),
     .field = LEME_POINTER_SCROLL_BUTTON,
     .effect = LEME_OPTION_EFFECT_POINTERS},
    {.name = "middle_emulation",
     .kind = LEME_OPTION_BOOL,
     .offset = offsetof(struct leme_pointer_settings, middle_emulation),
     .field = LEME_POINTER_MIDDLE_EMULATION,
     .effect = LEME_OPTION_EFFECT_POINTERS},
    {.name = "tap_button_map",
     .kind = LEME_OPTION_CHOICE,
     .offset = offsetof(struct leme_pointer_settings, tap_button_map),
     LEME_OPTION_CHOICES(leme_pointer_tap_map_names),
     .field = LEME_POINTER_TAP_BUTTON_MAP,
     .effect = LEME_OPTION_EFFECT_POINTERS},
    {.name = "tap_drag",
     .kind = LEME_OPTION_BOOL,
     .offset = offsetof(struct leme_pointer_settings, tap_drag),
     .field = LEME_POINTER_TAP_DRAG,
     .effect = LEME_OPTION_EFFECT_POINTERS},
    {.name = "drag_lock",
     .kind = LEME_OPTION_CHOICE,
     .offset = offsetof(struct leme_pointer_settings, drag_lock),
     LEME_OPTION_CHOICES(leme_pointer_drag_lock_names),
     .field = LEME_POINTER_DRAG_LOCK,
     .effect = LEME_OPTION_EFFECT_POINTERS},
    {.name = "drag_fingers",
     .kind = LEME_OPTION_CUSTOM,
     .field = LEME_POINTER_DRAG_FINGERS,
     .parse = leme_config_parse_drag_fingers},
    {.name = "scroll_factor",
     .kind = LEME_OPTION_DECIMAL,
     .offset = offsetof(struct leme_pointer_settings, scroll_factor),
     .min = 0.0,
     .max = LEME_POINTER_SCROLL_FACTOR_MAX,
     .flags = LEME_OPTION_MIN_OPEN,
     .field = LEME_POINTER_SCROLL_FACTOR,
     .effect = LEME_OPTION_EFFECT_POINTERS},
};

const struct leme_option_block leme_pointer_block = {
    .name = "pointer",
    .options = leme_pointer_options,
    .count = LEME_OPTION_COUNT(leme_pointer_options),
};

bool leme_config_parse_pointer(struct leme_config *config,
                               const struct leme_scfg_directive *directive,
                               const char *path, char **error,
                               bool *have_global) {
  struct leme_pointer_settings local = {0};
  struct leme_pointer_settings *settings;
  const char *name = NULL;

  if (directive->params_len > 1) {
    leme_config_set_error(error,
                          "%s:%d: pointer accepts at most one device name",
                          path, directive->lineno);
    return false;
  }
  if (directive->params_len == 0) {
    if (*have_global) {
      leme_config_set_error(error, "%s:%d: duplicate global pointer block",
                            path, directive->lineno);
      return false;
    }
    *have_global = true;
    settings = &config->pointer_defaults;
  } else {
    name = directive->params[0];
    if (name[0] == '\0') {
      leme_config_set_error(error, "%s:%d: pointer device name cannot be empty",
                            path, directive->lineno);
      return false;
    }
    for (size_t index = 0; index < config->pointer_rule_count; index++) {
      if (strcmp(config->pointer_rules[index].name, name) == 0) {
        leme_config_set_error(error, "%s:%d: duplicate pointer device %s", path,
                              directive->lineno, name);
        return false;
      }
    }
    settings = &local;
  }
  if (!leme_config_parse_options(config, directive, &leme_pointer_block,
                                 settings, &settings->fields, NULL, path,
                                 error)) {
    return false;
  }
  if (name != NULL) {
    struct leme_pointer_rule *rules =
        realloc(config->pointer_rules,
                (config->pointer_rule_count + 1) * sizeof(*rules));
    struct leme_pointer_rule *rule;

    if (rules == NULL) {
      leme_config_set_error(error, "%s:%d: out of memory", path,
                            directive->lineno);
      return false;
    }
    config->pointer_rules = rules;
    rule = &config->pointer_rules[config->pointer_rule_count];
    *rule = (struct leme_pointer_rule){
        .name = strdup(name),
        .settings = local,
    };
    config->pointer_rule_count++;
    if (rule->name == NULL) {
      leme_config_set_error(error, "%s:%d: out of memory", path,
                            directive->lineno);
      return false;
    }
  }
  return true;
}

static bool leme_config_parse_keyboard_layout(
    struct leme_config *config, const struct leme_scfg_directive *entry,
    const struct leme_scfg_directive **layout_directives, const char *path,
    char **error) {
  struct leme_keyboard_layout *layouts;
  struct leme_keyboard_layout *layout;
  const char *name;
  const char *variant;
  size_t previous;
  bool duplicate = false;

  if (entry->params_len < 1 || entry->params_len > 2 ||
      entry->children.directives_len != 0) {
    return leme_config_reject(config, entry, -1,
                              "layout requires a name and optional variant");
  }
  name = entry->params[0];
  variant = entry->params_len == 2 ? entry->params[1] : NULL;
  if (name[0] == '\0' || strchr(name, ',') != NULL ||
      (variant != NULL &&
       (variant[0] == '\0' || strchr(variant, ',') != NULL))) {
    return leme_config_reject(config, entry, 0,
                              "invalid keyboard layout or variant");
  }
  for (previous = 0; previous < config->keyboard_layout_count; previous++) {
    const struct leme_keyboard_layout *existing =
        &config->keyboard_layouts[previous];
    bool variants_match =
        existing->variant == NULL
            ? variant == NULL
            : variant != NULL && strcmp(existing->variant, variant) == 0;

    if (strcmp(existing->name, name) == 0 && variants_match) {
      duplicate = true;
      break;
    }
  }
  if (duplicate) {
    const struct leme_reject_extra extra = {
        .secondary = layout_directives[previous],
        .secondary_label = "first defined here",
    };
    return leme_config_reject_detailed(config, entry, -1, &extra,
                                       "duplicate keyboard layout %s", name);
  }
  layouts = realloc(config->keyboard_layouts,
                    (config->keyboard_layout_count + 1) * sizeof(*layouts));
  if (layouts == NULL) {
    leme_config_set_error(error, "%s:%d: out of memory", path, entry->lineno);
    return false;
  }
  config->keyboard_layouts = layouts;
  layout = &config->keyboard_layouts[config->keyboard_layout_count];
  *layout = (struct leme_keyboard_layout){
      .name = strdup(name),
      .variant = variant == NULL ? NULL : strdup(variant),
  };
  if (config->keyboard_layout_count < 4) {
    layout_directives[config->keyboard_layout_count] = entry;
  }
  config->keyboard_layout_count++;
  if (layout->name == NULL || (variant != NULL && layout->variant == NULL)) {
    leme_config_set_error(error, "%s:%d: out of memory", path, entry->lineno);
    return false;
  }
  return true;
}

static bool leme_config_keyboard_option_valid(const char *text) {
  const size_t length = strlen(text);

  return length > 0 && text[0] != ',' && text[length - 1] != ',' &&
         strstr(text, ",,") == NULL;
}

static bool
leme_config_parse_keyboard_options(struct leme_config *config,
                                   const struct leme_scfg_directive *entry,
                                   const char *path, char **error) {
  size_t param;

  if (entry->params_len < 1 || entry->children.directives_len != 0) {
    return leme_config_reject(config, entry, -1,
                              "options requires at least one XKB option");
  }
  for (param = 0; param < entry->params_len; param++) {
    if (!leme_config_keyboard_option_valid(entry->params[param])) {
      return leme_config_reject(config, entry, (int)param,
                                "invalid XKB option %s", entry->params[param]);
    }
  }
  for (param = 0; param < entry->params_len; param++) {
    const char *cursor = entry->params[param];

    for (;;) {
      const char *comma = strchr(cursor, ',');
      const size_t length =
          comma == NULL ? strlen(cursor) : (size_t)(comma - cursor);
      char **options;
      char *option;

      if (config->keyboard_option_count >= LEME_KEYBOARD_OPTIONS_MAX) {
        return leme_config_reject(config, entry, (int)param,
                                  "keyboard takes at most %d options",
                                  LEME_KEYBOARD_OPTIONS_MAX);
      }
      option = strndup(cursor, length);
      options =
          option == NULL
              ? NULL
              : realloc(config->keyboard_options,
                        (config->keyboard_option_count + 1) * sizeof(*options));
      if (options == NULL) {
        free(option);
        leme_config_set_error(error, "%s:%d: out of memory", path,
                              entry->lineno);
        return false;
      }
      config->keyboard_options = options;
      config->keyboard_options[config->keyboard_option_count++] = option;
      if (comma == NULL) {
        break;
      }
      cursor = comma + 1;
    }
  }
  return true;
}

struct leme_keyboard_parse {
  const struct leme_scfg_directive *layouts[4];
};

static enum leme_option_result leme_config_parse_keyboard_layout_option(
    const struct leme_option_parse *parse) {
  const struct leme_config_parse_context *context = parse->data;
  struct leme_keyboard_parse *keyboard = context->state;

  if (parse->config->keyboard_layout_count == 4) {
    leme_config_set_error(context->error,
                          "%s:%d: keyboard takes at most four layouts",
                          context->path, parse->entry->lineno);
    return LEME_OPTION_FATAL;
  }
  return leme_config_parse_keyboard_layout(parse->config, parse->entry,
                                           keyboard->layouts, context->path,
                                           context->error)
             ? LEME_OPTION_ACCEPTED
             : LEME_OPTION_FATAL;
}

static enum leme_option_result leme_config_parse_keyboard_options_option(
    const struct leme_option_parse *parse) {
  const struct leme_config_parse_context *context = parse->data;

  return leme_config_parse_keyboard_options(parse->config, parse->entry,
                                            context->path, context->error)
             ? LEME_OPTION_ACCEPTED
             : LEME_OPTION_FATAL;
}

static enum leme_option_result
leme_config_parse_keyboard_repeat(const struct leme_option_parse *parse) {
  const struct leme_scfg_directive *entry = parse->entry;
  const struct leme_option *option = parse->option;
  int value = 0;

  if (entry->params_len != 1 || entry->children.directives_len != 0 ||
      !leme_config_parse_nonnegative(entry->params[0], &value) ||
      value < option->min || value > option->max) {
    return leme_option_reject(parse, 0, NULL,
                              "`%s` expects an integer from %.0f through %.0f",
                              option->name, option->min, option->max);
  }
  *(int *)((char *)parse->target + option->offset) = value;
  return LEME_OPTION_ACCEPTED;
}

static const struct leme_option leme_keyboard_options[] = {
    {.name = "layout",
     .kind = LEME_OPTION_CUSTOM,
     .flags = LEME_OPTION_MULTI | LEME_OPTION_REPEAT,
     .parse = leme_config_parse_keyboard_layout_option},
    {.name = "options",
     .kind = LEME_OPTION_CUSTOM,
     .flags = LEME_OPTION_MULTI,
     .parse = leme_config_parse_keyboard_options_option},
    {.name = "repeat_rate",
     .kind = LEME_OPTION_CUSTOM,
     .offset = offsetof(struct leme_config, keyboard_repeat_rate),
     .min = 0,
     .max = LEME_KEYBOARD_REPEAT_RATE_MAX,
     .flags = LEME_OPTION_MULTI,
     .parse = leme_config_parse_keyboard_repeat},
    {.name = "repeat_delay",
     .kind = LEME_OPTION_CUSTOM,
     .offset = offsetof(struct leme_config, keyboard_repeat_delay),
     .min = 1,
     .max = LEME_KEYBOARD_REPEAT_DELAY_MAX,
     .flags = LEME_OPTION_MULTI,
     .parse = leme_config_parse_keyboard_repeat},
};

static const struct leme_option_block leme_keyboard_block = {
    .name = "keyboard",
    .options = leme_keyboard_options,
    .count = LEME_OPTION_COUNT(leme_keyboard_options),
    .flags = LEME_OPTION_BLOCK_FIRST_WINS,
};

bool leme_config_parse_keyboard(struct leme_config *config,
                                const struct leme_scfg_directive *directive,
                                const char *path, char **error) {
  struct leme_keyboard_parse keyboard = {0};

  if (directive->params_len != 0) {
    leme_config_set_error(error, "%s:%d: keyboard takes no arguments", path,
                          directive->lineno);
    return false;
  }
  if (directive->children.directives_len == 0) {
    leme_config_set_error(error, "%s:%d: keyboard requires at least one entry",
                          path, directive->lineno);
    return false;
  }
  return leme_config_parse_options(config, directive, &leme_keyboard_block,
                                   config, NULL, &keyboard, path, error);
}
