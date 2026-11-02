#include "config/style-internal.h"

#include "config/internal.h"
#include "config/scfg.h"
#include "desktop/theme.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static bool leme_desk_style_reject_named(
    struct leme_config *config, const struct leme_scfg_directive *directive,
    int param, const char *written, const char *const *names, size_t count,
    const char *format, ...) {
  char message[256] = {0};
  va_list arguments;
  va_start(arguments, format);
  const int length = vsnprintf(message, sizeof(message), format, arguments);
  va_end(arguments);
  if (length < 0) {
    return false;
  }
  char help[128] = {0};
  struct leme_reject_extra extra = {0};
  const char *nearest = leme_config_nearest_key(written, names, count);
  if (nearest != NULL) {
    (void)snprintf(help, sizeof(help), "a name like this exists: `%s`",
                   nearest);
    extra.help = help;
  }
  return leme_config_reject_detailed(config, directive, param, &extra, "%s",
                                     message);
}

static bool leme_desk_style_selector(
    struct leme_config *config, const struct leme_scfg_directive *directive,
    enum leme_desk_element *element, enum leme_desk_state *state, bool *fatal) {
  const char *selector = directive->params[0];
  char name[64] = {0};
  const char *colon = strchr(selector, ':');
  const size_t length =
      colon == NULL ? strlen(selector) : (size_t)(colon - selector);
  const char *element_names[LEME_DESK_ELEMENT_COUNT] = {0};
  for (size_t i = 0; i < LEME_DESK_ELEMENT_COUNT; i++) {
    element_names[i] = leme_desk_style_elements[i].name;
  }
  (void)snprintf(name, sizeof(name), "%.*s",
                 length < sizeof(name) ? (int)length : (int)sizeof(name) - 1,
                 selector);
  size_t found = 0;
  while (found < LEME_DESK_ELEMENT_COUNT &&
         (length >= sizeof(name) || strcmp(name, element_names[found]) != 0)) {
    found++;
  }
  if (found == LEME_DESK_ELEMENT_COUNT) {
    *fatal = !leme_desk_style_reject_named(
        config, directive, 0, name, element_names, LEME_DESK_ELEMENT_COUNT,
        "unknown style element `%s`", name);
    return false;
  }
  *element = (enum leme_desk_element)found;
  *state = LEME_DESK_STATE_NORMAL;
  if (colon == NULL) {
    return true;
  }
  const char *written = colon + 1;
  size_t known = 0;
  while (known < LEME_ARRAY_LENGTH(leme_desk_style_state_names) &&
         strcmp(written, leme_desk_style_state_names[known]) != 0) {
    known++;
  }
  if (known == LEME_ARRAY_LENGTH(leme_desk_style_state_names)) {
    const char *allowed[LEME_ARRAY_LENGTH(leme_desk_style_state_names)] = {0};
    size_t count = 0;
    for (size_t i = 0; i < LEME_ARRAY_LENGTH(leme_desk_style_state_names);
         i++) {
      if ((leme_desk_style_elements[found].states &
           LEME_DESK_STATE_BIT(i + 1)) != 0) {
        allowed[count++] = leme_desk_style_state_names[i];
      }
    }
    *fatal = !leme_desk_style_reject_named(config, directive, 0, written,
                                           allowed, count,
                                           "unknown style state `%s`", written);
    return false;
  }
  *state = (enum leme_desk_state)(known + 1);
  if ((leme_desk_style_elements[found].states & LEME_DESK_STATE_BIT(*state)) ==
      0) {
    *fatal =
        !leme_config_reject(config, directive, 0, "`%s` is not a state of `%s`",
                            written, element_names[found]);
    return false;
  }
  return true;
}

static bool leme_desk_style_layout(struct leme_config *config,
                                   const struct leme_scfg_directive *entry,
                                   struct leme_desk_layout *layout,
                                   bool *fatal) {
  *layout = (struct leme_desk_layout){0};
  bool title = false;
  for (size_t index = 0; index < entry->params_len; index++) {
    const char *word = entry->params[index];
    if (strcasecmp(word, "title") == 0) {
      if (title) {
        *fatal = !leme_config_reject(config, entry, (int)index,
                                     "layout lists title twice");
        return false;
      }
      title = true;
      layout->left = layout->count;
      continue;
    }
    size_t known = 0;
    while (known < LEME_ARRAY_LENGTH(leme_desk_style_button_names) &&
           strcasecmp(word, leme_desk_style_button_names[known]) != 0) {
      known++;
    }
    if (known == LEME_ARRAY_LENGTH(leme_desk_style_button_names)) {
      *fatal = !leme_config_reject(config, entry, (int)index,
                                   "unknown layout word `%s`, expected title, "
                                   "minimize, maximize or close",
                                   word);
      return false;
    }
    for (size_t previous = 0; previous < layout->count; previous++) {
      if ((size_t)layout->buttons[previous] == known) {
        *fatal = !leme_config_reject(config, entry, (int)index,
                                     "layout lists %s twice", word);
        return false;
      }
    }
    layout->buttons[layout->count] = (enum leme_titlebar_button)known;
    layout->count++;
  }
  if (!title) {
    *fatal = !leme_config_reject(config, entry, -1,
                                 "layout needs `title` once, to mark where the "
                                 "title goes between the buttons");
    return false;
  }
  return true;
}

static char *leme_desk_style_icon_path(const struct leme_config *config,
                                       const char *written) {
  char path[LEME_DESK_STYLE_PATH_MAX] = {0};
  int length = 0;
  if (written[0] == '/') {
    length = snprintf(path, sizeof(path), "%s", written);
  } else if (written[0] == '~' && written[1] == '/') {
    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
      return NULL;
    }
    length = snprintf(path, sizeof(path), "%s%s", home, written + 1);
  } else {
    const char *slash =
        config->path == NULL ? NULL : strrchr(config->path, '/');
    const int directory = slash == NULL ? 0 : (int)(slash - config->path) + 1;
    length = snprintf(path, sizeof(path), "%.*s%s", directory,
                      config->path == NULL ? "" : config->path, written);
  }
  if (length < 0 || (size_t)length >= sizeof(path)) {
    return NULL;
  }
  return strdup(path);
}

static bool leme_desk_style_suffix(const char *path, const char *suffix) {
  const size_t length = strlen(path);
  const size_t ending = strlen(suffix);
  return length > ending && strcasecmp(path + length - ending, suffix) == 0;
}

static bool
leme_desk_style_text(struct leme_config *config,
                     const struct leme_scfg_directive *entry,
                     const struct leme_desk_style_property_info *info,
                     char **slot, bool *fatal) {
  const char *value = entry->params[0];
  const size_t length = strlen(value);
  if (length < (size_t)info->low || length > (size_t)info->high) {
    *fatal = !leme_config_reject(config, entry, 0,
                                 "%s requires a name of 1 to %d bytes",
                                 entry->name, info->high);
    return false;
  }
  char *copy = strdup(value);
  if (copy == NULL) {
    *fatal = true;
    return false;
  }
  free(*slot);
  *slot = copy;
  return true;
}

static bool leme_desk_style_value(struct leme_config *config,
                                  struct leme_desk_style_rule *rule,
                                  enum leme_desk_property property,
                                  const struct leme_scfg_directive *entry,
                                  bool *fatal) {
  const struct leme_desk_style_property_info *info =
      &leme_desk_style_properties[property];
  struct leme_desk_look *look = &rule->look;
  if (info->kind == LEME_DESK_STYLE_LAYOUT) {
    return leme_desk_style_layout(config, entry, &look->layout, fatal);
  }
  if (entry->params_len != 1) {
    *fatal = !leme_config_reject(config, entry, -1, "%s requires one value",
                                 entry->name);
    return false;
  }
  const char *value = entry->params[0];
  switch (info->kind) {
  case LEME_DESK_STYLE_COLOR: {
    float color[4] = {0};
    if (!leme_config_parse_color(value, color)) {
      *fatal = !leme_config_reject(
          config, entry, 0, "%s expects #RRGGBB or #RRGGBBAA", entry->name);
      return false;
    }
    memcpy(leme_desk_style_color_slot(look, property), color, sizeof(color));
    return true;
  }
  case LEME_DESK_STYLE_TEXT: {
    const bool font = property == LEME_DESK_PROPERTY_FONT;
    char *name = NULL;
    if (!leme_desk_style_text(config, entry, info, &name, fatal)) {
      return false;
    }
    if (!font && (strchr(name, '/') != NULL || name[0] == '~')) {
      char *folder = leme_desk_style_icon_path(config, name);
      struct stat status = {0};
      if (folder == NULL || stat(folder, &status) != 0 ||
          !S_ISDIR(status.st_mode) || access(folder, R_OK | X_OK) != 0) {
        *fatal = !leme_config_reject(
            config, entry, 0,
            "icon folder %s cannot be read; %s is used instead",
            folder == NULL ? name : folder, LEME_DESK_STYLE_ICON_THEME);
        free(folder);
        free(name);
        return false;
      }
      free(name);
      name = folder;
    } else if (!font && !leme_desk_theme_installed(name)) {
      *fatal = !leme_config_reject(
          config, entry, 0,
          "icon theme %s is not installed; %s is used instead", name,
          LEME_DESK_STYLE_ICON_THEME);
      free(name);
      return false;
    }
    char **slot = font ? &rule->font : &rule->theme;
    free(*slot);
    *slot = name;
    if (font) {
      look->font = name;
    } else {
      look->theme = name;
    }
    return true;
  }
  case LEME_DESK_STYLE_PIXELS: {
    int pixels = 0;
    if (!leme_config_parse_nonnegative(value, &pixels) || pixels < info->low ||
        pixels > info->high) {
      *fatal = !leme_config_reject(
          config, entry, 0, "%s requires a whole number from %d through %d",
          entry->name, info->low, info->high);
      return false;
    }
    *leme_desk_style_pixels_slot(look, property) = pixels;
    return true;
  }
  case LEME_DESK_STYLE_DECIMAL: {
    double decimal = 0.0;
    if (!leme_config_parse_decimal(value, &decimal) ||
        decimal < (double)info->low || decimal > (double)info->high) {
      *fatal = !leme_config_reject(
          config, entry, 0, "%s requires a decimal from %d.0 through %d.0",
          entry->name, info->low, info->high);
      return false;
    }
    look->opacity = decimal;
    return true;
  }
  case LEME_DESK_STYLE_ALIGN:
    if (strcasecmp(value, "left") == 0) {
      look->align = LEME_TITLEBAR_ALIGN_LEFT;
      return true;
    }
    if (strcasecmp(value, "center") == 0) {
      look->align = LEME_TITLEBAR_ALIGN_CENTER;
      return true;
    }
    *fatal =
        !leme_config_reject(config, entry, 0, "align must be left or center");
    return false;
  case LEME_DESK_STYLE_BOOLEAN:
    if (!leme_config_parse_boolean(value, &look->shadow)) {
      *fatal = !leme_config_reject(config, entry, 0, "%s must be true or false",
                                   entry->name);
      return false;
    }
    return true;
  case LEME_DESK_STYLE_PATH: {
    if (!leme_desk_style_suffix(value, ".svg") &&
        !leme_desk_style_suffix(value, ".png")) {
      *fatal = !leme_config_reject(config, entry, 0,
                                   "icon must be a .svg or .png file");
      return false;
    }
    char *path = leme_desk_style_icon_path(config, value);
    if (path == NULL) {
      *fatal =
          !leme_config_reject(config, entry, 0, "icon path cannot be expanded");
      return false;
    }
    if (access(path, R_OK) != 0) {
      *fatal = !leme_config_reject(
          config, entry, 0,
          "icon file %s cannot be read; the built-in glyph is drawn instead",
          path);
      free(path);
      return false;
    }
    free(rule->icon);
    rule->icon = path;
    look->icon = path;
    return true;
  }
  case LEME_DESK_STYLE_LAYOUT:
  default:
    return false;
  }
}

static bool leme_desk_style_layout_property(enum leme_desk_property property) {
  switch (property) {
  case LEME_DESK_PROPERTY_WIDTH:
  case LEME_DESK_PROPERTY_HEIGHT:
  case LEME_DESK_PROPERTY_PADDING:
  case LEME_DESK_PROPERTY_SIZE:
  case LEME_DESK_PROPERTY_SPACING:
  case LEME_DESK_PROPERTY_GAP:
  case LEME_DESK_PROPERTY_ALIGN:
  case LEME_DESK_PROPERTY_LAYOUT:
    return true;
  default:
    return false;
  }
}

static bool leme_desk_style_entry(struct leme_config *config,
                                  struct leme_desk_style_rule *rule,
                                  enum leme_desk_element element,
                                  enum leme_desk_state state,
                                  const struct leme_scfg_directive *entry,
                                  const struct leme_scfg_directive **seen,
                                  bool *fatal) {
  const char *names[LEME_DESK_PROPERTY_COUNT] = {0};
  for (size_t i = 0; i < LEME_DESK_PROPERTY_COUNT; i++) {
    names[i] = leme_desk_style_properties[i].name;
  }
  size_t found = 0;
  while (found < LEME_DESK_PROPERTY_COUNT &&
         strcmp(entry->name, names[found]) != 0) {
    found++;
  }
  if (found == LEME_DESK_PROPERTY_COUNT) {
    *fatal = !leme_desk_style_reject_named(
        config, entry, -1, entry->name, names, LEME_DESK_PROPERTY_COUNT,
        "unknown style property `%s`", entry->name);
    return false;
  }
  const enum leme_desk_property property = (enum leme_desk_property)found;
  if ((leme_desk_style_elements[element].properties &
       leme_desk_style_bit(property)) == 0) {
    *fatal = !leme_config_reject(config, entry, -1, "`%s` does not take `%s`",
                                 leme_desk_style_elements[element].name,
                                 entry->name);
    return false;
  }
  if (state != LEME_DESK_STATE_NORMAL &&
      leme_desk_style_layout_property(property)) {
    *fatal = !leme_config_reject(
        config, entry, -1, "`%s` can only be set on `%s` without a state",
        entry->name, leme_desk_style_elements[element].name);
    return false;
  }
  if (seen[found] != NULL) {
    const struct leme_reject_extra extra = {
        .secondary = seen[found],
        .secondary_label = "first defined here",
    };
    *fatal = !leme_config_reject_detailed(
        config, entry, -1, &extra, "duplicate property `%s`", entry->name);
    return false;
  }
  if (entry->children.directives_len != 0) {
    *fatal = !leme_config_reject(config, entry, -1, "%s takes no block",
                                 entry->name);
    return false;
  }
  if (!leme_desk_style_value(config, rule, property, entry, fatal)) {
    return false;
  }
  seen[found] = entry;
  rule->look.set |= leme_desk_style_bit(property);
  return true;
}

bool leme_config_parse_style_rule(struct leme_config *config,
                                  const struct leme_scfg_directive *directive,
                                  const char *path, char **error) {
  bool fatal = false;
  if (directive->params_len != 1) {
    return leme_config_reject(config, directive, 1,
                              "style takes one selector, such as "
                              "`style \"menu_item:hover\"`");
  }
  enum leme_desk_element element = LEME_DESK_ELEMENT_TITLEBAR;
  enum leme_desk_state state = LEME_DESK_STATE_NORMAL;
  if (!leme_desk_style_selector(config, directive, &element, &state, &fatal)) {
    return !fatal;
  }
  if (config->desk_style == NULL) {
    config->desk_style = calloc(1, sizeof(*config->desk_style));
    if (config->desk_style == NULL) {
      leme_config_set_error(error, "%s:%d: out of memory", path,
                            directive->lineno);
      return false;
    }
  }
  struct leme_desk_style_rule *rule =
      &config->desk_style->rules[element][state];
  if (rule->lineno != 0) {
    return leme_config_reject(config, directive, 0,
                              "duplicate style block `%s`, first defined on "
                              "line %d",
                              directive->params[0], rule->lineno);
  }
  rule->lineno = directive->lineno;
  const struct leme_scfg_directive *seen[LEME_DESK_PROPERTY_COUNT] = {0};
  for (size_t index = 0; index < directive->children.directives_len; index++) {
    const struct leme_scfg_directive *entry =
        &directive->children.directives[index];
    (void)leme_desk_style_entry(config, rule, element, state, entry, seen,
                                &fatal);
    if (fatal) {
      leme_config_set_error(error, "%s:%d: out of memory", path, entry->lineno);
      return false;
    }
  }
  const struct leme_scfg_directive *spacing = seen[LEME_DESK_PROPERTY_SPACING];
  if (element == LEME_DESK_ELEMENT_ICON && spacing != NULL) {
    struct leme_desk_look look = {0};
    leme_desk_style_resolve(config, LEME_DESK_ELEMENT_ICON, 0, &look);
    if (look.spacing < look.size || look.spacing < look.width) {
      rule->look.set &= ~leme_desk_style_bit(LEME_DESK_PROPERTY_SPACING);
      return leme_config_reject(config, spacing, 0,
                                "spacing %d is smaller than the icon size %d "
                                "or the label width %d",
                                look.spacing, look.size, look.width);
    }
  }
  return true;
}
