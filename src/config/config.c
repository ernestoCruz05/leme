#include "config/config.h"
#include "config/internal.h"
#include "config/style.h"

#include "core/command.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wlr/types/wlr_keyboard.h>
#include <xkbcommon/xkbcommon-keysyms.h>

void leme_config_set_error(char **error, const char *format, ...) {
  va_list arguments;
  va_list copy;
  int length;

  if (error == NULL) {
    return;
  }
  va_start(arguments, format);
  va_copy(copy, arguments);
  length = vsnprintf(NULL, 0, format, copy);
  va_end(copy);
  if (length < 0) {
    *error = NULL;
    va_end(arguments);
    return;
  }
  *error = malloc((size_t)length + 1);
  if (*error != NULL) {
    vsnprintf(*error, (size_t)length + 1, format, arguments);
  }
  va_end(arguments);
}

char **leme_config_copy_argv(const char *name, char *const *params,
                             size_t count) {
  char **argv = calloc(count + 2, sizeof(*argv));
  size_t index;

  if (argv == NULL) {
    return NULL;
  }
  argv[0] = strdup(name);
  if (argv[0] == NULL) {
    free(argv);
    return NULL;
  }
  for (index = 0; index < count; index++) {
    argv[index + 1] = strdup(params[index]);
    if (argv[index + 1] == NULL) {
      while (index > 0) {
        free(argv[index--]);
      }
      free(argv[0]);
      free(argv);
      return NULL;
    }
  }
  return argv;
}

void leme_config_free_argv(char **argv) {
  size_t index;

  if (argv == NULL) {
    return;
  }
  for (index = 0; argv[index] != NULL; index++) {
    free(argv[index]);
  }
  free(argv);
}

void leme_config_finish_command(struct leme_command *command) {
  free(command->text);
  leme_config_free_argv(command->argv);
  *command = (struct leme_command){0};
}

bool leme_config_copy_command(struct leme_command *destination,
                              const struct leme_command *source) {
  size_t count = 0;
  size_t index;

  *destination = *source;
  destination->text = NULL;
  destination->argv = NULL;
  if (source->text != NULL) {
    destination->text = strdup(source->text);
    if (destination->text == NULL) {
      return false;
    }
  }
  if (source->argv == NULL) {
    return true;
  }
  while (source->argv[count] != NULL) {
    count++;
  }
  destination->argv = calloc(count + 1, sizeof(*destination->argv));
  if (destination->argv == NULL) {
    leme_config_finish_command(destination);
    return false;
  }
  for (index = 0; index < count; index++) {
    destination->argv[index] = strdup(source->argv[index]);
    if (destination->argv[index] == NULL) {
      leme_config_finish_command(destination);
      return false;
    }
  }
  return true;
}

struct leme_config *leme_config_ref(struct leme_config *config) {
  if (config == NULL) {
    return NULL;
  }
  config->refcount++;
  return config;
}

void leme_config_destroy(struct leme_config *config) {
  size_t mode_index;
  size_t binding_index;
  size_t identity_index;
  size_t index;

  if (config == NULL) {
    return;
  }
  if (config->refcount > 1) {
    config->refcount--;
    return;
  }
  if (config->is_effective_shell) {
    free(config->cursor.theme);
    free(config->path);
    if (config->baseline != NULL) {
      leme_config_destroy(config->baseline);
      config->baseline = NULL;
    }
    free(config);
    return;
  }
  for (index = 0; index < config->keyboard_layout_count; index++) {
    free(config->keyboard_layouts[index].name);
    free(config->keyboard_layouts[index].variant);
  }
  free(config->keyboard_layouts);
  for (index = 0; index < config->keyboard_option_count; index++) {
    free(config->keyboard_options[index]);
  }
  free(config->keyboard_options);
  for (index = 0; index < config->output_count; index++) {
    free(config->outputs[index].name);
    free(config->outputs[index].relative_to);
  }
  free(config->outputs);
  leme_desk_style_destroy(config->desk_style);
  leme_config_desktop_menu_finish(config);
  for (index = 0; index < config->tag_rule_count; index++) {
    free(config->tag_rules[index].ids);
  }
  free(config->tag_rules);
  for (index = 0; index < config->window_rule_count; index++) {
    for (identity_index = 0;
         identity_index < config->window_rules[index].identity_count;
         identity_index++) {
      free(config->window_rules[index].identities[identity_index]);
    }
    free(config->window_rules[index].identities);
    free(config->window_rules[index].title);
    free(config->window_rules[index].output);
  }
  free(config->window_rules);
  for (index = 0; index < config->pointer_rule_count; index++) {
    free(config->pointer_rules[index].name);
  }
  free(config->pointer_rules);
  for (mode_index = 0; mode_index < config->mode_count; mode_index++) {
    struct leme_mode *mode = &config->modes[mode_index];
    for (binding_index = 0; binding_index < mode->binding_count;
         binding_index++) {
      leme_config_finish_command(&mode->bindings[binding_index].command);
    }
    free(mode->bindings);
    free(mode->name);
  }
  free(config->modes);
  for (index = 0; index < config->environment_count; index++) {
    free(config->environment[index].name);
    free(config->environment[index].value);
  }
  free(config->environment);
  for (index = 0; index < config->startup_count; index++) {
    leme_config_free_argv(config->startup[index].argv);
  }
  free(config->startup);
  for (index = 0; index < config->scratchpad_count; index++) {
    free(config->scratchpads[index].name);
    free(config->scratchpads[index].identity);
    leme_config_free_argv(config->scratchpads[index].spawn);
  }
  free(config->scratchpads);
  leme_diagnostics_finish(&config->diagnostics);
  leme_source_table_finish(&config->sources);
  leme_trail_table_finish(&config->trails);
  free(config->cursor.theme);
  free(config->path);
  free(config);
}

void leme_config_set_output_defaults(struct leme_config *config) {
  config->outputs = NULL;
  config->output_count = 0;
  config->output_policy = (struct leme_output_policy){
      .cross_output_focus = true,
      .cross_output_move = false,
      .cross_output_drag = true,
      .warp_cursor = true,
      .secondary_gpu = LEME_SECONDARY_GPU_ON_DEMAND,
  };
  config->tag_defaults = (struct leme_tag_settings){
      .layout = LEME_LAYOUT_DWINDLE,
      .drop_mode = LEME_DROP_MODE_SIMPLE,
      .mfact = 0.5,
      .nmaster = 1,
      .gap = 0,
      .split_ratio = 0.5,
      .has_gap = false,
  };
  config->tag_rules = NULL;
  config->tag_rule_count = 0;
  config->window_rules = NULL;
  config->window_rule_count = 0;
}

void leme_config_set_style_defaults(struct leme_config *config) {
  config->gap_outer = (struct leme_gaps){0};
  config->smart_gaps = false;
  config->border_active[0] = 0.16f;
  config->border_active[1] = 0.42f;
  config->border_active[2] = 0.72f;
  config->border_active[3] = 1.0f;
  config->border_inactive[0] = 0.23f;
  config->border_inactive[1] = 0.23f;
  config->border_inactive[2] = 0.23f;
  config->border_inactive[3] = 1.0f;
  config->opacity_active = 1.0;
  config->opacity_inactive = 1.0;
  config->fullscreen_covers = LEME_FULLSCREEN_COVERS_TOP;
}

const struct leme_scratchpad_config *
leme_config_scratchpad(const struct leme_config *config, const char *name) {
  size_t index;

  if (config == NULL || name == NULL ||
      (config->scratchpad_count > 0 && config->scratchpads == NULL)) {
    return NULL;
  }
  for (index = 0; index < config->scratchpad_count; index++) {
    if (config->scratchpads[index].name != NULL &&
        strcmp(config->scratchpads[index].name, name) == 0) {
      return &config->scratchpads[index];
    }
  }
  return NULL;
}

void leme_config_tag_settings(const struct leme_config *config, uint16_t id,
                              struct leme_tag_settings *settings) {
  size_t rule_index;
  size_t id_index;

  *settings = config->tag_defaults;
  for (rule_index = 0; rule_index < config->tag_rule_count; rule_index++) {
    const struct leme_tag_rule *rule = &config->tag_rules[rule_index];

    for (id_index = 0; id_index < rule->id_count; id_index++) {
      if (rule->ids[id_index] != id) {
        continue;
      }
      if ((rule->fields & LEME_TAG_FIELD_LAYOUT) != 0) {
        settings->layout = rule->settings.layout;
      }
      if ((rule->fields & LEME_TAG_FIELD_DROP_MODE) != 0) {
        settings->drop_mode = rule->settings.drop_mode;
      }
      if ((rule->fields & LEME_TAG_FIELD_MFACT) != 0) {
        settings->mfact = rule->settings.mfact;
      }
      if ((rule->fields & LEME_TAG_FIELD_NMASTER) != 0) {
        settings->nmaster = rule->settings.nmaster;
      }
      if ((rule->fields & LEME_TAG_FIELD_GAP) != 0) {
        settings->gap = rule->settings.gap;
        settings->has_gap = true;
      }
      if ((rule->fields & LEME_TAG_FIELD_SPLIT_RATIO) != 0) {
        settings->split_ratio = rule->settings.split_ratio;
      }
      return;
    }
  }
}

void leme_config_set_pointer_defaults(struct leme_config *config) {
  config->pointer_defaults = (struct leme_pointer_settings){
      .fields = LEME_POINTER_PROFILE | LEME_POINTER_SPEED |
                LEME_POINTER_NATURAL_SCROLL | LEME_POINTER_LEFT_HANDED |
                LEME_POINTER_TAP,
      .profile = LEME_POINTER_ACCEL_ADAPTIVE,
      .speed = 0.0,
      .natural_scroll = false,
      .left_handed = false,
      .tap = true,
  };
  config->focus_follows_mouse = true;
}

const char *const leme_choice_activation[3] = {
    "follow",
    "urgent",
    "ignore",
};
const char *const leme_choice_banner[2] = {
    "top",
    "bottom",
};
const char *const leme_choice_accel_profile[2] = {
    "adaptive",
    "flat",
};
const char *const leme_choice_drop_mode[2] = {
    "simple",
    "edges",
};
const char *const leme_choice_fullscreen_covers[3] = {
    "none",
    "top",
    "overlay",
};
const char *const leme_choice_gesture_mode[3] = {
    "single",
    "scrub",
    "free",
};
const char *const leme_choice_layout[3] = {
    "dwindle",
    "master_stack",
    "desktop",
};
const char *const leme_choice_secondary_gpu[2] = {
    "on_demand",
    "always",
};

const char *const leme_choice_workspace_style[2] = {
    "full_slide",
    "glide_fade",
};
const char *const leme_pointer_click_names[3] = {
    "none",
    "button_areas",
    "clickfinger",
};
const char *const leme_pointer_scroll_names[4] = {
    "none",
    "two_finger",
    "edge",
    "on_button_down",
};
const char *const leme_pointer_button_names[8] = {
    "left", "right", "middle", "side", "extra", "forward", "back", "task",
};
const char *const leme_pointer_tap_map_names[2] = {
    "lrm",
    "lmr",
};
const char *const leme_pointer_drag_lock_names[3] = {
    "none",
    "timeout",
    "sticky",
};

static const char *const *
leme_pointer_choice_names(enum leme_pointer_choice choice, size_t *count) {
  switch (choice) {
  case LEME_POINTER_CHOICE_CLICK_METHOD:
    *count =
        sizeof(leme_pointer_click_names) / sizeof(*leme_pointer_click_names);
    return leme_pointer_click_names;
  case LEME_POINTER_CHOICE_SCROLL_METHOD:
    *count =
        sizeof(leme_pointer_scroll_names) / sizeof(*leme_pointer_scroll_names);
    return leme_pointer_scroll_names;
  case LEME_POINTER_CHOICE_SCROLL_BUTTON:
    *count =
        sizeof(leme_pointer_button_names) / sizeof(*leme_pointer_button_names);
    return leme_pointer_button_names;
  case LEME_POINTER_CHOICE_TAP_BUTTON_MAP:
    *count = sizeof(leme_pointer_tap_map_names) /
             sizeof(*leme_pointer_tap_map_names);
    return leme_pointer_tap_map_names;
  case LEME_POINTER_CHOICE_DRAG_LOCK:
    *count = sizeof(leme_pointer_drag_lock_names) /
             sizeof(*leme_pointer_drag_lock_names);
    return leme_pointer_drag_lock_names;
  }
  *count = 0;
  return NULL;
}

uint32_t leme_config_swipe_fingers(const struct leme_config *config) {
  const uint32_t tag_swipe = config->gestures.workspace_switch.fingers;
  uint32_t fingers = tag_swipe != 0 && tag_swipe < 32 ? 1u << tag_swipe : 0;

  for (size_t i = 0; i < config->mode_count; i++) {
    const struct leme_mode *mode = &config->modes[i];

    for (size_t j = 0; j < mode->binding_count; j++) {
      if (mode->bindings[j].trigger == LEME_BINDING_SWIPE) {
        fingers |= 1u << mode->bindings[j].fingers;
      }
    }
  }
  return fingers;
}

const char *leme_pointer_choice_name(enum leme_pointer_choice choice,
                                     unsigned value) {
  size_t count = 0;
  const char *const *names = leme_pointer_choice_names(choice, &count);

  return value < count ? names[value] : NULL;
}

bool leme_pointer_choice_find(enum leme_pointer_choice choice, const char *text,
                              size_t length, unsigned *value) {
  size_t count = 0;
  const char *const *names = leme_pointer_choice_names(choice, &count);

  for (unsigned i = 0; i < count; i++) {
    if (strlen(names[i]) == length && memcmp(names[i], text, length) == 0) {
      *value = i;
      return true;
    }
  }
  return false;
}

bool leme_config_set_default_keyboard(struct leme_config *config) {
  config->keyboard_layouts = calloc(1, sizeof(*config->keyboard_layouts));
  if (config->keyboard_layouts == NULL) {
    return false;
  }
  config->keyboard_layout_count = 1;
  config->keyboard_layouts[0].name = strdup("us");
  return config->keyboard_layouts[0].name != NULL;
}

static bool leme_config_default_binding(struct leme_mode *mode, size_t index,
                                        uint32_t modifiers, xkb_keysym_t keysym,
                                        enum leme_command_type type) {
  mode->bindings[index].modifiers = modifiers;
  mode->bindings[index].keysym = keysym;
  mode->bindings[index].command.type = type;
  return true;
}

struct leme_config *leme_config_defaults(void) {
  struct leme_config *config = calloc(1, sizeof(*config));
  struct leme_mode *common;
  size_t index;
  char shell_flag[] = "-c";
  char terminal_script[] =
      "for t in \"$TERMINAL\" foot alacritty kitty wezterm ghostty xterm; do "
      "command -v \"$t\" >/dev/null 2>&1 && exec \"$t\"; done";
  char *terminal_params[] = {shell_flag, terminal_script};

  if (config == NULL) {
    return NULL;
  }
  config->refcount = 1;
  config->initial_tags = 3;
  config->desktop_minimized_strip = true;
  config->desktop_launchers = true;
  config->desktop_container_modifier = WLR_MODIFIER_SHIFT;
  if (!leme_config_desktop_menu_defaults(config)) {
    leme_config_destroy(config);
    return NULL;
  }
  config->max_tags = 9;
  config->cursor.size = LEME_CURSOR_SIZE_DEFAULT;
  config->keyboard_repeat_rate = LEME_KEYBOARD_REPEAT_RATE_DEFAULT;
  config->keyboard_repeat_delay = LEME_KEYBOARD_REPEAT_DELAY_DEFAULT;
  config->gestures.workspace_switch =
      (struct leme_workspace_switch_gesture_settings){
          .mode = LEME_WORKSPACE_GESTURE_SINGLE,
          .fingers = 3,
          .distance = 300.0,
          .threshold = 0.5,
          .deceleration = 0.997,
          .velocity_window_ms = 150,
  };
  config->gestures.edges =
      (struct leme_edge_gesture_settings){.size = 8.0, .step = 5.0};
  config->publication.activation = LEME_ACTIVATION_FOLLOW;
  config->config_errors = (struct leme_config_errors){
      .show = true,
      .position = LEME_BANNER_TOP,
      .timeout = 0,
  };
  leme_config_set_style_defaults(config);
  leme_config_set_output_defaults(config);
  leme_config_set_pointer_defaults(config);
  if (!leme_config_set_default_keyboard(config)) {
    leme_config_destroy(config);
    return NULL;
  }
  config->modes = calloc(1, sizeof(*config->modes));
  if (config->modes == NULL) {
    leme_config_destroy(config);
    return NULL;
  }
  config->mode_count = 1;
  common = &config->modes[0];
  common->escape_exits = true;
  common->name = strdup("common");
  common->bindings = calloc(19, sizeof(*common->bindings));
  if (common->name == NULL || common->bindings == NULL) {
    leme_config_destroy(config);
    return NULL;
  }
  common->binding_count = 19;
  leme_config_default_binding(common, 0, 0, XKB_KEY_Escape,
                              LEME_COMMAND_SET_MODE);
  common->bindings[0].command.text = strdup("common");
  leme_config_default_binding(common, 1, WLR_MODIFIER_LOGO, XKB_KEY_Return,
                              LEME_COMMAND_SPAWN);
  common->bindings[1].command.argv =
      leme_config_copy_argv("/bin/sh", terminal_params, 2);
  leme_config_default_binding(common, 2, WLR_MODIFIER_LOGO, XKB_KEY_r,
                              LEME_COMMAND_RELOAD_CONFIG);
  leme_config_default_binding(common, 3, WLR_MODIFIER_LOGO, XKB_KEY_q,
                              LEME_COMMAND_QUIT);
  for (index = 0; index < 12; index++) {
    leme_config_default_binding(
        common, index + 4, WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT,
        XKB_KEY_F1 + (xkb_keysym_t)index, LEME_COMMAND_SWITCH_VT);
    common->bindings[index + 4].command.vt = (uint16_t)(index + 1);
  }
  leme_config_default_binding(common, 16, WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT,
                              XKB_KEY_space,
                              LEME_COMMAND_CYCLE_KEYBOARD_LAYOUT);
  leme_config_default_binding(common, 17, WLR_MODIFIER_LOGO, XKB_KEY_m,
                              LEME_COMMAND_MINIMIZE_VIEW);
  leme_config_default_binding(common, 18,
                              WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT, XKB_KEY_m,
                              LEME_COMMAND_RESTORE_MINIMIZED);
  if (common->bindings[0].command.text == NULL ||
      common->bindings[1].command.argv == NULL) {
    leme_config_destroy(config);
    return NULL;
  }
  return config;
}

const char *leme_config_path(void) {
  static char preferred[PATH_MAX];
  static char legacy[PATH_MAX];
  const char *base = getenv("XDG_CONFIG_HOME");
  const char *home;
  char directory[PATH_MAX];
  int length;

  if (base != NULL && base[0] != '\0') {
    length = snprintf(directory, sizeof(directory), "%s/leme", base);
  } else {
    home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
      return NULL;
    }
    length = snprintf(directory, sizeof(directory), "%s/.config/leme", home);
  }
  if (length < 0 || (size_t)length >= sizeof(directory)) {
    return NULL;
  }
  length = snprintf(preferred, sizeof(preferred), "%s/config.scfg", directory);
  if (length < 0 || (size_t)length >= sizeof(preferred)) {
    return NULL;
  }
  if (access(preferred, R_OK) == 0) {
    return preferred;
  }
  length = snprintf(legacy, sizeof(legacy), "%s/config", directory);
  if (length < 0 || (size_t)length >= sizeof(legacy)) {
    return NULL;
  }
  if (access(legacy, R_OK) == 0) {
    return legacy;
  }
  return preferred;
}
