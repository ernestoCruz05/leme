#include "config/config.h"
#include "config/envfile.h"
#include "config/expand.h"
#include "config/internal.h"
#include "config/render.h"
#include "config/scfg.h"
#include "config/source_table.h"

#include "core/command.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wlr/types/wlr_keyboard.h>

struct leme_reject_extra {
  const char *label;
  const char *help;
  const struct leme_scfg_directive *secondary;
  const char *secondary_label;
};

#define LEME_KEY_MAX 64U

static size_t leme_config_edit_distance(const char *left, const char *right) {
  size_t row[LEME_KEY_MAX + 1];
  size_t left_length = strnlen(left, LEME_KEY_MAX);
  size_t right_length = strnlen(right, LEME_KEY_MAX);
  size_t i;
  size_t j;

  for (j = 0; j <= right_length; j++) {
    row[j] = j;
  }
  for (i = 1; i <= left_length; i++) {
    size_t previous = row[0];

    row[0] = i;
    for (j = 1; j <= right_length; j++) {
      size_t current = row[j];
      size_t cost = (unsigned char)left[i - 1] == (unsigned char)right[j - 1]
                        ? previous
                        : previous + 1;

      if (row[j] + 1 < cost) {
        cost = row[j] + 1;
      }
      if (row[j - 1] + 1 < cost) {
        cost = row[j - 1] + 1;
      }
      row[j] = cost;
      previous = current;
    }
  }
  return row[right_length];
}

static const char *leme_config_nearest_key(const char *written,
                                           const char *const *keys,
                                           size_t count) {
  const char *best = NULL;
  size_t best_distance = 0;
  size_t written_length = strnlen(written, LEME_KEY_MAX);
  size_t limit = written_length / 3;
  size_t index;

  if (limit > 2) {
    limit = 2;
  }
  if (limit == 0) {
    return NULL;
  }
  for (index = 0; index < count; index++) {
    size_t distance = leme_config_edit_distance(written, keys[index]);

    if (distance <= limit && (best == NULL || distance < best_distance)) {
      best = keys[index];
      best_distance = distance;
    }
  }
  return best;
}

static bool leme_config_reject_detailed(
    struct leme_config *config, const struct leme_scfg_directive *directive,
    int param, const struct leme_reject_extra *extra, const char *format, ...) {
  struct leme_diagnostic_detail detail = {
      .severity = LEME_DIAGNOSTIC_WARNING,
      .line = directive->lineno,
      .trail = directive->trail,
  };
  va_list arguments;
  char message[256];
  int written;

  detail.primary.source = 0;
  if (param >= 0 && (size_t)param < directive->params_len &&
      directive->param_spans != NULL) {
    detail.primary.span = directive->param_spans[param];
  } else {
    detail.primary.span = directive->name_span;
  }
  if (extra != NULL) {
    detail.label = extra->label;
    detail.help = extra->help;
    if (extra->secondary != NULL) {
      detail.has_secondary = true;
      detail.secondary.source = 0;
      detail.secondary.span = extra->secondary->name_span;
      detail.secondary_label = extra->secondary_label;
    }
  }
  va_start(arguments, format);
  written = vsnprintf(message, sizeof(message), format, arguments);
  va_end(arguments);
  if (written < 0) {
    return false;
  }
  return leme_diagnostics_add_detailed(&config->diagnostics, &detail, "%s",
                                       message);
}

static bool leme_config_reject(struct leme_config *config,
                               const struct leme_scfg_directive *directive,
                               int param, const char *format, ...) {
  va_list arguments;
  char message[256];
  int written;

  va_start(arguments, format);
  written = vsnprintf(message, sizeof(message), format, arguments);
  va_end(arguments);
  if (written < 0) {
    return false;
  }
  return leme_config_reject_detailed(config, directive, param, NULL, "%s",
                                     message);
}

enum leme_bind_state {
  LEME_BIND_UNVISITED,
  LEME_BIND_RESOLVING,
  LEME_BIND_RESOLVED,
};

struct leme_bind_group {
  char *name;
  struct leme_binding *bindings;
  size_t binding_count;
  char **inherits;
  size_t inherit_count;
  bool is_mode;
  bool escape_exits;
  int lineno;
  enum leme_bind_state state;
  const struct leme_binding **resolved;
  const char **origin;
  size_t resolved_count;
};

struct leme_bind_scope {
  struct leme_bind_group *groups;
  size_t count;
};
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon-keysyms.h>

static bool leme_config_parse_u16(const char *text, uint16_t *value) {
  char *end;
  unsigned long parsed;

  errno = 0;
  parsed = strtoul(text, &end, 10);
  if (errno != 0 || text[0] == '\0' || *end != '\0' || parsed == 0 ||
      parsed > UINT16_MAX) {
    return false;
  }
  *value = (uint16_t)parsed;
  return true;
}

static bool leme_config_parse_u16_allow_zero(const char *text,
                                             uint16_t *value) {
  char *end;
  unsigned long parsed;

  errno = 0;
  parsed = strtoul(text, &end, 10);
  if (errno != 0 || text[0] == '\0' || *end != '\0' || parsed > UINT16_MAX) {
    return false;
  }
  *value = (uint16_t)parsed;
  return true;
}

static bool leme_config_parse_layout_kind(const char *text,
                                          enum leme_layout_kind *kind) {
  if (strcmp(text, "dwindle") == 0) {
    *kind = LEME_LAYOUT_DWINDLE;
  } else if (strcmp(text, "master_stack") == 0) {
    *kind = LEME_LAYOUT_MASTER_STACK;
  } else if (strcmp(text, "accordion") == 0) {
    *kind = LEME_LAYOUT_ACCORDION;
  } else {
    return false;
  }
  return true;
}

static bool
leme_config_parse_tag_rule(struct leme_config *config,
                           const struct leme_scfg_directive *directive,
                           const char *path, char **error);

static bool
leme_config_parse_window_rule(struct leme_config *config,
                              const struct leme_scfg_directive *directive,
                              const char *path, char **error);

static bool
leme_config_parse_scratchpad(struct leme_config *config,
                             const struct leme_scfg_directive *directive,
                             const char *path, char **error);

static bool leme_config_parse_boolean(const char *text, bool *value);

static bool leme_config_parse_decimal(const char *text, double *value);

static bool leme_config_parse_nonnegative(const char *text, int *value) {
  char *end;
  long parsed;

  errno = 0;
  parsed = strtol(text, &end, 10);
  if (errno != 0 || text[0] == '\0' || *end != '\0' || parsed < 0 ||
      parsed > INT_MAX) {
    return false;
  }
  *value = (int)parsed;
  return true;
}

static bool leme_config_parse_tags(struct leme_config *config,
                                   const struct leme_scfg_directive *directive,
                                   const char *path, char **error) {
  static const char *const tags_keys[] = {
      "initial", "maximum", "drop_mode", "layout", "tag",
  };
  const struct leme_scfg_directive *dir_initial = NULL;
  const struct leme_scfg_directive *dir_maximum = NULL;
  const struct leme_scfg_directive *dir_drop_mode = NULL;
  const struct leme_scfg_directive *dir_layout = NULL;
  size_t index;

  if (directive->params_len != 0) {
    leme_config_set_error(error, "%s:%d: tags takes no arguments", path,
                          directive->lineno);
    return false;
  }
  for (index = 0; index < directive->children.directives_len; index++) {
    const struct leme_scfg_directive *entry =
        &directive->children.directives[index];
    uint16_t value;

    if (strcmp(entry->name, "initial") == 0) {
      if (dir_initial != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_initial,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(
                config, entry, -1, &extra,
                "duplicate directive `initial` in `tags`")) {
          return false;
        }
        continue;
      }
      if (entry->params_len != 1 || entry->children.directives_len != 0 ||
          !leme_config_parse_u16(entry->params[0], &value)) {
        if (!leme_config_reject(config, entry, 0,
                                "initial requires one positive integer")) {
          return false;
        }
        continue;
      }
      config->initial_tags = value;
      dir_initial = entry;
    } else if (strcmp(entry->name, "maximum") == 0) {
      if (dir_maximum != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_maximum,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(
                config, entry, -1, &extra,
                "duplicate directive `maximum` in `tags`")) {
          return false;
        }
        continue;
      }
      if (entry->params_len != 1 || entry->children.directives_len != 0 ||
          !leme_config_parse_u16(entry->params[0], &value)) {
        if (!leme_config_reject(config, entry, 0,
                                "maximum requires one positive integer")) {
          return false;
        }
        continue;
      }
      if (value > 64) {
        if (!leme_config_reject(config, entry, 0, "maximum cannot exceed 64")) {
          return false;
        }
        continue;
      }
      config->max_tags = value;
      dir_maximum = entry;
    } else if (strcmp(entry->name, "drop_mode") == 0) {
      if (dir_drop_mode != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_drop_mode,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(
                config, entry, -1, &extra,
                "duplicate directive `drop_mode` in `tags`")) {
          return false;
        }
        continue;
      }
      if (entry->params_len != 1 || entry->children.directives_len != 0) {
        const struct leme_reject_extra extra = {
            .help = "valid values are `simple` and `edges`",
        };
        if (!leme_config_reject_detailed(config, entry, 0, &extra,
                                         "drop_mode must be simple or edges")) {
          return false;
        }
        continue;
      }
      if (strcmp(entry->params[0], "simple") == 0) {
        config->drop_mode = LEME_DROP_MODE_SIMPLE;
      } else if (strcmp(entry->params[0], "edges") == 0) {
        config->drop_mode = LEME_DROP_MODE_EDGES;
      } else {
        const struct leme_reject_extra extra = {
            .help = "valid values are `simple` and `edges`",
        };
        if (!leme_config_reject_detailed(config, entry, 0, &extra,
                                         "drop_mode must be simple or edges")) {
          return false;
        }
        continue;
      }
      config->tag_defaults.drop_mode = config->drop_mode;
      dir_drop_mode = entry;
    } else if (strcmp(entry->name, "layout") == 0) {
      if (dir_layout != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_layout,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(
                config, entry, -1, &extra,
                "duplicate directive `layout` in `tags`")) {
          return false;
        }
        continue;
      }
      if (entry->params_len != 1 || entry->children.directives_len != 0 ||
          !leme_config_parse_layout_kind(entry->params[0],
                                         &config->tag_defaults.layout)) {
        const struct leme_reject_extra extra = {
            .help =
                "valid values are `dwindle`, `master_stack`, and `accordion`",
        };
        if (!leme_config_reject_detailed(
                config, entry, 0, &extra,
                "layout must be dwindle, master_stack, "
                "or accordion")) {
          return false;
        }
        continue;
      }
      dir_layout = entry;
    } else if (strcmp(entry->name, "tag") == 0) {
      if (dir_maximum == NULL) {
        if (!leme_config_reject(config, entry, -1,
                                "tag requires maximum first")) {
          return false;
        }
        continue;
      }
      if (!leme_config_parse_tag_rule(config, entry, path, error)) {
        return false;
      }
    } else {
      const char *nearest = leme_config_nearest_key(
          entry->name, tags_keys, sizeof(tags_keys) / sizeof(tags_keys[0]));
      char help_buf[128] = {0};
      struct leme_reject_extra extra = {0};

      if (nearest != NULL) {
        snprintf(help_buf, sizeof(help_buf),
                 "a directive with a similar name exists: `%s`", nearest);
        extra.help = help_buf;
      }
      if (!leme_config_reject_detailed(config, entry, -1, &extra,
                                       "unknown directive `%s` in `tags`",
                                       entry->name)) {
        return false;
      }
    }
  }
  return true;
}

static bool leme_config_parse_color(const char *text, float value[4]) {
  unsigned int channel[4] = {0, 0, 0, 255};
  size_t length;
  size_t index;

  if (text == NULL || text[0] != '#') {
    return false;
  }
  length = strlen(text + 1);
  if (length != 6 && length != 8) {
    return false;
  }
  for (index = 0; index < length; index++) {
    if (!isxdigit((unsigned char)text[index + 1])) {
      return false;
    }
  }
  for (index = 0; index < length / 2; index++) {
    char pair[3] = {text[index * 2 + 1], text[index * 2 + 2], '\0'};

    channel[index] = (unsigned int)strtoul(pair, NULL, 16);
  }
  for (index = 0; index < 4; index++) {
    value[index] = (float)channel[index] / 255.0f;
  }
  for (index = 0; index < 3; index++) {
    value[index] *= value[3];
  }
  return true;
}

static bool leme_config_parse_opacity(const char *text, double *value) {
  char *end;
  double parsed;

  errno = 0;
  parsed = strtod(text, &end);
  if (errno != 0 || text[0] == '\0' || *end != '\0' || !isfinite(parsed) ||
      parsed < 0.0 || parsed > 1.0) {
    return false;
  }
  *value = parsed;
  return true;
}

static bool leme_config_scratchpad_name_valid(const char *name) {
  return name[0] != '\0' && strpbrk(name, " \t\n\r\f\v") == NULL;
}

static void
leme_config_finish_scratchpad(struct leme_scratchpad_config *scratchpad) {
  free(scratchpad->name);
  free(scratchpad->identity);
  leme_config_free_argv(scratchpad->spawn);
  *scratchpad = (struct leme_scratchpad_config){0};
}

static bool
leme_config_scratchpad_exists(const struct leme_config *config,
                              const struct leme_scratchpad_config *scratchpad) {
  size_t index;

  for (index = 0; index < config->scratchpad_count; index++) {
    const struct leme_scratchpad_config *previous = &config->scratchpads[index];

    if (strcmp(previous->name, scratchpad->name) == 0 ||
        strcmp(previous->identity, scratchpad->identity) == 0) {
      return true;
    }
  }
  return false;
}

static bool
leme_config_parse_scratchpad(struct leme_config *config,
                             const struct leme_scfg_directive *directive,
                             const char *path, char **error) {
  static const char *const scratchpad_keys[] = {
      "identity",
      "spawn",
      "width",
      "height",
  };
  struct leme_scratchpad_config scratchpad = {
      .width = 0.6,
      .height = 0.6,
      .line = directive->lineno,
  };
  const struct leme_scfg_directive *dir_identity = NULL;
  const struct leme_scfg_directive *dir_spawn = NULL;
  const struct leme_scfg_directive *dir_width = NULL;
  const struct leme_scfg_directive *dir_height = NULL;
  bool complete = true;
  size_t index;

  if (directive->params_len != 1 ||
      !leme_config_scratchpad_name_valid(
          directive->params_len == 1 ? directive->params[0] : "")) {
    return leme_config_reject(
        config, directive, -1,
        "scratchpad requires one nonempty name without whitespace");
  }
  scratchpad.name = strdup(directive->params[0]);
  if (scratchpad.name == NULL) {
    leme_config_set_error(error, "%s:%d: out of memory", path,
                          directive->lineno);
    return false;
  }
  for (index = 0; index < directive->children.directives_len; index++) {
    const struct leme_scfg_directive *entry =
        &directive->children.directives[index];
    double dimension;

    if (strcmp(entry->name, "identity") == 0) {
      if (dir_identity != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_identity,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(
                config, entry, -1, &extra,
                "duplicate scratchpad property identity")) {
          leme_config_finish_scratchpad(&scratchpad);
          return false;
        }
        complete = false;
        continue;
      }
      if (entry->params_len != 1 || entry->children.directives_len != 0 ||
          entry->params[0][0] == '\0') {
        if (!leme_config_reject(config, entry, 0,
                                "identity requires one nonempty value")) {
          leme_config_finish_scratchpad(&scratchpad);
          return false;
        }
        complete = false;
        continue;
      }
      scratchpad.identity = strdup(entry->params[0]);
      if (scratchpad.identity == NULL) {
        leme_config_finish_scratchpad(&scratchpad);
        leme_config_set_error(error, "%s:%d: out of memory", path,
                              entry->lineno);
        return false;
      }
      dir_identity = entry;
    } else if (strcmp(entry->name, "spawn") == 0) {
      if (dir_spawn != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_spawn,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(
                config, entry, -1, &extra,
                "duplicate scratchpad property spawn")) {
          leme_config_finish_scratchpad(&scratchpad);
          return false;
        }
        complete = false;
        continue;
      }
      if (entry->params_len == 0 || entry->params[0][0] == '\0' ||
          entry->children.directives_len != 0) {
        if (!leme_config_reject(config, entry, 0,
                                "spawn requires a nonempty command")) {
          leme_config_finish_scratchpad(&scratchpad);
          return false;
        }
        complete = false;
        continue;
      }
      scratchpad.spawn = leme_config_copy_argv(
          entry->params[0], entry->params + 1, entry->params_len - 1);
      if (scratchpad.spawn == NULL) {
        leme_config_finish_scratchpad(&scratchpad);
        leme_config_set_error(error, "%s:%d: out of memory", path,
                              entry->lineno);
        return false;
      }
      dir_spawn = entry;
    } else if (strcmp(entry->name, "width") == 0 ||
               strcmp(entry->name, "height") == 0) {
      const bool width = strcmp(entry->name, "width") == 0;
      const struct leme_scfg_directive *dir_dim =
          width ? dir_width : dir_height;

      if (dir_dim != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_dim,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(config, entry, -1, &extra,
                                         "duplicate scratchpad property %s",
                                         entry->name)) {
          leme_config_finish_scratchpad(&scratchpad);
          return false;
        }
        complete = false;
        continue;
      }
      if (entry->params_len != 1 || entry->children.directives_len != 0 ||
          !leme_config_parse_decimal(entry->params[0], &dimension) ||
          dimension < 0.1 || dimension > 1.0) {
        if (!leme_config_reject(
                config, entry, 0,
                "%s requires a finite decimal from 0.1 through 1.0",
                entry->name)) {
          leme_config_finish_scratchpad(&scratchpad);
          return false;
        }
        complete = false;
        continue;
      }
      if (width) {
        scratchpad.width = dimension;
        dir_width = entry;
      } else {
        scratchpad.height = dimension;
        dir_height = entry;
      }
    } else {
      const char *nearest = leme_config_nearest_key(
          entry->name, scratchpad_keys,
          sizeof(scratchpad_keys) / sizeof(scratchpad_keys[0]));
      char help_buf[128] = {0};
      struct leme_reject_extra extra = {0};

      if (nearest != NULL) {
        snprintf(help_buf, sizeof(help_buf),
                 "a directive with a similar name exists: `%s`", nearest);
        extra.help = help_buf;
      }
      if (!leme_config_reject_detailed(config, entry, -1, &extra,
                                       "unknown scratchpad property %s",
                                       entry->name)) {
        leme_config_finish_scratchpad(&scratchpad);
        return false;
      }
      complete = false;
    }
  }
  if (dir_identity == NULL || dir_spawn == NULL) {
    if (!leme_config_reject(config, directive, -1,
                            "scratchpad requires identity and spawn")) {
      leme_config_finish_scratchpad(&scratchpad);
      return false;
    }
    complete = false;
  }
  if (complete && leme_config_scratchpad_exists(config, &scratchpad)) {
    if (!leme_config_reject(config, directive, -1,
                            "duplicate scratchpad name or identity %s",
                            scratchpad.name)) {
      leme_config_finish_scratchpad(&scratchpad);
      return false;
    }
    complete = false;
  }
  if (!complete) {
    leme_config_finish_scratchpad(&scratchpad);
    return true;
  }
  if (config->scratchpad_count == SIZE_MAX / sizeof(*config->scratchpads)) {
    leme_config_finish_scratchpad(&scratchpad);
    leme_config_set_error(error, "%s:%d: too many scratchpads", path,
                          directive->lineno);
    return false;
  }
  {
    struct leme_scratchpad_config *scratchpads =
        realloc(config->scratchpads,
                (config->scratchpad_count + 1) * sizeof(*scratchpads));

    if (scratchpads == NULL) {
      leme_config_finish_scratchpad(&scratchpad);
      leme_config_set_error(error, "%s:%d: out of memory", path,
                            directive->lineno);
      return false;
    }
    config->scratchpads = scratchpads;
  }
  config->scratchpads[config->scratchpad_count++] = scratchpad;
  return true;
}

static void leme_config_free_window_identities(char **identities,
                                               size_t count) {
  size_t index;

  for (index = 0; index < count; index++) {
    free(identities[index]);
  }
  free(identities);
}

static bool
leme_config_parse_window_rule(struct leme_config *config,
                              const struct leme_scfg_directive *directive,
                              const char *path, char **error) {
  struct leme_window_rule *rules;
  struct leme_window_rule *rule;
  char **identities;
  size_t index;

  if (directive->params_len == 0) {
    return leme_config_reject(
        config, directive, -1,
        "window requires at least one application identity");
  }
  if (directive->params_len > SIZE_MAX / sizeof(*identities)) {
    leme_config_set_error(error, "%s:%d: too many window identities", path,
                          directive->lineno);
    return false;
  }
  identities = calloc(directive->params_len, sizeof(*identities));
  if (identities == NULL) {
    leme_config_set_error(error, "%s:%d: out of memory", path,
                          directive->lineno);
    return false;
  }
  for (index = 0; index < directive->params_len; index++) {
    identities[index] = strdup(directive->params[index]);
    if (identities[index] == NULL) {
      leme_config_free_window_identities(identities, index);
      leme_config_set_error(error, "%s:%d: out of memory", path,
                            directive->lineno);
      return false;
    }
  }
  rules = realloc(config->window_rules,
                  (config->window_rule_count + 1) * sizeof(*rules));
  if (rules == NULL) {
    leme_config_free_window_identities(identities, directive->params_len);
    leme_config_set_error(error, "%s:%d: out of memory", path,
                          directive->lineno);
    return false;
  }
  config->window_rules = rules;
  rule = &config->window_rules[config->window_rule_count];
  *rule = (struct leme_window_rule){
      .identities = identities,
      .identity_count = directive->params_len,
  };
  config->window_rule_count++;

  static const char *const window_keys[] = {
      "title", "tag", "floating", "fullscreen", "output", "opacity",
  };
  const struct leme_scfg_directive *dir_title = NULL;
  const struct leme_scfg_directive *dir_tag = NULL;
  const struct leme_scfg_directive *dir_floating = NULL;
  const struct leme_scfg_directive *dir_fullscreen = NULL;
  const struct leme_scfg_directive *dir_output = NULL;
  const struct leme_scfg_directive *dir_opacity = NULL;

  for (index = 0; index < directive->children.directives_len; index++) {
    const struct leme_scfg_directive *entry =
        &directive->children.directives[index];
    int value;
    double decimal;
    bool flag;

    if (entry->params_len != 1 || entry->children.directives_len != 0) {
      if (!leme_config_reject(config, entry, -1, "%s requires one value",
                              entry->name)) {
        return false;
      }
      continue;
    }
    if (strcmp(entry->name, "title") == 0) {
      if (dir_title != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_title,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(config, entry, -1, &extra,
                                         "duplicate window property title")) {
          return false;
        }
        continue;
      }
      rule->title = strdup(entry->params[0]);
      if (rule->title == NULL) {
        leme_config_set_error(error, "%s:%d: out of memory", path,
                              entry->lineno);
        return false;
      }
      dir_title = entry;
    } else if (strcmp(entry->name, "tag") == 0) {
      if (dir_tag != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_tag,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(config, entry, -1, &extra,
                                         "duplicate window property tag")) {
          return false;
        }
        continue;
      }
      if (!leme_config_parse_nonnegative(entry->params[0], &value) ||
          value < 1 || value > UINT16_MAX) {
        if (!leme_config_reject(config, entry, 0,
                                "tag requires a positive integer")) {
          return false;
        }
        continue;
      }
      rule->tag_id = (uint16_t)value;
      rule->fields |= LEME_WINDOW_RULE_TAG;
      dir_tag = entry;
    } else if (strcmp(entry->name, "floating") == 0) {
      if (dir_floating != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_floating,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(
                config, entry, -1, &extra,
                "duplicate window property floating")) {
          return false;
        }
        continue;
      }
      if (!leme_config_parse_boolean(entry->params[0], &flag)) {
        if (!leme_config_reject(config, entry, 0,
                                "floating expects true or false")) {
          return false;
        }
        continue;
      }
      rule->floating = flag;
      rule->fields |= LEME_WINDOW_RULE_FLOATING;
      dir_floating = entry;
    } else if (strcmp(entry->name, "fullscreen") == 0) {
      if (dir_fullscreen != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_fullscreen,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(
                config, entry, -1, &extra,
                "duplicate window property fullscreen")) {
          return false;
        }
        continue;
      }
      if (!leme_config_parse_boolean(entry->params[0], &flag)) {
        if (!leme_config_reject(config, entry, 0,
                                "fullscreen expects true or false")) {
          return false;
        }
        continue;
      }
      rule->fullscreen = flag;
      rule->fields |= LEME_WINDOW_RULE_FULLSCREEN;
      dir_fullscreen = entry;
    } else if (strcmp(entry->name, "output") == 0) {
      if (dir_output != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_output,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(config, entry, -1, &extra,
                                         "duplicate window property output")) {
          return false;
        }
        continue;
      }
      rule->output = strdup(entry->params[0]);
      if (rule->output == NULL) {
        leme_config_set_error(error, "%s:%d: out of memory", path,
                              entry->lineno);
        return false;
      }
      rule->fields |= LEME_WINDOW_RULE_OUTPUT;
      dir_output = entry;
    } else if (strcmp(entry->name, "opacity") == 0) {
      if (dir_opacity != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_opacity,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(config, entry, -1, &extra,
                                         "duplicate window property opacity")) {
          return false;
        }
        continue;
      }
      if (!leme_config_parse_opacity(entry->params[0], &decimal)) {
        if (!leme_config_reject(
                config, entry, 0,
                "opacity expects a decimal from 0.0 through 1.0")) {
          return false;
        }
        continue;
      }
      rule->opacity = decimal;
      rule->fields |= LEME_WINDOW_RULE_OPACITY;
      dir_opacity = entry;
    } else {
      const char *nearest =
          leme_config_nearest_key(entry->name, window_keys,
                                  sizeof(window_keys) / sizeof(window_keys[0]));
      char help_buf[128] = {0};
      struct leme_reject_extra extra = {0};

      if (nearest != NULL) {
        snprintf(help_buf, sizeof(help_buf),
                 "a directive with a similar name exists: `%s`", nearest);
        extra.help = help_buf;
      }
      if (!leme_config_reject_detailed(config, entry, -1, &extra,
                                       "unknown directive `%s` in `window`",
                                       entry->name)) {
        return false;
      }
    }
  }
  return true;
}

#define LEME_ANIMATION_DURATION_MAX 1000u

static bool leme_config_parse_decimal(const char *text, double *value) {
  char *end;
  double parsed;

  errno = 0;
  parsed = strtod(text, &end);
  if (errno != 0 || text[0] == '\0' || *end != '\0' || !isfinite(parsed)) {
    return false;
  }
  *value = parsed;
  return true;
}

static bool
leme_config_parse_animation_curve(const struct leme_scfg_directive *entry,
                                  struct leme_animation_curve *curve) {
  double points[4];
  size_t index;

  if (entry->params_len == 1) {
    const char *text = entry->params[0];

    if (strcmp(text, "linear") == 0) {
      *curve = leme_animation_curve_preset(LEME_ANIMATION_CURVE_LINEAR);
    } else if (strcmp(text, "ease_in") == 0) {
      *curve = leme_animation_curve_preset(LEME_ANIMATION_CURVE_EASE_IN);
    } else if (strcmp(text, "ease_out") == 0) {
      *curve = leme_animation_curve_preset(LEME_ANIMATION_CURVE_EASE_OUT);
    } else if (strcmp(text, "ease_in_out") == 0) {
      *curve = leme_animation_curve_preset(LEME_ANIMATION_CURVE_EASE_IN_OUT);
    } else {
      return false;
    }
    return true;
  }
  if (entry->params_len != 4) {
    return false;
  }
  for (index = 0; index < LEME_ARRAY_LENGTH(points); index++) {
    if (!leme_config_parse_decimal(entry->params[index], &points[index])) {
      return false;
    }
  }
  if (points[0] < 0.0 || points[0] > 1.0 || points[2] < 0.0 ||
      points[2] > 1.0) {
    return false;
  }
  *curve = (struct leme_animation_curve){
      points[0],
      points[1],
      points[2],
      points[3],
  };
  return true;
}

static bool
leme_config_parse_animation_effects(const struct leme_scfg_directive *entry,
                                    uint32_t *effects) {
  uint32_t parsed = 0;
  size_t index;

  for (index = 0; index < entry->params_len; index++) {
    const char *name = entry->params[index];

    if (strcmp(name, "fade") == 0) {
      parsed |= (uint32_t)LEME_ANIMATION_EFFECT_FADE;
    } else if (strcmp(name, "scale") == 0) {
      parsed |= (uint32_t)LEME_ANIMATION_EFFECT_SCALE;
    } else if (strcmp(name, "none") == 0 && entry->params_len == 1) {
      parsed = 0;
    } else {
      return false;
    }
  }
  *effects = parsed;
  return true;
}

static bool leme_config_parse_spring(struct leme_config *config,
                                     const struct leme_scfg_directive *entry,
                                     enum leme_animation_kind *kind,
                                     struct leme_animation_spring *out) {
  struct leme_animation_spring spring = {
      .damping_ratio = 1.0,
      .stiffness = 800.0,
      .epsilon = 0.0001,
  };
  const struct leme_scfg_directive *seen[3] = {NULL, NULL, NULL};
  static const char *const spring_keys[] = {
      "damping_ratio",
      "stiffness",
      "epsilon",
  };
  uint32_t settle;
  size_t child;
  bool ok = true;

  if (entry->params_len != 0) {
    if (!leme_config_reject(config, entry, 0, "spring takes no arguments")) {
      return false;
    }
    ok = false;
  }
  if (entry->children.directives_len == 0) {
    const struct leme_reject_extra extra = {
        .help = "a spring needs damping_ratio, stiffness or epsilon",
    };
    if (!leme_config_reject_detailed(config, entry, -1, &extra,
                                     "spring requires a block")) {
      return false;
    }
    return true;
  }
  for (child = 0; child < entry->children.directives_len; child++) {
    const struct leme_scfg_directive *field =
        &entry->children.directives[child];
    double value;
    size_t key;

    for (key = 0; key < LEME_ARRAY_LENGTH(spring_keys); key++) {
      if (strcmp(field->name, spring_keys[key]) == 0) {
        break;
      }
    }
    if (key == LEME_ARRAY_LENGTH(spring_keys)) {
      if (!leme_config_reject(config, field, -1,
                              "unknown directive `%s` in `spring`",
                              field->name)) {
        return false;
      }
      ok = false;
      continue;
    }
    if (seen[key] != NULL) {
      const struct leme_reject_extra extra = {
          .secondary = seen[key],
          .secondary_label = "first defined here",
      };
      if (!leme_config_reject_detailed(config, field, -1, &extra,
                                       "duplicate directive `%s` in `spring`",
                                       field->name)) {
        return false;
      }
      ok = false;
      continue;
    }
    if (field->params_len != 1 ||
        !leme_config_parse_decimal(field->params[0], &value)) {
      if (!leme_config_reject(config, field, -1, "%s expects one decimal",
                              field->name)) {
        return false;
      }
      ok = false;
      continue;
    }
    seen[key] = field;
    if (key == 0) {
      spring.damping_ratio = value;
    } else if (key == 1) {
      spring.stiffness = value;
    } else {
      spring.epsilon = value;
    }
  }
  if (ok && !(spring.damping_ratio > 0.0)) {
    const struct leme_reject_extra extra = {
        .help = "a spring with no damping never settles",
    };
    if (!leme_config_reject_detailed(
            config, seen[0] != NULL ? seen[0] : entry, seen[0] != NULL ? 0 : -1,
            &extra, "damping_ratio must be greater than zero")) {
      return false;
    }
    ok = false;
  }
  if (ok && !(spring.stiffness > 0.0)) {
    if (!leme_config_reject(config, seen[1] != NULL ? seen[1] : entry,
                            seen[1] != NULL ? 0 : -1,
                            "stiffness must be greater than zero")) {
      return false;
    }
    ok = false;
  }
  if (ok && !(spring.epsilon > 0.0 && spring.epsilon < 1.0)) {
    const struct leme_reject_extra extra = {
        .help = "epsilon is an amplitude, and must sit between 0 and 1",
    };
    if (!leme_config_reject_detailed(config, seen[2] != NULL ? seen[2] : entry,
                                     seen[2] != NULL ? 0 : -1, &extra,
                                     "epsilon must be above 0 and below 1")) {
      return false;
    }
    ok = false;
  }
  if (ok && spring.epsilon < 0.000001) {
    spring.epsilon = 0.000001;
  }
  if (!ok) {
    return true;
  }
  settle = leme_animation_spring_duration_ms(&spring, 0.0);
  if (settle == 0) {
    if (!leme_config_reject(config, entry, -1,
                            "these spring parameters never settle")) {
      return false;
    }
    return true;
  }
  if (settle > LEME_ANIMATION_DURATION_MAX) {
    char help[96] = {0};
    struct leme_reject_extra extra = {0};

    if (snprintf(help, sizeof(help),
                 "these parameters settle in %ums; raise damping_ratio or "
                 "stiffness",
                 settle) > 0) {
      extra.help = help;
    }
    if (!leme_config_reject_detailed(config, entry, -1, &extra,
                                     "a spring may not settle slower than %ums",
                                     LEME_ANIMATION_DURATION_MAX)) {
      return false;
    }
    return true;
  }
  *kind = LEME_ANIMATION_KIND_SPRING;
  *out = spring;
  return true;
}

static bool
leme_config_parse_animation_event(struct leme_config *config,
                                  const struct leme_scfg_directive *directive,
                                  struct leme_animation_settings *settings) {
  static const char *const animation_keys[] = {
      "effect", "curve", "opacity_curve", "duration", "scale_from", "spring",
  };
  const struct leme_scfg_directive *dir_duration = NULL;
  const struct leme_scfg_directive *dir_curve = NULL;
  const struct leme_scfg_directive *dir_opacity_curve = NULL;
  const struct leme_scfg_directive *dir_effect = NULL;
  const struct leme_scfg_directive *dir_scale_from = NULL;
  const struct leme_scfg_directive *dir_spring = NULL;
  size_t index;

  settings->duration_ms = 150;
  settings->curve = leme_animation_curve_preset(LEME_ANIMATION_CURVE_EASE_OUT);
  settings->opacity_curve = settings->curve;
  settings->effects = LEME_ANIMATION_EFFECT_FADE;
  settings->scale_from = 0.92;
  settings->kind = LEME_ANIMATION_KIND_EASING;
  settings->spring = (struct leme_animation_spring){0};
  for (index = 0; index < directive->children.directives_len; index++) {
    const struct leme_scfg_directive *entry =
        &directive->children.directives[index];
    struct leme_animation_curve curve;
    uint32_t effects;
    double decimal;
    int value;

    if (strcmp(entry->name, "spring") == 0) {
      if (dir_spring != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_spring,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(
                config, entry, -1, &extra,
                "duplicate directive `spring` in animation event")) {
          return false;
        }
        continue;
      }
      dir_spring = entry;
      if (!leme_config_parse_spring(config, entry, &settings->kind,
                                    &settings->spring)) {
        return false;
      }
      continue;
    }

    if (entry->children.directives_len != 0) {
      if (!leme_config_reject(config, entry, -1, "%s takes no block",
                              entry->name)) {
        return false;
      }
      continue;
    }
    if (strcmp(entry->name, "effect") == 0) {
      if (dir_effect != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_effect,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(config, entry, -1, &extra,
                                         "duplicate animation key effect")) {
          return false;
        }
        continue;
      }
      if (entry->params_len == 0 ||
          !leme_config_parse_animation_effects(entry, &effects)) {
        if (!leme_config_reject(config, entry, 0,
                                "effect expects fade, scale, or none")) {
          return false;
        }
        continue;
      }
      settings->effects = effects;
      dir_effect = entry;
      continue;
    }
    if (strcmp(entry->name, "curve") == 0 ||
        strcmp(entry->name, "opacity_curve") == 0) {
      bool opacity = strcmp(entry->name, "opacity_curve") == 0;
      const struct leme_scfg_directive *dir_curv =
          opacity ? dir_opacity_curve : dir_curve;

      if (dir_curv != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_curv,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(config, entry, -1, &extra,
                                         "duplicate animation key %s",
                                         entry->name)) {
          return false;
        }
        continue;
      }
      if (!leme_config_parse_animation_curve(entry, &curve)) {
        const struct leme_reject_extra extra = {
            .help = "valid presets are `linear`, `ease_in`, `ease_out`, and "
                    "`ease_in_out`",
        };
        if (!leme_config_reject_detailed(
                config, entry, 0, &extra,
                "%s expects a preset name or four decimals", entry->name)) {
          return false;
        }
        continue;
      }
      if (opacity) {
        settings->opacity_curve = curve;
        dir_opacity_curve = entry;
      } else {
        settings->curve = curve;
        dir_curve = entry;
      }
      continue;
    }
    if (entry->params_len != 1) {
      if (!leme_config_reject(config, entry, -1, "%s requires one value",
                              entry->name)) {
        return false;
      }
      continue;
    }
    if (strcmp(entry->name, "duration") == 0) {
      if (dir_duration != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_duration,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(config, entry, -1, &extra,
                                         "duplicate animation key duration")) {
          return false;
        }
        continue;
      }
      if (!leme_config_parse_nonnegative(entry->params[0], &value)) {
        if (!leme_config_reject(config, entry, 0,
                                "duration requires one nonnegative integer")) {
          return false;
        }
        continue;
      }
      if ((uint32_t)value > LEME_ANIMATION_DURATION_MAX) {
        if (!leme_config_reject(config, entry, 0,
                                "duration is clamped to %u milliseconds",
                                LEME_ANIMATION_DURATION_MAX)) {
          return false;
        }
        value = (int)LEME_ANIMATION_DURATION_MAX;
      }
      settings->duration_ms = (uint32_t)value;
      dir_duration = entry;
    } else if (strcmp(entry->name, "scale_from") == 0) {
      if (dir_scale_from != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_scale_from,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(
                config, entry, -1, &extra,
                "duplicate animation key scale_from")) {
          return false;
        }
        continue;
      }
      if (!leme_config_parse_decimal(entry->params[0], &decimal) ||
          decimal < 0.1 || decimal > 2.0) {
        if (!leme_config_reject(
                config, entry, 0,
                "scale_from expects a decimal from 0.1 through 2.0")) {
          return false;
        }
        continue;
      }
      settings->scale_from = decimal;
      dir_scale_from = entry;
    } else {
      const char *nearest = leme_config_nearest_key(
          entry->name, animation_keys,
          sizeof(animation_keys) / sizeof(animation_keys[0]));
      char help_buf[128] = {0};
      struct leme_reject_extra extra = {0};

      if (nearest != NULL) {
        snprintf(help_buf, sizeof(help_buf),
                 "a directive with a similar name exists: `%s`", nearest);
        extra.help = help_buf;
      }
      if (!leme_config_reject_detailed(config, entry, -1, &extra,
                                       "unknown directive `%s` in `animation`",
                                       entry->name)) {
        return false;
      }
    }
  }
  if (dir_opacity_curve == NULL) {
    settings->opacity_curve = settings->curve;
  }
  settings->configured = (settings->kind == LEME_ANIMATION_KIND_SPRING ||
                          settings->duration_ms > 0) &&
                         settings->effects != 0;
  return true;
}

static bool leme_config_parse_workspace_animation(
    struct leme_config *config, const struct leme_scfg_directive *directive,
    struct leme_workspace_animation_settings *settings) {
  static const char *const workspace_animation_keys[] = {
      "curve", "duration", "style", "distance", "spring",
  };
  const struct leme_scfg_directive *dir_spring = NULL;
  const struct leme_scfg_directive *dir_duration = NULL;
  const struct leme_scfg_directive *dir_style = NULL;
  const struct leme_scfg_directive *dir_distance = NULL;
  const struct leme_scfg_directive *dir_curve = NULL;
  size_t index;

  *settings = (struct leme_workspace_animation_settings){
      .configured = true,
      .duration_ms = 180,
      .style = LEME_WORKSPACE_ANIMATION_GLIDE_FADE,
      .distance = 0.15,
      .curve = leme_animation_curve_preset(LEME_ANIMATION_CURVE_EASE_OUT),
  };
  for (index = 0; index < directive->children.directives_len; index++) {
    const struct leme_scfg_directive *entry =
        &directive->children.directives[index];
    struct leme_animation_curve curve;
    double decimal;
    int value;

    if (strcmp(entry->name, "spring") == 0) {
      if (dir_spring != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_spring,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(
                config, entry, -1, &extra,
                "duplicate workspace animation key spring")) {
          return false;
        }
        continue;
      }
      dir_spring = entry;
      if (!leme_config_parse_spring(config, entry, &settings->kind,
                                    &settings->spring)) {
        return false;
      }
      continue;
    }
    if (entry->children.directives_len != 0) {
      if (!leme_config_reject(config, entry, -1, "%s takes no block",
                              entry->name)) {
        return false;
      }
      continue;
    }
    if (strcmp(entry->name, "opacity_curve") == 0) {
      if (!leme_config_reject(
              config, entry, -1,
              "opacity_curve is not supported in workspace animation; opacity follows transition travel")) {
        return false;
      }
      continue;
    }
    if (strcmp(entry->name, "curve") == 0) {
      if (dir_curve != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_curve,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(
                config, entry, -1, &extra,
                "duplicate workspace animation key curve")) {
          return false;
        }
        continue;
      }
      if (!leme_config_parse_animation_curve(entry, &curve)) {
        const struct leme_reject_extra extra = {
            .help = "valid presets are `linear`, `ease_in`, `ease_out`, and "
                    "`ease_in_out`",
        };
        if (!leme_config_reject_detailed(
                config, entry, 0, &extra,
                "%s expects a preset name or four decimals", entry->name)) {
          return false;
        }
        continue;
      }
      settings->curve = curve;
      dir_curve = entry;
      continue;
    }
    if (entry->params_len != 1) {
      if (!leme_config_reject(config, entry, -1, "%s requires one value",
                              entry->name)) {
        return false;
      }
      continue;
    }
    if (strcmp(entry->name, "duration") == 0) {
      if (dir_duration != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_duration,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(
                config, entry, -1, &extra,
                "duplicate workspace animation key duration")) {
          return false;
        }
        continue;
      }
      if (!leme_config_parse_nonnegative(entry->params[0], &value)) {
        if (!leme_config_reject(config, entry, 0,
                                "duration requires one nonnegative integer")) {
          return false;
        }
        continue;
      }
      if ((uint32_t)value > LEME_ANIMATION_DURATION_MAX) {
        if (!leme_config_reject(config, entry, 0,
                                "duration is clamped to %u milliseconds",
                                LEME_ANIMATION_DURATION_MAX)) {
          return false;
        }
        value = (int)LEME_ANIMATION_DURATION_MAX;
      }
      settings->duration_ms = (uint32_t)value;
      dir_duration = entry;
    } else if (strcmp(entry->name, "style") == 0) {
      if (dir_style != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_style,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(
                config, entry, -1, &extra,
                "duplicate workspace animation key style")) {
          return false;
        }
        continue;
      }
      if (strcmp(entry->params[0], "full_slide") == 0) {
        settings->style = LEME_WORKSPACE_ANIMATION_FULL_SLIDE;
      } else if (strcmp(entry->params[0], "glide_fade") == 0) {
        settings->style = LEME_WORKSPACE_ANIMATION_GLIDE_FADE;
      } else {
        const struct leme_reject_extra extra = {
            .help = "valid values are `full_slide` and `glide_fade`",
        };
        if (!leme_config_reject_detailed(
                config, entry, 0, &extra,
                "style expects full_slide or glide_fade")) {
          return false;
        }
        continue;
      }
      dir_style = entry;
    } else if (strcmp(entry->name, "distance") == 0) {
      if (dir_distance != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_distance,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(
                config, entry, -1, &extra,
                "duplicate workspace animation key distance")) {
          return false;
        }
        continue;
      }
      if (!leme_config_parse_decimal(entry->params[0], &decimal) ||
          decimal < 0.0 || decimal > 1.0) {
        if (!leme_config_reject(
                config, entry, 0,
                "distance expects a decimal from 0.0 through 1.0")) {
          return false;
        }
        continue;
      }
      settings->distance = decimal;
      dir_distance = entry;
    } else {
      const char *nearest =
          leme_config_nearest_key(entry->name, workspace_animation_keys,
                                  sizeof(workspace_animation_keys) /
                                      sizeof(workspace_animation_keys[0]));
      char help_buf[128] = {0};
      struct leme_reject_extra extra = {0};

      if (nearest != NULL) {
        snprintf(help_buf, sizeof(help_buf),
                 "a directive with a similar name exists: `%s`", nearest);
        extra.help = help_buf;
      }
      if (!leme_config_reject_detailed(
              config, entry, -1, &extra,
              "unknown directive `%s` in `workspace` animation", entry->name)) {
        return false;
      }
    }
  }
  settings->opacity_curve = settings->curve;
  settings->configured =
      settings->kind == LEME_ANIMATION_KIND_SPRING || settings->duration_ms > 0;
  return true;
}

static bool
leme_config_parse_animation(struct leme_config *config,
                            const struct leme_scfg_directive *directive,
                            const char *path, char **error) {
  static const char *const animation_blocks[] = {
      "workspace",
      "open",
      "close",
  };
  const struct leme_scfg_directive *dir_workspace = NULL;
  const struct leme_scfg_directive *dir_open = NULL;
  const struct leme_scfg_directive *dir_close = NULL;
  size_t index;

  if (directive->params_len != 0) {
    leme_config_set_error(error, "%s:%d: animation takes no arguments", path,
                          directive->lineno);
    return false;
  }
  for (index = 0; index < directive->children.directives_len; index++) {
    const struct leme_scfg_directive *entry =
        &directive->children.directives[index];

    if (strcmp(entry->name, "workspace") == 0) {
      if (dir_workspace != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_workspace,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(
                config, entry, -1, &extra,
                "duplicate workspace block in animation")) {
          return false;
        }
        continue;
      }
      if (entry->params_len != 0) {
        if (!leme_config_reject(config, entry, -1,
                                "malformed workspace block")) {
          return false;
        }
        continue;
      }
      dir_workspace = entry;
      if (!leme_config_parse_workspace_animation(
              config, entry, &config->workspace_animation)) {
        return false;
      }
      continue;
    }
    if (strcmp(entry->name, "open") == 0 || strcmp(entry->name, "close") == 0) {
      const bool is_open = strcmp(entry->name, "open") == 0;
      const struct leme_scfg_directive *dir_evt =
          is_open ? dir_open : dir_close;
      enum leme_animation_event event =
          is_open ? LEME_ANIMATION_OPEN : LEME_ANIMATION_CLOSE;

      if (dir_evt != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_evt,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(config, entry, -1, &extra,
                                         "duplicate %s block in animation",
                                         entry->name)) {
          return false;
        }
        continue;
      }
      if (entry->params_len != 0) {
        if (!leme_config_reject(config, entry, -1, "malformed %s block",
                                entry->name)) {
          return false;
        }
        continue;
      }
      if (is_open) {
        dir_open = entry;
      } else {
        dir_close = entry;
      }
      if (!leme_config_parse_animation_event(config, entry,
                                             &config->animation[event])) {
        return false;
      }
      continue;
    }
    {
      const char *nearest = leme_config_nearest_key(
          entry->name, animation_blocks,
          sizeof(animation_blocks) / sizeof(animation_blocks[0]));
      char help_buf[128] = {0};
      struct leme_reject_extra extra = {0};

      if (nearest != NULL) {
        snprintf(help_buf, sizeof(help_buf),
                 "a directive with a similar name exists: `%s`", nearest);
        extra.help = help_buf;
      }
      if (!leme_config_reject_detailed(config, entry, -1, &extra,
                                       "unknown animation block %s",
                                       entry->name)) {
        return false;
      }
    }
  }
  return true;
}

static bool leme_config_parse_style(struct leme_config *config,
                                    const struct leme_scfg_directive *directive,
                                    const char *path, char **error) {
  static const char *const style_keys[] = {
      "gap",
      "border_width",
      "corner_radius",
      "blur",
      "border_active",
      "border_inactive",
      "opacity_active",
      "opacity_inactive",
      "fullscreen_covers",
  };
  const struct leme_scfg_directive *dir_gap = NULL;
  const struct leme_scfg_directive *dir_border = NULL;
  const struct leme_scfg_directive *dir_corner_radius = NULL;
  const struct leme_scfg_directive *dir_blur = NULL;
  const struct leme_scfg_directive *dir_border_active = NULL;
  const struct leme_scfg_directive *dir_border_inactive = NULL;
  const struct leme_scfg_directive *dir_opacity_active = NULL;
  const struct leme_scfg_directive *dir_opacity_inactive = NULL;
  const struct leme_scfg_directive *dir_fullscreen_covers = NULL;
  size_t index;

  if (directive->params_len != 0) {
    leme_config_set_error(error, "%s:%d: style takes no arguments", path,
                          directive->lineno);
    return false;
  }
  for (index = 0; index < directive->children.directives_len; index++) {
    const struct leme_scfg_directive *entry =
        &directive->children.directives[index];
    int value;
    double decimal;

    if (entry->params_len != 1 || entry->children.directives_len != 0) {
      if (!leme_config_reject(config, entry, -1, "%s requires one value",
                              entry->name)) {
        return false;
      }
      continue;
    }
    if (strcmp(entry->name, "gap") == 0) {
      if (dir_gap != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_gap,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(
                config, entry, -1, &extra,
                "duplicate directive `gap` in `style`")) {
          return false;
        }
        continue;
      }
      if (!leme_config_parse_nonnegative(entry->params[0], &value)) {
        if (!leme_config_reject(config, entry, 0,
                                "gap requires one nonnegative integer")) {
          return false;
        }
        continue;
      }
      config->gap = value;
      dir_gap = entry;
    } else if (strcmp(entry->name, "border_width") == 0) {
      if (dir_border != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_border,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(
                config, entry, -1, &extra,
                "duplicate directive `border_width` in `style`")) {
          return false;
        }
        continue;
      }
      if (!leme_config_parse_nonnegative(entry->params[0], &value)) {
        if (!leme_config_reject(
                config, entry, 0,
                "border_width requires one nonnegative integer")) {
          return false;
        }
        continue;
      }
      config->border_width = value;
      dir_border = entry;
    } else if (strcmp(entry->name, "corner_radius") == 0) {
      if (dir_corner_radius != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_corner_radius,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(
                config, entry, -1, &extra,
                "duplicate directive `corner_radius` in `style`")) {
          return false;
        }
        continue;
      }
      if (!leme_config_parse_nonnegative(entry->params[0], &value)) {
        if (!leme_config_reject(
                config, entry, 0,
                "corner_radius requires one nonnegative integer")) {
          return false;
        }
        continue;
      }
      config->corner_radius = value;
      dir_corner_radius = entry;
    } else if (strcmp(entry->name, "blur") == 0) {
      if (dir_blur != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_blur,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(
                config, entry, -1, &extra,
                "duplicate directive `blur` in `style`")) {
          return false;
        }
        continue;
      }
      if (!leme_config_parse_nonnegative(entry->params[0], &value) ||
          value > 64) {
        if (!leme_config_reject(config, entry, 0,
                                "blur requires an integer from 0 through 64")) {
          return false;
        }
        continue;
      }
      config->blur = value;
      dir_blur = entry;
    } else if (strcmp(entry->name, "border_active") == 0) {
      if (dir_border_active != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_border_active,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(
                config, entry, -1, &extra,
                "duplicate directive `border_active` in `style`")) {
          return false;
        }
        continue;
      }
      if (!leme_config_parse_color(entry->params[0], config->border_active)) {
        if (!leme_config_reject(config, entry, 0,
                                "border_active expects #RRGGBB or #RRGGBBAA")) {
          return false;
        }
        continue;
      }
      dir_border_active = entry;
    } else if (strcmp(entry->name, "border_inactive") == 0) {
      if (dir_border_inactive != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_border_inactive,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(
                config, entry, -1, &extra,
                "duplicate directive `border_inactive` in `style`")) {
          return false;
        }
        continue;
      }
      if (!leme_config_parse_color(entry->params[0], config->border_inactive)) {
        if (!leme_config_reject(
                config, entry, 0,
                "border_inactive expects #RRGGBB or #RRGGBBAA")) {
          return false;
        }
        continue;
      }
      dir_border_inactive = entry;
    } else if (strcmp(entry->name, "opacity_active") == 0) {
      if (dir_opacity_active != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_opacity_active,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(
                config, entry, -1, &extra,
                "duplicate directive `opacity_active` in `style`")) {
          return false;
        }
        continue;
      }
      if (!leme_config_parse_opacity(entry->params[0], &decimal)) {
        if (!leme_config_reject(config, entry, 0,
                                "%s expects a decimal from 0.0 through 1.0",
                                entry->name)) {
          return false;
        }
        continue;
      }
      config->opacity_active = decimal;
      dir_opacity_active = entry;
    } else if (strcmp(entry->name, "opacity_inactive") == 0) {
      if (dir_opacity_inactive != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_opacity_inactive,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(
                config, entry, -1, &extra,
                "duplicate directive `opacity_inactive` in `style`")) {
          return false;
        }
        continue;
      }
      if (!leme_config_parse_opacity(entry->params[0], &decimal)) {
        if (!leme_config_reject(config, entry, 0,
                                "%s expects a decimal from 0.0 through 1.0",
                                entry->name)) {
          return false;
        }
        continue;
      }
      config->opacity_inactive = decimal;
      dir_opacity_inactive = entry;
    } else if (strcmp(entry->name, "fullscreen_covers") == 0) {
      if (dir_fullscreen_covers != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_fullscreen_covers,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(
                config, entry, -1, &extra,
                "duplicate directive `fullscreen_covers` in `style`")) {
          return false;
        }
        continue;
      }
      if (strcmp(entry->params[0], "none") == 0) {
        config->fullscreen_covers = LEME_FULLSCREEN_COVERS_NONE;
      } else if (strcmp(entry->params[0], "top") == 0) {
        config->fullscreen_covers = LEME_FULLSCREEN_COVERS_TOP;
      } else if (strcmp(entry->params[0], "overlay") == 0) {
        config->fullscreen_covers = LEME_FULLSCREEN_COVERS_OVERLAY;
      } else {
        const struct leme_reject_extra extra = {
            .help = "valid values are `none`, `top`, and `overlay`",
        };
        if (!leme_config_reject_detailed(
                config, entry, 0, &extra,
                "fullscreen_covers must be none, top, or overlay")) {
          return false;
        }
        continue;
      }
      dir_fullscreen_covers = entry;
    } else {
      const char *nearest = leme_config_nearest_key(
          entry->name, style_keys, sizeof(style_keys) / sizeof(style_keys[0]));
      char help_buf[128] = {0};
      struct leme_reject_extra extra = {0};

      if (nearest != NULL) {
        snprintf(help_buf, sizeof(help_buf),
                 "a directive with a similar name exists: `%s`", nearest);
        extra.help = help_buf;
      }
      if (!leme_config_reject_detailed(config, entry, -1, &extra,
                                       "unknown directive `%s` in `style`",
                                       entry->name)) {
        return false;
      }
    }
  }
  return true;
}

static bool leme_config_parse_output_mode(const char *text, int *width,
                                          int *height) {
  char extra;

  return sscanf(text, "%dx%d%c", width, height, &extra) == 2 && *width > 0 &&
         *height > 0;
}

static bool leme_config_parse_output_decimal(const char *text, double *value) {
  char *end;
  double parsed;

  errno = 0;
  parsed = strtod(text, &end);
  if (errno != 0 || text[0] == '\0' || *end != '\0' || !isfinite(parsed) ||
      parsed <= 0.0) {
    return false;
  }
  *value = parsed;
  return true;
}

static bool leme_config_parse_tag_selector(const char *text, uint16_t maximum,
                                           uint16_t **ids, size_t *count) {
  const char *cursor = text;
  uint16_t *result = NULL;
  size_t total = 0;

  if (text[0] == '\0') {
    return false;
  }
  while (*cursor != '\0') {
    unsigned long low;
    unsigned long high;
    char *end;
    uint16_t value;

    if (*cursor < '0' || *cursor > '9') {
      free(result);
      return false;
    }
    low = strtoul(cursor, &end, 10);
    high = low;
    cursor = end;
    if (*cursor == '-') {
      cursor++;
      if (*cursor < '0' || *cursor > '9') {
        free(result);
        return false;
      }
      high = strtoul(cursor, &end, 10);
      cursor = end;
    }
    if (low == 0 || high < low || high > maximum) {
      free(result);
      return false;
    }
    for (value = (uint16_t)low; value <= (uint16_t)high; value++) {
      uint16_t *grown = realloc(result, (total + 1) * sizeof(*grown));

      if (grown == NULL) {
        free(result);
        return false;
      }
      result = grown;
      result[total++] = value;
      if (value == (uint16_t)high) {
        break;
      }
    }
    if (*cursor == ',') {
      cursor++;
      if (*cursor == '\0') {
        free(result);
        return false;
      }
      continue;
    }
    if (*cursor != '\0') {
      free(result);
      return false;
    }
  }
  *ids = result;
  *count = total;
  return true;
}

static bool
leme_config_parse_tag_rule(struct leme_config *config,
                           const struct leme_scfg_directive *directive,
                           const char *path, char **error) {
  struct leme_tag_rule *rules;
  struct leme_tag_rule *rule;
  uint16_t *ids = NULL;
  size_t id_count = 0;
  size_t index;

  if (directive->params_len != 1) {
    return leme_config_reject(config, directive, -1,
                              "tag requires one selector");
  }
  if (!leme_config_parse_tag_selector(directive->params[0], config->max_tags,
                                      &ids, &id_count) ||
      id_count == 0) {
    return leme_config_reject(config, directive, 0, "invalid tag selector %s",
                              directive->params[0]);
  }
  rules =
      realloc(config->tag_rules, (config->tag_rule_count + 1) * sizeof(*rules));
  if (rules == NULL) {
    free(ids);
    leme_config_set_error(error, "%s:%d: out of memory", path,
                          directive->lineno);
    return false;
  }
  config->tag_rules = rules;
  rule = &config->tag_rules[config->tag_rule_count];
  *rule = (struct leme_tag_rule){
      .ids = ids,
      .id_count = id_count,
      .fields = 0,
  };
  config->tag_rule_count++;

  for (index = 0; index < directive->children.directives_len; index++) {
    const struct leme_scfg_directive *entry =
        &directive->children.directives[index];
    uint32_t field;
    double number;
    uint16_t small;

    if (entry->params_len != 1 || entry->children.directives_len != 0) {
      if (!leme_config_reject(config, entry, -1,
                              "tag property requires one value")) {
        return false;
      }
      continue;
    }
    if (strcmp(entry->name, "layout") == 0) {
      field = LEME_TAG_FIELD_LAYOUT;
      if (!leme_config_parse_layout_kind(entry->params[0],
                                         &rule->settings.layout)) {
        goto invalid;
      }
    } else if (strcmp(entry->name, "drop_mode") == 0) {
      field = LEME_TAG_FIELD_DROP_MODE;
      if (strcmp(entry->params[0], "simple") == 0) {
        rule->settings.drop_mode = LEME_DROP_MODE_SIMPLE;
      } else if (strcmp(entry->params[0], "edges") == 0) {
        rule->settings.drop_mode = LEME_DROP_MODE_EDGES;
      } else {
        goto invalid;
      }
    } else if (strcmp(entry->name, "mfact") == 0) {
      field = LEME_TAG_FIELD_MFACT;
      if (!leme_config_parse_output_decimal(entry->params[0], &number) ||
          number < 0.10 || number > 0.90) {
        goto invalid;
      }
      rule->settings.mfact = number;
    } else if (strcmp(entry->name, "split_ratio") == 0) {
      field = LEME_TAG_FIELD_SPLIT_RATIO;
      if (!leme_config_parse_output_decimal(entry->params[0], &number) ||
          number < 0.10 || number > 0.90) {
        goto invalid;
      }
      rule->settings.split_ratio = number;
    } else if (strcmp(entry->name, "nmaster") == 0) {
      field = LEME_TAG_FIELD_NMASTER;
      if (!leme_config_parse_u16(entry->params[0], &small) || small < 1 ||
          small > 16) {
        goto invalid;
      }
      rule->settings.nmaster = small;
    } else if (strcmp(entry->name, "gap") == 0) {
      field = LEME_TAG_FIELD_GAP;
      if (!leme_config_parse_u16_allow_zero(entry->params[0], &small)) {
        goto invalid;
      }
      rule->settings.gap = (int)small;
      rule->settings.has_gap = true;
    } else if (strcmp(entry->name, "accordion_collapse_width") == 0) {
      field = LEME_TAG_FIELD_COLLAPSE_WIDTH;
      if (!leme_config_parse_u16(entry->params[0], &small) || small < 10 ||
          small > 400) {
        goto invalid;
      }
      rule->settings.collapse_width = (int)small;
    } else {
      if (!leme_config_reject(config, entry, -1, "unknown tag property %s",
                              entry->name)) {
        return false;
      }
      continue;
    }
    if ((rule->fields & field) != 0) {
      if (!leme_config_reject(config, entry, -1, "duplicate tag property %s",
                              entry->name)) {
        return false;
      }
      continue;
    }
    rule->fields |= field;
    continue;

  invalid:
    if (!leme_config_reject(config, entry, 0, "invalid tag property %s",
                            entry->name)) {
      return false;
    }
  }
  return true;
}

static bool leme_config_parse_output_position(const char *first,
                                              const char *second, int *x,
                                              int *y) {
  long parsed_x;
  long parsed_y;
  char *end;

  errno = 0;
  parsed_x = strtol(first, &end, 10);
  if (errno != 0 || first[0] == '\0' || *end != '\0' || parsed_x < INT_MIN ||
      parsed_x > INT_MAX) {
    return false;
  }
  errno = 0;
  parsed_y = strtol(second, &end, 10);
  if (errno != 0 || second[0] == '\0' || *end != '\0' || parsed_y < INT_MIN ||
      parsed_y > INT_MAX) {
    return false;
  }
  *x = (int)parsed_x;
  *y = (int)parsed_y;
  return true;
}

static bool leme_config_output_relation(const char *name,
                                        enum leme_output_relation *relation) {
  if (strcmp(name, "left_of") == 0) {
    *relation = LEME_OUTPUT_RELATION_LEFT_OF;
  } else if (strcmp(name, "right_of") == 0) {
    *relation = LEME_OUTPUT_RELATION_RIGHT_OF;
  } else if (strcmp(name, "top_of") == 0) {
    *relation = LEME_OUTPUT_RELATION_TOP_OF;
  } else if (strcmp(name, "bottom_of") == 0) {
    *relation = LEME_OUTPUT_RELATION_BOTTOM_OF;
  } else {
    return false;
  }
  return true;
}

static bool
leme_config_parse_cursor_size(struct leme_config *config,
                              const struct leme_scfg_directive *entry,
                              bool *handled) {
  int value;

  if (!leme_config_parse_nonnegative(entry->params[0], &value) || value == 0 ||
      value > LEME_CURSOR_SIZE_MAX) {
    return leme_config_reject(config, entry, 0,
                              "size expects an integer between 1 and %d",
                              LEME_CURSOR_SIZE_MAX);
  }
  config->cursor.size = value;
  *handled = true;
  return true;
}

static bool
leme_config_parse_cursor(struct leme_config *config,
                         const struct leme_scfg_directive *directive,
                         const char *path, char **error) {
  static const char *const cursor_keys[] = {
      "theme",
      "size",
  };
  const struct leme_scfg_directive *dir_theme = NULL;
  const struct leme_scfg_directive *dir_size = NULL;
  bool have_size = false;
  size_t index;

  if (directive->params_len != 0) {
    leme_config_set_error(error, "%s:%d: cursor block takes no parameters",
                          path, directive->lineno);
    return false;
  }
  for (index = 0; index < directive->children.directives_len; index++) {
    const struct leme_scfg_directive *entry =
        &directive->children.directives[index];
    const bool theme = strcmp(entry->name, "theme") == 0;
    const bool size = strcmp(entry->name, "size") == 0;

    if (entry->params_len != 1 || entry->children.directives_len != 0) {
      if (!leme_config_reject(config, entry, -1,
                              "cursor property requires one value")) {
        return false;
      }
      continue;
    }
    if (theme) {
      if (dir_theme != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_theme,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(config, entry, -1, &extra,
                                         "duplicate cursor property theme")) {
          return false;
        }
        continue;
      }
      char *name = strdup(entry->params[0]);

      if (name == NULL) {
        leme_config_set_error(error, "%s:%d: out of memory", path,
                              entry->lineno);
        return false;
      }
      free(config->cursor.theme);
      config->cursor.theme = name;
      dir_theme = entry;
    } else if (size) {
      if (dir_size != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_size,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(config, entry, -1, &extra,
                                         "duplicate cursor property size")) {
          return false;
        }
        continue;
      }
      if (!leme_config_parse_cursor_size(config, entry, &have_size)) {
        return false;
      }
      dir_size = entry;
    } else {
      const char *nearest =
          leme_config_nearest_key(entry->name, cursor_keys,
                                  sizeof(cursor_keys) / sizeof(cursor_keys[0]));
      char help_buf[128] = {0};
      struct leme_reject_extra extra = {0};

      if (nearest != NULL) {
        snprintf(help_buf, sizeof(help_buf),
                 "a directive with a similar name exists: `%s`", nearest);
        extra.help = help_buf;
      }
      if (!leme_config_reject_detailed(config, entry, -1, &extra,
                                       "unknown cursor property %s",
                                       entry->name)) {
        return false;
      }
    }
  }
  return true;
}

static bool leme_config_parse_workspace_switch_gesture(
    struct leme_config *config, const struct leme_scfg_directive *directive,
    struct leme_workspace_switch_gesture_settings *settings) {
  static const char *const gesture_keys[] = {
      "fingers",      "distance",        "threshold",
      "deceleration", "velocity_window", "mode",
  };
  const struct leme_scfg_directive *seen[LEME_ARRAY_LENGTH(gesture_keys)] = {0};
  size_t index;

  if (directive->params_len != 0) {
    if (!leme_config_reject(config, directive, 0,
                            "workspace_switch block takes no parameters")) {
      return false;
    }
  }
  for (index = 0; index < directive->children.directives_len; index++) {
    const struct leme_scfg_directive *entry =
        &directive->children.directives[index];
    size_t key;

    for (key = 0; key < LEME_ARRAY_LENGTH(gesture_keys); key++) {
      if (strcmp(entry->name, gesture_keys[key]) == 0) {
        break;
      }
    }
    if (key == LEME_ARRAY_LENGTH(gesture_keys)) {
      const char *nearest = leme_config_nearest_key(
          entry->name, gesture_keys, LEME_ARRAY_LENGTH(gesture_keys));
      char help_buf[128] = {0};
      struct leme_reject_extra extra = {0};

      if (nearest != NULL) {
        snprintf(help_buf, sizeof(help_buf),
                 "a directive with a similar name exists: `%s`", nearest);
        extra.help = help_buf;
      }
      if (!leme_config_reject_detailed(config, entry, -1, &extra,
                                       "unknown workspace_switch property %s",
                                       entry->name)) {
        return false;
      }
      continue;
    }
    if (entry->children.directives_len != 0) {
      if (!leme_config_reject(config, entry, -1, "%s takes no block",
                              entry->name)) {
        return false;
      }
      continue;
    }
    if (seen[key] != NULL) {
      const struct leme_reject_extra extra = {
          .secondary = seen[key],
          .secondary_label = "first defined here",
      };
      if (!leme_config_reject_detailed(config, entry, -1, &extra,
                                       "duplicate workspace_switch property %s",
                                       entry->name)) {
        return false;
      }
      continue;
    }
    if (entry->params_len != 1) {
      if (!leme_config_reject(config, entry, -1, "%s requires one value",
                              entry->name)) {
        return false;
      }
      continue;
    }
    seen[key] = entry;
    if (key == 0) {
      int value;
      if (!leme_config_parse_nonnegative(entry->params[0], &value)) {
        if (!leme_config_reject(config, entry, 0,
                                "fingers expects an integer")) {
          return false;
        }
        continue;
      }
      if (value == 2) {
        if (!leme_config_reject(
                config, entry, 0,
                "libinput does not deliver two-finger swipes")) {
          return false;
        }
        continue;
      }
      if (value == 1) {
        if (!leme_config_reject(config, entry, 0,
                                "fingers must be 0 or at least 3")) {
          return false;
        }
        continue;
      }
      settings->fingers = (uint32_t)value;
    } else if (key == 1) {
      double decimal;
      if (!leme_config_parse_decimal(entry->params[0], &decimal) ||
          decimal <= 0.0) {
        if (!leme_config_reject(config, entry, 0,
                                "distance must be greater than zero")) {
          return false;
        }
        continue;
      }
      settings->distance = decimal;
    } else if (key == 2) {
      double decimal;
      if (!leme_config_parse_decimal(entry->params[0], &decimal) ||
          !(decimal > 0.0 && decimal < 1.0)) {
        if (!leme_config_reject(config, entry, 0,
                                "threshold must be between 0 and 1")) {
          return false;
        }
        continue;
      }
      settings->threshold = decimal;
    } else if (key == 3) {
      double decimal;
      if (!leme_config_parse_decimal(entry->params[0], &decimal) ||
          !(decimal > 0.0 && decimal < 1.0)) {
        if (!leme_config_reject(config, entry, 0,
                                "deceleration must be between 0 and 1")) {
          return false;
        }
        continue;
      }
      settings->deceleration = decimal;
    } else if (key == 4) {
      int value;
      if (!leme_config_parse_nonnegative(entry->params[0], &value) ||
          value <= 0) {
        if (!leme_config_reject(config, entry, 0,
                                "velocity_window must be greater than zero")) {
          return false;
        }
        continue;
      }
      settings->velocity_window_ms = (uint32_t)value;
    } else if (key == 5) {
      if (strcmp(entry->params[0], "single") == 0) {
        settings->mode = LEME_WORKSPACE_GESTURE_SINGLE;
      } else if (strcmp(entry->params[0], "scrub") == 0) {
        settings->mode = LEME_WORKSPACE_GESTURE_SCRUB;
      } else if (strcmp(entry->params[0], "free") == 0) {
        settings->mode = LEME_WORKSPACE_GESTURE_FREE;
      } else if (!leme_config_reject(config, entry, 0,
                                     "mode expects single, scrub, or free")) {
        return false;
      }
    }
  }
  return true;
}

static bool
leme_config_parse_gestures(struct leme_config *config,
                           const struct leme_scfg_directive *directive,
                           const char *path, char **error) {
  static const char *const gesture_blocks[] = {
      "workspace_switch",
  };
  const struct leme_scfg_directive *dir_workspace_switch = NULL;
  size_t index;

  if (directive->params_len != 0) {
    leme_config_set_error(error, "%s:%d: gestures block takes no parameters",
                          path, directive->lineno);
    return false;
  }
  for (index = 0; index < directive->children.directives_len; index++) {
    const struct leme_scfg_directive *entry =
        &directive->children.directives[index];

    if (strcmp(entry->name, "workspace_switch") == 0) {
      if (dir_workspace_switch != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_workspace_switch,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(
                config, entry, -1, &extra,
                "duplicate gestures property workspace_switch")) {
          return false;
        }
        continue;
      }
      dir_workspace_switch = entry;
      if (!leme_config_parse_workspace_switch_gesture(
              config, entry, &config->gestures.workspace_switch)) {
        return false;
      }
    } else {
      const char *nearest = leme_config_nearest_key(
          entry->name, gesture_blocks,
          sizeof(gesture_blocks) / sizeof(gesture_blocks[0]));
      char help_buf[128] = {0};
      struct leme_reject_extra extra = {0};

      if (nearest != NULL) {
        snprintf(help_buf, sizeof(help_buf),
                 "a directive with a similar name exists: `%s`", nearest);
        extra.help = help_buf;
      }
      if (!leme_config_reject_detailed(config, entry, -1, &extra,
                                       "unknown gestures property %s",
                                       entry->name)) {
        return false;
      }
    }
  }
  return true;
}

static bool
leme_config_parse_publication(struct leme_config *config,
                              const struct leme_scfg_directive *directive,
                              const char *path, char **error) {
  static const char *const publication_keys[] = {
      "activation",
  };
  const struct leme_scfg_directive *dir_activation = NULL;
  size_t index;

  if (directive->params_len != 0) {
    leme_config_set_error(error, "%s:%d: publication block takes no parameters",
                          path, directive->lineno);
    return false;
  }
  for (index = 0; index < directive->children.directives_len; index++) {
    const struct leme_scfg_directive *entry =
        &directive->children.directives[index];

    if (entry->params_len != 1 || entry->children.directives_len != 0) {
      if (!leme_config_reject(config, entry, -1,
                              "publication property requires one value")) {
        return false;
      }
      continue;
    }
    if (strcmp(entry->name, "activation") == 0) {
      if (dir_activation != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_activation,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(
                config, entry, -1, &extra,
                "duplicate publication property activation")) {
          return false;
        }
        continue;
      }
      if (strcmp(entry->params[0], "follow") == 0) {
        config->publication.activation = LEME_ACTIVATION_FOLLOW;
      } else if (strcmp(entry->params[0], "urgent") == 0) {
        config->publication.activation = LEME_ACTIVATION_URGENT;
      } else if (strcmp(entry->params[0], "ignore") == 0) {
        config->publication.activation = LEME_ACTIVATION_IGNORE;
      } else {
        const struct leme_reject_extra extra = {
            .help = "valid values are `follow`, `urgent`, and `ignore`",
        };
        if (!leme_config_reject_detailed(
                config, entry, 0, &extra,
                "activation must be follow, urgent, or ignore")) {
          return false;
        }
        continue;
      }
      dir_activation = entry;
    } else {
      const char *nearest = leme_config_nearest_key(
          entry->name, publication_keys,
          sizeof(publication_keys) / sizeof(publication_keys[0]));
      char help_buf[128] = {0};
      struct leme_reject_extra extra = {0};

      if (nearest != NULL) {
        snprintf(help_buf, sizeof(help_buf),
                 "a directive with a similar name exists: `%s`", nearest);
        extra.help = help_buf;
      }
      if (!leme_config_reject_detailed(config, entry, -1, &extra,
                                       "unknown publication property %s",
                                       entry->name)) {
        return false;
      }
    }
  }
  return true;
}

static bool
leme_config_parse_output_policy(struct leme_config *config,
                                const struct leme_scfg_directive *directive,
                                const char *path, char **error) {
  static const char *const output_policy_keys[] = {
      "cross_output_focus",
      "cross_output_move",
      "cross_output_drag",
      "warp_cursor",
  };
  const struct leme_scfg_directive *dir_focus = NULL;
  const struct leme_scfg_directive *dir_move = NULL;
  const struct leme_scfg_directive *dir_drag = NULL;
  const struct leme_scfg_directive *dir_warp = NULL;
  size_t index;

  (void)path;
  (void)error;
  for (index = 0; index < directive->children.directives_len; index++) {
    const struct leme_scfg_directive *entry =
        &directive->children.directives[index];
    bool value;

    if (entry->params_len != 1 || entry->children.directives_len != 0) {
      if (!leme_config_reject(config, entry, -1,
                              "output policy property requires one value")) {
        return false;
      }
      continue;
    }
    if (strcmp(entry->params[0], "true") == 0) {
      value = true;
    } else if (strcmp(entry->params[0], "false") == 0) {
      value = false;
    } else {
      if (!leme_config_reject(config, entry, 0,
                              "output policy boolean must be true or false")) {
        return false;
      }
      continue;
    }
    if (strcmp(entry->name, "cross_output_focus") == 0) {
      if (dir_focus != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_focus,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(
                config, entry, -1, &extra,
                "duplicate output policy property cross_output_focus")) {
          return false;
        }
        continue;
      }
      config->output_policy.cross_output_focus = value;
      dir_focus = entry;
    } else if (strcmp(entry->name, "cross_output_move") == 0) {
      if (dir_move != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_move,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(
                config, entry, -1, &extra,
                "duplicate output policy property cross_output_move")) {
          return false;
        }
        continue;
      }
      config->output_policy.cross_output_move = value;
      dir_move = entry;
    } else if (strcmp(entry->name, "cross_output_drag") == 0) {
      if (dir_drag != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_drag,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(
                config, entry, -1, &extra,
                "duplicate output policy property cross_output_drag")) {
          return false;
        }
        continue;
      }
      config->output_policy.cross_output_drag = value;
      dir_drag = entry;
    } else if (strcmp(entry->name, "warp_cursor") == 0) {
      if (dir_warp != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_warp,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(
                config, entry, -1, &extra,
                "duplicate output policy property warp_cursor")) {
          return false;
        }
        continue;
      }
      config->output_policy.warp_cursor = value;
      dir_warp = entry;
    } else {
      const char *nearest = leme_config_nearest_key(
          entry->name, output_policy_keys,
          sizeof(output_policy_keys) / sizeof(output_policy_keys[0]));
      char help_buf[128] = {0};
      struct leme_reject_extra extra = {0};

      if (nearest != NULL) {
        snprintf(help_buf, sizeof(help_buf),
                 "a directive with a similar name exists: `%s`", nearest);
        extra.help = help_buf;
      }
      if (!leme_config_reject_detailed(config, entry, -1, &extra,
                                       "unknown output policy property %s",
                                       entry->name)) {
        return false;
      }
    }
  }
  return true;
}

static struct leme_output_config *
leme_config_append_output(struct leme_config *config, const char *name) {
  struct leme_output_config *outputs =
      realloc(config->outputs, (config->output_count + 1) * sizeof(*outputs));
  struct leme_output_config *entry;

  if (outputs == NULL) {
    return NULL;
  }
  config->outputs = outputs;
  entry = &config->outputs[config->output_count];
  *entry = (struct leme_output_config){
      .name = strdup(name),
      .scale = 1.0f,
      .transform = LEME_OUTPUT_TRANSFORM_NORMAL,
      .relation = LEME_OUTPUT_RELATION_NONE,
      .configured = true,
  };
  if (entry->name == NULL) {
    return NULL;
  }
  config->output_count++;
  return entry;
}

static bool
leme_config_parse_output(struct leme_config *config,
                         const struct leme_scfg_directive *directive,
                         const char *path, char **error, bool *have_policy) {
  enum {
    OUTPUT_MODE = 1 << 0,
    OUTPUT_REFRESH = 1 << 1,
    OUTPUT_SCALE = 1 << 2,
    OUTPUT_TRANSFORM = 1 << 3,
    OUTPUT_POSITION = 1 << 4,
    OUTPUT_RELATION = 1 << 5,
  };
  struct leme_output_config *target;
  uint32_t seen = 0;
  size_t index;

  if (directive->params_len > 1) {
    leme_config_set_error(error,
                          "%s:%d: output accepts at most one connector name",
                          path, directive->lineno);
    return false;
  }
  if (directive->params_len == 0) {
    if (*have_policy) {
      leme_config_set_error(error, "%s:%d: duplicate global output block", path,
                            directive->lineno);
      return false;
    }
    *have_policy = true;
    return leme_config_parse_output_policy(config, directive, path, error);
  }
  if (directive->params[0][0] == '\0') {
    leme_config_set_error(error, "%s:%d: output connector name cannot be empty",
                          path, directive->lineno);
    return false;
  }
  for (index = 0; index < config->output_count; index++) {
    if (strcmp(config->outputs[index].name, directive->params[0]) == 0) {
      leme_config_set_error(error, "%s:%d: duplicate output %s", path,
                            directive->lineno, directive->params[0]);
      return false;
    }
  }
  target = leme_config_append_output(config, directive->params[0]);
  if (target == NULL) {
    leme_config_set_error(error, "%s:%d: out of memory", path,
                          directive->lineno);
    return false;
  }
  for (index = 0; index < directive->children.directives_len; index++) {
    const struct leme_scfg_directive *entry =
        &directive->children.directives[index];
    enum leme_output_relation relation;
    uint32_t field;

    if (entry->children.directives_len != 0) {
      leme_config_set_error(error, "%s:%d: output property takes no block",
                            path, entry->lineno);
      return false;
    }
    if (strcmp(entry->name, "position") == 0) {
      field = OUTPUT_POSITION;
      if (entry->params_len != 2 ||
          !leme_config_parse_output_position(entry->params[0], entry->params[1],
                                             &target->x, &target->y)) {
        leme_config_set_error(error,
                              "%s:%d: output position requires two integers",
                              path, entry->lineno);
        return false;
      }
      target->has_position = true;
      seen |= field;
      continue;
    }
    if (leme_config_output_relation(entry->name, &relation)) {
      field = OUTPUT_RELATION;
      if (entry->params_len != 1 || entry->params[0][0] == '\0') {
        leme_config_set_error(error, "%s:%d: %s requires one connector name",
                              path, entry->lineno, entry->name);
        return false;
      }
      if ((seen & field) != 0) {
        leme_config_set_error(error,
                              "%s:%d: output already has a relative placement",
                              path, entry->lineno);
        return false;
      }
      free(target->relative_to);
      target->relative_to = strdup(entry->params[0]);
      if (target->relative_to == NULL) {
        leme_config_set_error(error, "%s:%d: out of memory", path,
                              entry->lineno);
        return false;
      }
      target->relation = relation;
      seen |= field;
      continue;
    }
    if (entry->params_len != 1) {
      leme_config_set_error(error, "%s:%d: output property requires one value",
                            path, entry->lineno);
      return false;
    }
    if (strcmp(entry->name, "mode") == 0) {
      field = OUTPUT_MODE;
      if (!leme_config_parse_output_mode(entry->params[0], &target->width,
                                         &target->height)) {
        leme_config_set_error(error, "%s:%d: invalid output mode", path,
                              entry->lineno);
        return false;
      }
      target->has_mode = true;
    } else if (strcmp(entry->name, "refresh") == 0) {
      double refresh;

      field = OUTPUT_REFRESH;
      if (!leme_config_parse_output_decimal(entry->params[0], &refresh) ||
          refresh > (double)INT_MAX / 1000.0) {
        leme_config_set_error(error, "%s:%d: invalid output refresh", path,
                              entry->lineno);
        return false;
      }
      target->refresh_mhz = (int)lround(refresh * 1000.0);
      target->has_refresh = true;
    } else if (strcmp(entry->name, "scale") == 0) {
      double scale;

      field = OUTPUT_SCALE;
      if (!leme_config_parse_output_decimal(entry->params[0], &scale) ||
          scale < 0.5 || scale > 4.0) {
        leme_config_set_error(error,
                              "%s:%d: output scale must be between 0.5 and 4.0",
                              path, entry->lineno);
        return false;
      }
      target->scale = (float)scale;
    } else if (strcmp(entry->name, "transform") == 0) {
      field = OUTPUT_TRANSFORM;
      if (strcmp(entry->params[0], "normal") == 0) {
        target->transform = LEME_OUTPUT_TRANSFORM_NORMAL;
      } else if (strcmp(entry->params[0], "90") == 0) {
        target->transform = LEME_OUTPUT_TRANSFORM_90;
      } else if (strcmp(entry->params[0], "180") == 0) {
        target->transform = LEME_OUTPUT_TRANSFORM_180;
      } else if (strcmp(entry->params[0], "270") == 0) {
        target->transform = LEME_OUTPUT_TRANSFORM_270;
      } else {
        leme_config_set_error(error, "%s:%d: invalid output transform", path,
                              entry->lineno);
        return false;
      }
    } else {
      leme_config_set_error(error, "%s:%d: unknown output property %s", path,
                            entry->lineno, entry->name);
      return false;
    }
    if ((seen & field) != 0) {
      leme_config_set_error(error, "%s:%d: duplicate output property %s", path,
                            entry->lineno, entry->name);
      return false;
    }
    seen |= field;
  }
  if (target->has_refresh && !target->has_mode) {
    leme_config_set_error(error, "%s:%d: output refresh requires mode", path,
                          directive->lineno);
    return false;
  }
  if (target->has_position && target->relation != LEME_OUTPUT_RELATION_NONE) {
    wlr_log(WLR_INFO,
            "leme: output %s sets both position and a relative placement, "
            "using position",
            target->name);
  }
  return true;
}

static bool leme_config_parse_boolean(const char *text, bool *value) {
  if (strcmp(text, "true") == 0) {
    *value = true;
    return true;
  }
  if (strcmp(text, "false") == 0) {
    *value = false;
    return true;
  }
  return false;
}

static bool leme_config_parse_pointer_speed(const char *text, double *value) {
  char *end;
  double parsed;

  errno = 0;
  parsed = strtod(text, &end);
  if (errno != 0 || text[0] == '\0' || *end != '\0' || !isfinite(parsed) ||
      parsed < -1.0 || parsed > 1.0) {
    return false;
  }
  *value = parsed;
  return true;
}

static bool
leme_config_parse_pointer(struct leme_config *config,
                          const struct leme_scfg_directive *directive,
                          const char *path, char **error, bool *have_global) {
  static const char *const pointer_keys[] = {
      "accel_profile", "accel_speed", "natural_scroll", "left_handed", "tap",
  };
  struct leme_pointer_settings local = {0};
  struct leme_pointer_settings *settings;
  const struct leme_scfg_directive *dir_profile = NULL;
  const struct leme_scfg_directive *dir_speed = NULL;
  const struct leme_scfg_directive *dir_natural_scroll = NULL;
  const struct leme_scfg_directive *dir_left_handed = NULL;
  const struct leme_scfg_directive *dir_tap = NULL;
  const char *name = NULL;
  size_t index;

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
    for (index = 0; index < config->pointer_rule_count; index++) {
      if (strcmp(config->pointer_rules[index].name, name) == 0) {
        leme_config_set_error(error, "%s:%d: duplicate pointer device %s", path,
                              directive->lineno, name);
        return false;
      }
    }
    settings = &local;
  }
  for (index = 0; index < directive->children.directives_len; index++) {
    const struct leme_scfg_directive *entry =
        &directive->children.directives[index];

    if (entry->params_len != 1 || entry->children.directives_len != 0) {
      if (!leme_config_reject(config, entry, -1,
                              "pointer property requires one value")) {
        return false;
      }
      continue;
    }
    if (strcmp(entry->name, "accel_profile") == 0) {
      if (dir_profile != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_profile,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(
                config, entry, -1, &extra,
                "duplicate pointer property accel_profile")) {
          return false;
        }
        continue;
      }
      if (strcmp(entry->params[0], "adaptive") == 0) {
        settings->profile = LEME_POINTER_ACCEL_ADAPTIVE;
      } else if (strcmp(entry->params[0], "flat") == 0) {
        settings->profile = LEME_POINTER_ACCEL_FLAT;
      } else {
        const struct leme_reject_extra extra = {
            .help = "valid values are `adaptive` and `flat`",
        };
        if (!leme_config_reject_detailed(
                config, entry, 0, &extra,
                "invalid pointer acceleration profile")) {
          return false;
        }
        continue;
      }
      dir_profile = entry;
      settings->fields |= LEME_POINTER_PROFILE;
    } else if (strcmp(entry->name, "accel_speed") == 0) {
      if (dir_speed != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_speed,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(
                config, entry, -1, &extra,
                "duplicate pointer property accel_speed")) {
          return false;
        }
        continue;
      }
      if (!leme_config_parse_pointer_speed(entry->params[0],
                                           &settings->speed)) {
        if (!leme_config_reject(config, entry, 0,
                                "pointer speed must be between -1.0 and 1.0")) {
          return false;
        }
        continue;
      }
      dir_speed = entry;
      settings->fields |= LEME_POINTER_SPEED;
    } else if (strcmp(entry->name, "natural_scroll") == 0) {
      if (dir_natural_scroll != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_natural_scroll,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(
                config, entry, -1, &extra,
                "duplicate pointer property natural_scroll")) {
          return false;
        }
        continue;
      }
      if (!leme_config_parse_boolean(entry->params[0],
                                     &settings->natural_scroll)) {
        goto invalid_boolean;
      }
      dir_natural_scroll = entry;
      settings->fields |= LEME_POINTER_NATURAL_SCROLL;
    } else if (strcmp(entry->name, "left_handed") == 0) {
      if (dir_left_handed != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_left_handed,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(
                config, entry, -1, &extra,
                "duplicate pointer property left_handed")) {
          return false;
        }
        continue;
      }
      if (!leme_config_parse_boolean(entry->params[0],
                                     &settings->left_handed)) {
        goto invalid_boolean;
      }
      dir_left_handed = entry;
      settings->fields |= LEME_POINTER_LEFT_HANDED;
    } else if (strcmp(entry->name, "tap") == 0) {
      if (dir_tap != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_tap,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(config, entry, -1, &extra,
                                         "duplicate pointer property tap")) {
          return false;
        }
        continue;
      }
      if (!leme_config_parse_boolean(entry->params[0], &settings->tap)) {
        goto invalid_boolean;
      }
      dir_tap = entry;
      settings->fields |= LEME_POINTER_TAP;
    } else {
      const char *nearest = leme_config_nearest_key(
          entry->name, pointer_keys,
          sizeof(pointer_keys) / sizeof(pointer_keys[0]));
      char help_buf[128] = {0};
      struct leme_reject_extra extra = {0};

      if (nearest != NULL) {
        snprintf(help_buf, sizeof(help_buf),
                 "a directive with a similar name exists: `%s`", nearest);
        extra.help = help_buf;
      }
      if (!leme_config_reject_detailed(config, entry, -1, &extra,
                                       "unknown pointer property %s",
                                       entry->name)) {
        return false;
      }
      continue;
    }
    continue;

  invalid_boolean:
    if (!leme_config_reject(config, entry, 0,
                            "pointer boolean must be true or false")) {
      return false;
    }
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

static bool
leme_config_parse_config_errors(struct leme_config *config,
                                const struct leme_scfg_directive *directive,
                                const char *path, char **error) {
  static const char *const config_errors_keys[] = {
      "show",
      "position",
      "timeout",
  };
  const struct leme_scfg_directive *dir_show = NULL;
  const struct leme_scfg_directive *dir_position = NULL;
  const struct leme_scfg_directive *dir_timeout = NULL;
  size_t index;

  if (directive->params_len != 0) {
    leme_config_set_error(error, "%s:%d: config_errors takes no arguments",
                          path, directive->lineno);
    return false;
  }
  for (index = 0; index < directive->children.directives_len; index++) {
    const struct leme_scfg_directive *entry =
        &directive->children.directives[index];
    bool flag;
    int value;

    if (entry->params_len != 1 || entry->children.directives_len != 0) {
      if (!leme_config_reject(config, entry, -1, "%s requires one value",
                              entry->name)) {
        return false;
      }
      continue;
    }
    if (strcmp(entry->name, "show") == 0) {
      if (dir_show != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_show,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(
                config, entry, -1, &extra,
                "duplicate config_errors directive show")) {
          return false;
        }
        continue;
      }
      if (!leme_config_parse_boolean(entry->params[0], &flag)) {
        if (!leme_config_reject(config, entry, 0,
                                "show expects true or false")) {
          return false;
        }
        continue;
      }
      config->config_errors.show = flag;
      dir_show = entry;
    } else if (strcmp(entry->name, "position") == 0) {
      if (dir_position != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_position,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(
                config, entry, -1, &extra,
                "duplicate config_errors directive position")) {
          return false;
        }
        continue;
      }
      if (strcmp(entry->params[0], "top") == 0) {
        config->config_errors.position = LEME_BANNER_TOP;
      } else if (strcmp(entry->params[0], "bottom") == 0) {
        config->config_errors.position = LEME_BANNER_BOTTOM;
      } else {
        const struct leme_reject_extra extra = {
            .help = "valid values are `top` and `bottom`",
        };
        if (!leme_config_reject_detailed(config, entry, 0, &extra,
                                         "position expects top or bottom")) {
          return false;
        }
        continue;
      }
      dir_position = entry;
    } else if (strcmp(entry->name, "timeout") == 0) {
      if (dir_timeout != NULL) {
        const struct leme_reject_extra extra = {
            .secondary = dir_timeout,
            .secondary_label = "first defined here",
        };
        if (!leme_config_reject_detailed(
                config, entry, -1, &extra,
                "duplicate config_errors directive timeout")) {
          return false;
        }
        continue;
      }
      if (!leme_config_parse_nonnegative(entry->params[0], &value)) {
        if (!leme_config_reject(config, entry, 0,
                                "timeout expects a nonnegative integer")) {
          return false;
        }
        continue;
      }
      config->config_errors.timeout = value;
      dir_timeout = entry;
    } else {
      const char *nearest = leme_config_nearest_key(
          entry->name, config_errors_keys,
          sizeof(config_errors_keys) / sizeof(config_errors_keys[0]));
      char help_buf[128] = {0};
      struct leme_reject_extra extra = {0};

      if (nearest != NULL) {
        snprintf(help_buf, sizeof(help_buf),
                 "a directive with a similar name exists: `%s`", nearest);
        extra.help = help_buf;
      }
      if (!leme_config_reject_detailed(config, entry, -1, &extra,
                                       "unknown config_errors directive %s",
                                       entry->name)) {
        return false;
      }
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

static bool
leme_config_parse_keyboard_repeat(struct leme_config *config,
                                  const struct leme_scfg_directive *entry,
                                  int minimum, int maximum, int *target) {
  int value;

  if (entry->params_len != 1 || entry->children.directives_len != 0 ||
      !leme_config_parse_nonnegative(entry->params[0], &value) ||
      value < minimum || value > maximum) {
    return leme_config_reject(config, entry, 0,
                              "%s expects an integer between %d and %d",
                              entry->name, minimum, maximum);
  }
  *target = value;
  return true;
}

static bool
leme_config_parse_keyboard(struct leme_config *config,
                           const struct leme_scfg_directive *directive,
                           const char *path, char **error) {
  static const char *const keyboard_keys[] = {
      "layout",
      "options",
      "repeat_rate",
      "repeat_delay",
  };
  const struct leme_scfg_directive *layout_directives[4] = {NULL};
  const struct leme_scfg_directive *dir_options = NULL;
  const struct leme_scfg_directive *dir_rate = NULL;
  const struct leme_scfg_directive *dir_delay = NULL;
  size_t index;

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
  for (index = 0; index < directive->children.directives_len; index++) {
    const struct leme_scfg_directive *entry =
        &directive->children.directives[index];
    const struct leme_scfg_directive **first = NULL;

    if (strcmp(entry->name, "layout") == 0) {
      if (config->keyboard_layout_count == 4) {
        leme_config_set_error(error,
                              "%s:%d: keyboard takes at most four layouts",
                              path, entry->lineno);
        return false;
      }
      if (!leme_config_parse_keyboard_layout(config, entry, layout_directives,
                                             path, error)) {
        return false;
      }
      continue;
    }
    if (strcmp(entry->name, "options") == 0) {
      first = &dir_options;
    } else if (strcmp(entry->name, "repeat_rate") == 0) {
      first = &dir_rate;
    } else if (strcmp(entry->name, "repeat_delay") == 0) {
      first = &dir_delay;
    } else {
      const char *nearest = leme_config_nearest_key(
          entry->name, keyboard_keys,
          sizeof(keyboard_keys) / sizeof(keyboard_keys[0]));
      char help_buf[128] = {0};
      struct leme_reject_extra extra = {0};

      if (nearest != NULL) {
        snprintf(help_buf, sizeof(help_buf),
                 "a directive with a similar name exists: `%s`", nearest);
        extra.help = help_buf;
      }
      if (!leme_config_reject_detailed(config, entry, -1, &extra,
                                       "unknown keyboard property %s",
                                       entry->name)) {
        return false;
      }
      continue;
    }
    if (*first != NULL) {
      const struct leme_reject_extra extra = {
          .secondary = *first,
          .secondary_label = "first defined here",
      };
      if (!leme_config_reject_detailed(config, entry, -1, &extra,
                                       "duplicate keyboard property %s",
                                       entry->name)) {
        return false;
      }
      continue;
    }
    *first = entry;
    if (first == &dir_options) {
      if (!leme_config_parse_keyboard_options(config, entry, path, error)) {
        return false;
      }
    } else if (first == &dir_rate) {
      if (!leme_config_parse_keyboard_repeat(config, entry, 0,
                                             LEME_KEYBOARD_REPEAT_RATE_MAX,
                                             &config->keyboard_repeat_rate)) {
        return false;
      }
    } else if (!leme_config_parse_keyboard_repeat(
                   config, entry, 1, LEME_KEYBOARD_REPEAT_DELAY_MAX,
                   &config->keyboard_repeat_delay)) {
      return false;
    }
  }
  return true;
}

static bool leme_config_parse_key(const char *text, uint32_t *modifiers,
                                  xkb_keysym_t *keysym) {
  char *copy = strdup(text);
  char *key;
  char *prefix;
  char *save = NULL;
  char *token;
  char *separator;
  bool valid = true;

  if (copy == NULL) {
    return false;
  }
  *modifiers = 0;
  separator = strrchr(copy, '+');
  if (separator == NULL) {
    key = copy;
    prefix = NULL;
  } else {
    *separator = '\0';
    key = separator + 1;
    prefix = copy;
  }
  if (key[0] == '\0' || (prefix != NULL && (prefix[0] == '\0' ||
                                            prefix[strlen(prefix) - 1] == '+' ||
                                            strstr(prefix, "++") != NULL))) {
    valid = false;
  }
  for (token = prefix == NULL ? NULL : strtok_r(prefix, "+", &save);
       valid && token != NULL; token = strtok_r(NULL, "+", &save)) {
    uint32_t modifier;

    if (strcasecmp(token, "SUPER") == 0) {
      modifier = WLR_MODIFIER_LOGO;
    } else if (strcasecmp(token, "ALT") == 0) {
      modifier = WLR_MODIFIER_ALT;
    } else if (strcasecmp(token, "CTRL") == 0 ||
               strcasecmp(token, "CONTROL") == 0) {
      modifier = WLR_MODIFIER_CTRL;
    } else if (strcasecmp(token, "SHIFT") == 0) {
      modifier = WLR_MODIFIER_SHIFT;
    } else {
      valid = false;
      break;
    }
    if ((*modifiers & modifier) != 0) {
      valid = false;
      break;
    }
    *modifiers |= modifier;
  }
  *keysym = valid ? xkb_keysym_from_name(key, XKB_KEYSYM_CASE_INSENSITIVE)
                  : XKB_KEY_NoSymbol;
  valid = valid && *keysym != XKB_KEY_NoSymbol;
  free(copy);
  return valid;
}

static bool leme_config_parse_command(struct leme_command *command,
                                      const struct leme_scfg_directive *entry,
                                      const char *path, char **error) {
  char *detail = NULL;

  if (entry->params_len == 0) {
    leme_config_set_error(error, "%s:%d: binding requires a command", path,
                          entry->lineno);
    return false;
  }
  if (!leme_command_parse(command, entry->params, entry->params_len, &detail)) {
    leme_config_set_error(error, "%s:%d: %s", path, entry->lineno,
                          detail == NULL ? "invalid command" : detail);
    free(detail);
    return false;
  }
  return true;
}

static struct leme_bind_group *
leme_bind_scope_find(struct leme_bind_scope *scope, const char *name) {
  size_t index;

  for (index = 0; index < scope->count; index++) {
    if (strcmp(scope->groups[index].name, name) == 0) {
      return &scope->groups[index];
    }
  }
  return NULL;
}

static bool leme_bind_group_add_inherit(struct leme_bind_group *group,
                                        const char *name, const char *path,
                                        int lineno, char **error) {
  char **names =
      realloc(group->inherits, (group->inherit_count + 1) * sizeof(*names));

  if (names == NULL) {
    leme_config_set_error(error, "%s:%d: out of memory", path, lineno);
    return false;
  }
  group->inherits = names;
  group->inherits[group->inherit_count] = strdup(name);
  if (group->inherits[group->inherit_count] == NULL) {
    leme_config_set_error(error, "%s:%d: out of memory", path, lineno);
    return false;
  }
  group->inherit_count++;
  return true;
}

static bool
leme_config_parse_bind_block(struct leme_config *config,
                             struct leme_bind_scope *scope,
                             const struct leme_scfg_directive *directive,
                             bool is_mode, const char *path, char **error) {
  struct leme_bind_group *groups;
  struct leme_bind_group *group;
  size_t index;
  bool seen_escape_exits = false;

  if (directive->params_len != 1) {
    leme_config_set_error(error, "%s:%d: %s requires one name", path,
                          directive->lineno, directive->name);
    return false;
  }
  if (leme_bind_scope_find(scope, directive->params[0]) != NULL) {
    leme_config_set_error(error, "%s:%d: duplicate binding block name %s", path,
                          directive->lineno, directive->params[0]);
    return false;
  }
  groups = realloc(scope->groups, (scope->count + 1) * sizeof(*groups));
  if (groups == NULL) {
    leme_config_set_error(error, "%s:%d: out of memory", path,
                          directive->lineno);
    return false;
  }
  scope->groups = groups;
  group = &scope->groups[scope->count];
  *group = (struct leme_bind_group){0};
  group->is_mode = is_mode;
  group->escape_exits = true;
  group->lineno = directive->lineno;
  group->name = strdup(directive->params[0]);
  if (group->name == NULL) {
    leme_config_set_error(error, "%s:%d: out of memory", path,
                          directive->lineno);
    return false;
  }
  scope->count++;

  const struct leme_scfg_directive **binding_directives = NULL;

  for (index = 0; index < directive->children.directives_len; index++) {
    const struct leme_scfg_directive *entry =
        &directive->children.directives[index];
    const struct leme_scfg_directive **dirs;
    struct leme_binding *bindings;
    struct leme_binding *binding;
    size_t previous;
    bool duplicate = false;

    if (entry->children.directives_len != 0) {
      if (!leme_config_reject(config, entry, -1,
                              "binding cannot contain a block")) {
        free(binding_directives);
        return false;
      }
      continue;
    }
    if (strcmp(entry->name, "inherit") == 0) {
      if (entry->params_len != 1) {
        if (!leme_config_reject(config, entry, 0,
                                "inherit requires one bind group name")) {
          free(binding_directives);
          return false;
        }
        continue;
      }
      if (!leme_bind_group_add_inherit(group, entry->params[0], path,
                                       entry->lineno, error)) {
        free(binding_directives);
        return false;
      }
      continue;
    }
    if (strcmp(entry->name, "escape_exits") == 0) {
      if (!is_mode) {
        if (!leme_config_reject(config, entry, -1,
                                "escape_exits belongs to a binds block, "
                                "not a bind_group")) {
          free(binding_directives);
          return false;
        }
        continue;
      }
      if (seen_escape_exits || entry->params_len != 1) {
        if (!leme_config_reject(
                config, entry, -1,
                "escape_exits takes one value and may appear once")) {
          free(binding_directives);
          return false;
        }
        continue;
      }
      if (strcmp(entry->params[0], "true") == 0) {
        group->escape_exits = true;
      } else if (strcmp(entry->params[0], "false") == 0) {
        group->escape_exits = false;
      } else {
        if (!leme_config_reject(config, entry, 0,
                                "escape_exits must be true or false")) {
          free(binding_directives);
          return false;
        }
        continue;
      }
      seen_escape_exits = true;
      continue;
    }
    bindings = realloc(group->bindings,
                       (group->binding_count + 1) * sizeof(*bindings));
    if (bindings == NULL) {
      free(binding_directives);
      leme_config_set_error(error, "%s:%d: out of memory", path, entry->lineno);
      return false;
    }
    group->bindings = bindings;
    dirs =
        realloc(binding_directives, (group->binding_count + 1) * sizeof(*dirs));
    if (dirs == NULL) {
      free(binding_directives);
      leme_config_set_error(error, "%s:%d: out of memory", path, entry->lineno);
      return false;
    }
    binding_directives = dirs;
    binding = &group->bindings[group->binding_count];
    *binding = (struct leme_binding){0};
    binding->lineno = entry->lineno;
    if (!leme_config_parse_key(entry->name, &binding->modifiers,
                               &binding->keysym)) {
      if (!leme_config_reject(config, entry, -1, "invalid key specification %s",
                              entry->name)) {
        free(binding_directives);
        return false;
      }
      continue;
    }
    for (previous = 0; previous < group->binding_count; previous++) {
      if (group->bindings[previous].modifiers == binding->modifiers &&
          group->bindings[previous].keysym == binding->keysym) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      const struct leme_reject_extra extra = {
          .secondary = binding_directives[previous],
          .secondary_label = "first defined here",
      };
      if (!leme_config_reject_detailed(config, entry, -1, &extra,
                                       "duplicate binding %s", entry->name)) {
        free(binding_directives);
        return false;
      }
      continue;
    }
    if (!leme_config_parse_command(&binding->command, entry, path, error)) {
      free(*error);
      *error = NULL;
      if (!leme_config_reject(config, entry, 0, "invalid command for %s",
                              entry->name)) {
        free(binding_directives);
        return false;
      }
      continue;
    }
    binding_directives[group->binding_count] = entry;
    group->binding_count++;
  }
  free(binding_directives);
  return true;
}

static bool leme_bind_group_append(struct leme_bind_group *group,
                                   const struct leme_binding *binding,
                                   const char *origin, const char *path,
                                   char **error) {
  const struct leme_binding **resolved;
  const char **origins;
  size_t index;

  for (index = 0; index < group->resolved_count; index++) {
    if (group->resolved[index]->modifiers != binding->modifiers ||
        group->resolved[index]->keysym != binding->keysym) {
      continue;
    }
    if (group->origin[index] == origin) {
      return true;
    }
    if (origin != group->name) {
      leme_config_set_error(
          error, "%s:%d: groups %s and %s both bind the same key in %s", path,
          group->lineno, group->origin[index], origin, group->name);
      return false;
    }
    group->resolved[index] = binding;
    group->origin[index] = origin;
    return true;
  }
  resolved =
      realloc(group->resolved, (group->resolved_count + 1) * sizeof(*resolved));
  if (resolved == NULL) {
    leme_config_set_error(error, "%s:%d: out of memory", path, group->lineno);
    return false;
  }
  group->resolved = resolved;
  origins =
      realloc(group->origin, (group->resolved_count + 1) * sizeof(*origins));
  if (origins == NULL) {
    leme_config_set_error(error, "%s:%d: out of memory", path, group->lineno);
    return false;
  }
  group->origin = origins;
  group->resolved[group->resolved_count] = binding;
  group->origin[group->resolved_count] = origin;
  group->resolved_count++;
  return true;
}

static bool leme_bind_group_resolve(struct leme_bind_scope *scope,
                                    struct leme_bind_group *group,
                                    const char *path, char **error) {
  size_t index;
  size_t entry;

  if (group->state == LEME_BIND_RESOLVED) {
    return true;
  }
  if (group->state == LEME_BIND_RESOLVING) {
    leme_config_set_error(error, "%s:%d: bind group %s inherits itself", path,
                          group->lineno, group->name);
    return false;
  }
  group->state = LEME_BIND_RESOLVING;
  for (index = 0; index < group->inherit_count; index++) {
    struct leme_bind_group *parent =
        leme_bind_scope_find(scope, group->inherits[index]);

    if (parent == NULL) {
      leme_config_set_error(error, "%s:%d: %s inherits unknown bind group %s",
                            path, group->lineno, group->name,
                            group->inherits[index]);
      return false;
    }
    if (parent->is_mode) {
      leme_config_set_error(error,
                            "%s:%d: %s is a binding mode, not a bind group",
                            path, group->lineno, parent->name);
      return false;
    }
    if (parent->state == LEME_BIND_RESOLVING) {
      leme_config_set_error(
          error,
          parent == group
              ? "%s:%d: bind group %s inherits itself"
              : "%s:%d: bind groups %s and %s form an inheritance cycle",
          path, group->lineno, group->name, parent->name);
      return false;
    }
    if (!leme_bind_group_resolve(scope, parent, path, error)) {
      return false;
    }
    for (entry = 0; entry < parent->resolved_count; entry++) {
      if (!leme_bind_group_append(group, parent->resolved[entry],
                                  parent->origin[entry], path, error)) {
        return false;
      }
    }
  }
  for (index = 0; index < group->binding_count; index++) {
    if (!leme_bind_group_append(group, &group->bindings[index], group->name,
                                path, error)) {
      return false;
    }
  }
  group->state = LEME_BIND_RESOLVED;
  return true;
}

static bool leme_config_materialize_mode(struct leme_config *config,
                                         const struct leme_bind_group *group,
                                         const char *path, char **error) {
  struct leme_mode *modes;
  struct leme_mode *mode;
  size_t index;
  bool has_exit = false;

  modes = realloc(config->modes, (config->mode_count + 1) * sizeof(*modes));
  if (modes == NULL) {
    leme_config_set_error(error, "%s:%d: out of memory", path, group->lineno);
    return false;
  }
  config->modes = modes;
  mode = &config->modes[config->mode_count];
  *mode = (struct leme_mode){0};
  mode->escape_exits = group->escape_exits;
  mode->name = strdup(group->name);
  if (mode->name == NULL) {
    leme_config_set_error(error, "%s:%d: out of memory", path, group->lineno);
    return false;
  }
  config->mode_count++;
  if (group->resolved_count == 0) {
    return true;
  }
  mode->bindings = calloc(group->resolved_count, sizeof(*mode->bindings));
  if (mode->bindings == NULL) {
    leme_config_set_error(error, "%s:%d: out of memory", path, group->lineno);
    return false;
  }
  for (index = 0; index < group->resolved_count; index++) {
    const struct leme_binding *source = group->resolved[index];

    mode->bindings[index].modifiers = source->modifiers;
    mode->bindings[index].keysym = source->keysym;
    mode->bindings[index].lineno = source->lineno;
    if (!leme_config_copy_command(&mode->bindings[index].command,
                                  &source->command)) {
      leme_config_set_error(error, "%s:%d: out of memory", path, group->lineno);
      return false;
    }
    mode->binding_count++;
    if (source->command.type == LEME_COMMAND_SET_MODE) {
      has_exit = true;
    }
  }
  if (!mode->escape_exits && !has_exit) {
    leme_config_set_error(
        error, "%s:%d: mode %s keeps Escape but binds no mode command", path,
        group->lineno, mode->name);
    return false;
  }
  return true;
}

static bool leme_config_resolve_binds(struct leme_config *config,
                                      struct leme_bind_scope *scope,
                                      const char *path, char **error) {
  size_t index;

  for (index = 0; index < scope->count; index++) {
    if (!scope->groups[index].is_mode) {
      continue;
    }
    if (!leme_bind_group_resolve(scope, &scope->groups[index], path, error)) {
      return false;
    }
    if (!leme_config_materialize_mode(config, &scope->groups[index], path,
                                      error)) {
      return false;
    }
  }
  return true;
}

static void leme_bind_scope_finish(struct leme_bind_scope *scope) {
  size_t index;

  for (index = 0; index < scope->count; index++) {
    struct leme_bind_group *group = &scope->groups[index];
    size_t entry;

    for (entry = 0; entry < group->binding_count; entry++) {
      leme_config_finish_command(&group->bindings[entry].command);
    }
    for (entry = 0; entry < group->inherit_count; entry++) {
      free(group->inherits[entry]);
    }
    free(group->bindings);
    free(group->inherits);
    free(group->resolved);
    free(group->origin);
    free(group->name);
  }
  free(scope->groups);
  *scope = (struct leme_bind_scope){0};
}

static bool leme_config_parse_env(struct leme_config *config,
                                  const struct leme_scfg_directive *directive,
                                  const char *path, char **error) {
  const struct leme_scfg_directive **env_directives = NULL;
  size_t index;

  if (directive->params_len != 0) {
    leme_config_set_error(error, "%s:%d: env takes no arguments", path,
                          directive->lineno);
    return false;
  }
  for (index = 0; index < directive->children.directives_len; index++) {
    const struct leme_scfg_directive *entry =
        &directive->children.directives[index];
    const struct leme_scfg_directive **dirs;
    struct leme_environment *environment;
    size_t previous;
    bool duplicate = false;

    if (entry->params_len != 1 || entry->children.directives_len != 0 ||
        entry->name[0] == '\0' || strchr(entry->name, '=') != NULL) {
      if (!leme_config_reject(
              config, entry, -1,
              "environment entry requires a valid name and one value")) {
        free(env_directives);
        return false;
      }
      continue;
    }
    for (previous = 0; previous < config->environment_count; previous++) {
      if (strcmp(config->environment[previous].name, entry->name) == 0) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      const struct leme_reject_extra extra = {
          .secondary = env_directives[previous],
          .secondary_label = "first defined here",
      };
      if (!leme_config_reject_detailed(config, entry, -1, &extra,
                                       "duplicate environment variable %s",
                                       entry->name)) {
        free(env_directives);
        return false;
      }
      continue;
    }
    environment = realloc(config->environment, (config->environment_count + 1) *
                                                   sizeof(*environment));
    if (environment == NULL) {
      free(env_directives);
      leme_config_set_error(error, "%s:%d: out of memory", path, entry->lineno);
      return false;
    }
    config->environment = environment;
    dirs = realloc(env_directives,
                   (config->environment_count + 1) * sizeof(*dirs));
    if (dirs == NULL) {
      free(env_directives);
      leme_config_set_error(error, "%s:%d: out of memory", path, entry->lineno);
      return false;
    }
    env_directives = dirs;
    environment = &config->environment[config->environment_count];
    *environment = (struct leme_environment){
        .name = strdup(entry->name),
        .value = strdup(entry->params[0]),
    };
    if (environment->name == NULL || environment->value == NULL) {
      free(env_directives);
      leme_config_set_error(error, "%s:%d: out of memory", path, entry->lineno);
      return false;
    }
    env_directives[config->environment_count] = entry;
    config->environment_count++;
  }
  free(env_directives);
  return true;
}

static bool leme_config_parse_exec(struct leme_config *config,
                                   const struct leme_scfg_directive *directive,
                                   const char *path, char **error) {
  size_t index;

  if (directive->params_len != 0) {
    leme_config_set_error(error, "%s:%d: exec takes no arguments", path,
                          directive->lineno);
    return false;
  }
  for (index = 0; index < directive->children.directives_len; index++) {
    const struct leme_scfg_directive *entry =
        &directive->children.directives[index];
    struct leme_startup *startup;

    if (entry->children.directives_len != 0) {
      if (!leme_config_reject(config, entry, -1,
                              "exec entry cannot contain a block")) {
        return false;
      }
      continue;
    }
    startup = realloc(config->startup,
                      (config->startup_count + 1) * sizeof(*startup));
    if (startup == NULL) {
      leme_config_set_error(error, "%s:%d: out of memory", path, entry->lineno);
      return false;
    }
    config->startup = startup;
    startup = &config->startup[config->startup_count];
    startup->argv =
        leme_config_copy_argv(entry->name, entry->params, entry->params_len);
    if (startup->argv == NULL) {
      leme_config_set_error(error, "%s:%d: out of memory", path, entry->lineno);
      return false;
    }
    config->startup_count++;
  }
  return true;
}

bool leme_config_drop_invalid_binds(struct leme_config *config) {
  size_t mode_index;

  for (mode_index = 0; mode_index < config->mode_count; mode_index++) {
    struct leme_mode *mode = &config->modes[mode_index];
    size_t index = 0;

    while (index < mode->binding_count) {
      struct leme_binding *binding = &mode->bindings[index];

      if (leme_config_validate_command(config, &binding->command)) {
        index++;
        continue;
      }
      if (!leme_diagnostics_add(
              &config->diagnostics, binding->lineno,
              "binding in mode %s refers to something undefined", mode->name)) {
        return false;
      }
      leme_config_finish_command(&binding->command);
      memmove(binding, binding + 1,
              (mode->binding_count - index - 1) * sizeof(*binding));
      mode->binding_count--;
    }
  }
  return true;
}

static void leme_config_report_syntax(const struct leme_scfg_source *source,
                                      const char *path,
                                      const struct leme_scfg_result *parsed,
                                      char **error) {
  char *joined = NULL;
  size_t length = 0;
  size_t index;

  for (index = 0; index < parsed->error_count; index++) {
    const struct leme_scfg_error *scfg_err = &parsed->errors[index];
    int line = 0;
    int column = 0;
    leme_scfg_source_locate(source, scfg_err->span, &line, &column);
    struct leme_diagnostic diag = {
        .severity = LEME_DIAGNOSTIC_ERROR,
        .message = scfg_err->message,
        .line = line,
        .primary =
            {
                .source = 0,
                .span = scfg_err->span,
            },
    };
    char *rendered = leme_diagnostic_render_rich_tables(NULL, source, NULL,
                                                        path, &diag, false);
    size_t rendered_length;
    char *grown;

    if (rendered == NULL) {
      continue;
    }
    rendered_length = strlen(rendered);
    grown = realloc(joined, length + rendered_length + 1);
    if (grown == NULL) {
      free(rendered);
      break;
    }
    joined = grown;
    memcpy(joined + length, rendered, rendered_length);
    length += rendered_length;
    joined[length] = '\0';
    free(rendered);
  }
  if (joined == NULL) {
    leme_config_set_error(error, "%s:1: unable to parse the configuration",
                          path);
    return;
  }
  if (parsed->truncated) {
    leme_config_set_error(error, "%sfurther errors were not reported\n",
                          joined);
  } else {
    leme_config_set_error(error, "%s", joined);
  }
  free(joined);
}

struct leme_config *leme_config_load(const char *path, char **error) {
  struct leme_config *config = calloc(1, sizeof(*config));
  struct leme_scfg_source source = {0};
  struct leme_scfg_result parsed = {0};
  struct leme_scfg_block expanded = {0};
  struct leme_bind_scope scope = {0};
  struct leme_env_file env_file = {0};
  bool have_tags = false;
  bool have_style = false;
  bool have_animation = false;
  bool have_keyboard = false;
  bool have_output_policy = false;
  bool have_cursor = false;
  bool have_gestures = false;
  bool have_publication = false;
  bool have_config_errors = false;
  bool have_global_pointer = false;
  bool have_env = false;
  bool have_exec = false;
  size_t index;

  if (config == NULL) {
    leme_config_set_error(error, "%s:1: out of memory", path);
    return NULL;
  }
  config->refcount = 1;
  config->path = strdup(path);
  if (config->path == NULL) {
    leme_config_set_error(error, "%s:1: out of memory", path);
    leme_config_destroy(config);
    return NULL;
  }
  config->initial_tags = 3;
  config->max_tags = 9;
  config->config_errors = (struct leme_config_errors){
      .show = true,
      .position = LEME_BANNER_TOP,
      .timeout = 0,
  };
  config->drop_mode = LEME_DROP_MODE_SIMPLE;
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
  config->publication.activation = LEME_ACTIVATION_FOLLOW;
  leme_config_set_style_defaults(config);
  leme_config_set_output_defaults(config);
  leme_config_set_pointer_defaults(config);
  if (!leme_scfg_source_load(&source, path)) {
    leme_config_set_error(error, "%s:1: %s", path,
                          errno == ENOENT
                              ? "configuration file not found"
                              : "unable to read the configuration file");
    leme_config_destroy(config);
    return NULL;
  }
  if (!leme_scfg_parse(&source, &parsed)) {
    leme_config_report_syntax(&source, path, &parsed, error);
    leme_scfg_result_finish(&parsed);
    leme_scfg_source_finish(&source);
    leme_config_destroy(config);
    return NULL;
  }
  if (!leme_source_table_add(&config->sources, &source, path, NULL)) {
    leme_config_set_error(error, "%s:1: out of memory", path);
    leme_scfg_result_finish(&parsed);
    leme_scfg_source_finish(&source);
    leme_config_destroy(config);
    return NULL;
  }
  if (!leme_env_file_load(&env_file, path, &config->diagnostics, error)) {
    leme_scfg_result_finish(&parsed);
    leme_config_destroy(config);
    return NULL;
  }
  if (!leme_config_expand_templates(&parsed.block, &expanded,
                                    &config->diagnostics,
                                    leme_source_table_get(&config->sources, 0),
                                    path, &config->trails, &env_file, error)) {
    leme_env_file_finish(&env_file);
    leme_scfg_result_finish(&parsed);
    leme_config_destroy(config);
    return NULL;
  }
  leme_env_file_finish(&env_file);
  for (index = 0; index < expanded.directives_len; index++) {
    const struct leme_scfg_directive *directive = &expanded.directives[index];
    bool valid;

    if (strcmp(directive->name, "tags") == 0 && !have_tags) {
      have_tags = true;
      valid = leme_config_parse_tags(config, directive, path, error);
    } else if (strcmp(directive->name, "animation") == 0 && !have_animation) {
      have_animation = true;
      valid = leme_config_parse_animation(config, directive, path, error);
    } else if (strcmp(directive->name, "style") == 0 && !have_style) {
      have_style = true;
      valid = leme_config_parse_style(config, directive, path, error);
    } else if (strcmp(directive->name, "keyboard") == 0 && !have_keyboard) {
      have_keyboard = true;
      valid = leme_config_parse_keyboard(config, directive, path, error);
    } else if (strcmp(directive->name, "output") == 0) {
      valid = leme_config_parse_output(config, directive, path, error,
                                       &have_output_policy);
    } else if (strcmp(directive->name, "cursor") == 0 && !have_cursor) {
      have_cursor = true;
      valid = leme_config_parse_cursor(config, directive, path, error);
    } else if (strcmp(directive->name, "gestures") == 0 && !have_gestures) {
      have_gestures = true;
      valid = leme_config_parse_gestures(config, directive, path, error);
    } else if (strcmp(directive->name, "publication") == 0 &&
               !have_publication) {
      have_publication = true;
      valid = leme_config_parse_publication(config, directive, path, error);
    } else if (strcmp(directive->name, "pointer") == 0) {
      valid = leme_config_parse_pointer(config, directive, path, error,
                                        &have_global_pointer);
    } else if (strcmp(directive->name, "window") == 0) {
      valid = leme_config_parse_window_rule(config, directive, path, error);
    } else if (strcmp(directive->name, "scratchpad") == 0) {
      valid = leme_config_parse_scratchpad(config, directive, path, error);
    } else if (strcmp(directive->name, "binds") == 0) {
      valid = leme_config_parse_bind_block(config, &scope, directive, true,
                                           path, error);
    } else if (strcmp(directive->name, "bind_group") == 0) {
      valid = leme_config_parse_bind_block(config, &scope, directive, false,
                                           path, error);
    } else if (strcmp(directive->name, "config_errors") == 0 &&
               !have_config_errors) {
      have_config_errors = true;
      valid = leme_config_parse_config_errors(config, directive, path, error);
    } else if (strcmp(directive->name, "env") == 0 && !have_env) {
      have_env = true;
      valid = leme_config_parse_env(config, directive, path, error);
    } else if (strcmp(directive->name, "exec") == 0 && !have_exec) {
      have_exec = true;
      valid = leme_config_parse_exec(config, directive, path, error);
    } else {
      leme_config_set_error(error, "%s:%d: duplicate or unknown block %s", path,
                            directive->lineno, directive->name);
      valid = false;
    }
    if (!valid) {
      leme_bind_scope_finish(&scope);
      leme_scfg_block_finish(&expanded);
      leme_scfg_result_finish(&parsed);
      leme_config_destroy(config);
      return NULL;
    }
  }
  if (!leme_config_resolve_binds(config, &scope, path, error)) {
    leme_bind_scope_finish(&scope);
    leme_scfg_block_finish(&expanded);
    leme_scfg_result_finish(&parsed);
    leme_config_destroy(config);
    return NULL;
  }
  leme_bind_scope_finish(&scope);
  leme_scfg_block_finish(&expanded);
  leme_scfg_result_finish(&parsed);
  if (!leme_config_drop_invalid_binds(config)) {
    leme_config_set_error(error, "%s:1: out of memory", path);
    leme_config_destroy(config);
    return NULL;
  }
  if (config->keyboard_layout_count == 0 &&
      !leme_config_set_default_keyboard(config)) {
    leme_config_set_error(error, "%s:1: out of memory", path);
    leme_config_destroy(config);
    return NULL;
  }
  return config;
}
