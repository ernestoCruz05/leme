#include "config/parse-internal.h"

#include "config/config.h"
#include "config/envfile.h"
#include "config/expand.h"
#include "config/internal.h"
#include "config/option.h"
#include "config/render.h"
#include "config/scfg.h"
#include "config/source_table.h"

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

const char *leme_config_nearest_key(const char *written,
                                    const char *const *keys, size_t count) {
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

bool leme_config_reject_detailed(struct leme_config *config,
                                 const struct leme_scfg_directive *directive,
                                 int param,
                                 const struct leme_reject_extra *extra,
                                 const char *format, ...) {
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

bool leme_config_reject(struct leme_config *config,
                        const struct leme_scfg_directive *directive, int param,
                        const char *format, ...) {
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

bool leme_config_parse_nonnegative(const char *text, int *value) {
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

bool leme_config_parse_options(struct leme_config *config,
                               const struct leme_scfg_directive *directive,
                               const struct leme_option_block *block,
                               void *target, uint32_t *fields, void *state,
                               const char *path, char **error) {
  struct leme_config_parse_context context = {
      .path = path,
      .error = error,
      .state = state,
  };

  if (leme_option_parse_block(config, directive, block, target, fields,
                              &context) == LEME_OPTION_FATAL) {
    if (error != NULL && *error == NULL) {
      leme_config_set_error(error, "%s:%d: out of memory", path,
                            directive->lineno);
    }
    return false;
  }
  return true;
}

bool leme_config_parse_color(const char *text, float value[4]) {
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

bool leme_config_parse_decimal(const char *text, double *value) {
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

static enum leme_option_result
leme_config_parse_gap_outer(const struct leme_option_parse *parse) {
  const struct leme_scfg_directive *entry = parse->entry;
  int sides[4] = {0};
  bool valid = entry->params_len >= 1 && entry->params_len <= 4 &&
               entry->children.directives_len == 0;

  for (size_t arg = 0; valid && arg < entry->params_len; ++arg) {
    valid = leme_config_parse_nonnegative(entry->params[arg], &sides[arg]);
  }
  if (!valid) {
    return leme_option_reject(
        parse, -1, NULL, "gap_outer requires one to four nonnegative integers");
  }
  parse->config->gap_outer = (struct leme_gaps){
      .top = sides[0],
      .right = entry->params_len >= 2 ? sides[1] : sides[0],
      .bottom = entry->params_len >= 3 ? sides[2] : sides[0],
      .left = entry->params_len == 4   ? sides[3]
              : entry->params_len >= 2 ? sides[1]
                                       : sides[0],
  };
  return LEME_OPTION_ACCEPTED;
}

_Static_assert(sizeof(enum leme_fullscreen_coverage) == sizeof(unsigned),
               "choice options are stored as unsigned");

static const struct leme_option leme_style_options[] = {
    {.name = "gap",
     .kind = LEME_OPTION_INT,
     .offset = offsetof(struct leme_config, gap),
     .min = 0,
     .max = INT_MAX,
     .effect = LEME_OPTION_EFFECT_ARRANGE},
    {.name = "gap_outer",
     .kind = LEME_OPTION_CUSTOM,
     .flags = LEME_OPTION_MULTI,
     .parse = leme_config_parse_gap_outer},
    {.name = "smart_gaps",
     .kind = LEME_OPTION_BOOL,
     .offset = offsetof(struct leme_config, smart_gaps),
     .effect = LEME_OPTION_EFFECT_ARRANGE},
    {.name = "border_width",
     .kind = LEME_OPTION_INT,
     .offset = offsetof(struct leme_config, border_width),
     .min = 0,
     .max = INT_MAX,
     .effect = LEME_OPTION_EFFECT_ARRANGE},
    {.name = "corner_radius",
     .kind = LEME_OPTION_INT,
     .offset = offsetof(struct leme_config, corner_radius),
     .min = 0,
     .max = INT_MAX,
     .effect = LEME_OPTION_EFFECT_EFFECTS},
    {.name = "blur",
     .kind = LEME_OPTION_INT,
     .offset = offsetof(struct leme_config, blur),
     .min = 0,
     .max = 64,
     .effect = LEME_OPTION_EFFECT_EFFECTS},
    {.name = "border_active",
     .kind = LEME_OPTION_COLOR,
     .offset = offsetof(struct leme_config, border_active),
     .effect = LEME_OPTION_EFFECT_REDRAW},
    {.name = "border_inactive",
     .kind = LEME_OPTION_COLOR,
     .offset = offsetof(struct leme_config, border_inactive),
     .effect = LEME_OPTION_EFFECT_REDRAW},
    {.name = "opacity_active",
     .kind = LEME_OPTION_DECIMAL,
     .offset = offsetof(struct leme_config, opacity_active),
     .min = 0.0,
     .max = 1.0,
     .effect = LEME_OPTION_EFFECT_REDRAW},
    {.name = "opacity_inactive",
     .kind = LEME_OPTION_DECIMAL,
     .offset = offsetof(struct leme_config, opacity_inactive),
     .min = 0.0,
     .max = 1.0,
     .effect = LEME_OPTION_EFFECT_REDRAW},
    {.name = "fullscreen_covers",
     .kind = LEME_OPTION_CHOICE,
     .offset = offsetof(struct leme_config, fullscreen_covers),
     LEME_OPTION_CHOICES(leme_choice_fullscreen_covers),
     .effect = LEME_OPTION_EFFECT_FULLSCREEN},
};

const struct leme_option_block leme_style_block = {
    .name = "style",
    .options = leme_style_options,
    .count = LEME_OPTION_COUNT(leme_style_options),
};

static bool leme_config_parse_style(struct leme_config *config,
                                    const struct leme_scfg_directive *directive,
                                    const char *path, char **error) {
  if (directive->params_len != 0) {
    leme_config_set_error(error, "%s:%d: style takes no arguments", path,
                          directive->lineno);
    return false;
  }
  return leme_config_parse_options(config, directive, &leme_style_block, config,
                                   NULL, NULL, path, error);
}

static const struct leme_option leme_cursor_options[] = {
    {.name = "theme",
     .kind = LEME_OPTION_STRING,
     .offset = offsetof(struct leme_config, cursor.theme),
     .effect = LEME_OPTION_EFFECT_CURSOR},
    {.name = "size",
     .kind = LEME_OPTION_INT,
     .offset = offsetof(struct leme_config, cursor.size),
     .min = 1,
     .max = LEME_CURSOR_SIZE_MAX,
     .effect = LEME_OPTION_EFFECT_CURSOR},
};

const struct leme_option_block leme_cursor_block = {
    .name = "cursor",
    .options = leme_cursor_options,
    .count = LEME_OPTION_COUNT(leme_cursor_options),
    .flags = LEME_OPTION_BLOCK_FIRST_WINS,
};

static bool
leme_config_parse_cursor(struct leme_config *config,
                         const struct leme_scfg_directive *directive,
                         const char *path, char **error) {
  if (directive->params_len != 0) {
    leme_config_set_error(error, "%s:%d: cursor block takes no parameters",
                          path, directive->lineno);
    return false;
  }
  return leme_config_parse_options(config, directive, &leme_cursor_block,
                                   config, NULL, NULL, path, error);
}

static bool leme_config_parse_modifier_name(const char *text,
                                            uint32_t *modifier) {
  static const struct {
    const char *name;
    uint32_t modifier;
  } names[] = {{"shift", WLR_MODIFIER_SHIFT},
               {"ctrl", WLR_MODIFIER_CTRL},
               {"control", WLR_MODIFIER_CTRL},
               {"alt", WLR_MODIFIER_ALT},
               {"super", WLR_MODIFIER_LOGO}};
  for (size_t index = 0; index < LEME_ARRAY_LENGTH(names); index++) {
    if (strcasecmp(text, names[index].name) == 0) {
      *modifier = names[index].modifier;
      return true;
    }
  }
  return false;
}

static enum leme_option_result
leme_config_parse_container_modifier(const struct leme_option_parse *parse) {
  uint32_t modifier = 0;

  if (!leme_config_parse_modifier_name(parse->entry->params[0], &modifier)) {
    return leme_option_reject(
        parse, 0, NULL,
        "`container_modifier` expects `shift`, `ctrl`, `alt` or `super`");
  }
  parse->config->desktop_container_modifier = modifier;
  return LEME_OPTION_ACCEPTED;
}

static const struct leme_option leme_desktop_options[] = {
    {.name = "minimized_strip",
     .kind = LEME_OPTION_BOOL,
     .offset = offsetof(struct leme_config, desktop_minimized_strip),
     .effect = LEME_OPTION_EFFECT_LAYOUT},
    {.name = "launchers",
     .kind = LEME_OPTION_BOOL,
     .offset = offsetof(struct leme_config, desktop_launchers)},
    {.name = "container_modifier",
     .kind = LEME_OPTION_CUSTOM,
     .parse = leme_config_parse_container_modifier},
};

const struct leme_option_block leme_desktop_block = {
    .name = "desktop",
    .options = leme_desktop_options,
    .count = LEME_OPTION_COUNT(leme_desktop_options),
};

static bool
leme_config_parse_desktop(struct leme_config *config,
                          const struct leme_scfg_directive *directive,
                          const char *path, char **error) {
  if (directive->params_len != 0) {
    leme_config_set_error(error, "%s:%d: desktop block takes no parameters",
                          path, directive->lineno);
    return false;
  }
  return leme_config_parse_options(config, directive, &leme_desktop_block,
                                   config, NULL, NULL, path, error);
}

_Static_assert(sizeof(enum leme_activation_policy) == sizeof(unsigned),
               "choice options are stored as unsigned");

static const struct leme_option leme_publication_options[] = {
    {.name = "activation",
     .kind = LEME_OPTION_CHOICE,
     .offset = offsetof(struct leme_config, publication.activation),
     LEME_OPTION_CHOICES(leme_choice_activation),
     .effect = LEME_OPTION_EFFECT_STORE},
};

const struct leme_option_block leme_publication_block = {
    .name = "publication",
    .options = leme_publication_options,
    .count = LEME_OPTION_COUNT(leme_publication_options),
};

static bool
leme_config_parse_publication(struct leme_config *config,
                              const struct leme_scfg_directive *directive,
                              const char *path, char **error) {
  if (directive->params_len != 0) {
    leme_config_set_error(error, "%s:%d: publication block takes no parameters",
                          path, directive->lineno);
    return false;
  }
  return leme_config_parse_options(config, directive, &leme_publication_block,
                                   config, NULL, NULL, path, error);
}

bool leme_config_parse_boolean(const char *text, bool *value) {
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

_Static_assert(sizeof(enum leme_banner_position) == sizeof(unsigned),
               "choice options are stored as unsigned");

static const struct leme_option leme_config_errors_options[] = {
    {.name = "show",
     .kind = LEME_OPTION_BOOL,
     .offset = offsetof(struct leme_config, config_errors.show)},
    {.name = "position",
     .kind = LEME_OPTION_CHOICE,
     .offset = offsetof(struct leme_config, config_errors.position),
     LEME_OPTION_CHOICES(leme_choice_banner)},
    {.name = "timeout",
     .kind = LEME_OPTION_INT,
     .offset = offsetof(struct leme_config, config_errors.timeout),
     .min = 0,
     .max = INT_MAX},
};

const struct leme_option_block leme_config_errors_block = {
    .name = "config_errors",
    .options = leme_config_errors_options,
    .count = LEME_OPTION_COUNT(leme_config_errors_options),
};

static bool
leme_config_parse_config_errors(struct leme_config *config,
                                const struct leme_scfg_directive *directive,
                                const char *path, char **error) {
  if (directive->params_len != 0) {
    leme_config_set_error(error, "%s:%d: config_errors takes no arguments",
                          path, directive->lineno);
    return false;
  }
  return leme_config_parse_options(config, directive, &leme_config_errors_block,
                                   config, NULL, NULL, path, error);
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

struct leme_config_load_state {
  struct leme_bind_scope scope;
  const struct leme_scfg_directive *desktop_menus[LEME_DESKTOP_MENU_COUNT];
  bool have_style;
  bool have_output_policy;
  bool have_global_pointer;
};

enum leme_config_block_kind {
  LEME_CONFIG_BLOCK_ONCE,
  LEME_CONFIG_BLOCK_REPEAT,
  LEME_CONFIG_BLOCK_EXPANDED,
};

struct leme_config_block {
  const char *name;
  enum leme_config_block_kind kind;
  bool (*parse)(struct leme_config *config,
                const struct leme_scfg_directive *directive, const char *path,
                char **error);
  bool (*parse_state)(struct leme_config *config,
                      const struct leme_scfg_directive *directive,
                      struct leme_config_load_state *state, const char *path,
                      char **error);
};

static bool leme_config_parse_style_block(
    struct leme_config *config, const struct leme_scfg_directive *directive,
    struct leme_config_load_state *state, const char *path, char **error);

static bool leme_config_parse_output_block(
    struct leme_config *config, const struct leme_scfg_directive *directive,
    struct leme_config_load_state *state, const char *path, char **error) {
  return leme_config_parse_output(config, directive, path, error,
                                  &state->have_output_policy);
}

static bool leme_config_parse_pointer_block(
    struct leme_config *config, const struct leme_scfg_directive *directive,
    struct leme_config_load_state *state, const char *path, char **error) {
  return leme_config_parse_pointer(config, directive, path, error,
                                   &state->have_global_pointer);
}

static bool leme_config_parse_desktop_menu_block(
    struct leme_config *config, const struct leme_scfg_directive *directive,
    struct leme_config_load_state *state, const char *path, char **error) {
  if (!leme_config_parse_desktop_menu(config, directive,
                                      state->desktop_menus)) {
    leme_config_set_error(error, "%s:%d: out of memory", path,
                          directive->lineno);
    return false;
  }
  return true;
}

static bool leme_config_parse_binds_block(
    struct leme_config *config, const struct leme_scfg_directive *directive,
    struct leme_config_load_state *state, const char *path, char **error) {
  return leme_config_parse_bind_block(config, &state->scope, directive, true,
                                      path, error);
}

static bool leme_config_parse_bind_group_block(
    struct leme_config *config, const struct leme_scfg_directive *directive,
    struct leme_config_load_state *state, const char *path, char **error) {
  return leme_config_parse_bind_block(config, &state->scope, directive, false,
                                      path, error);
}

static const struct leme_config_block leme_config_blocks[] = {
    {.name = "tags", .parse = leme_config_parse_tags},
    {.name = "animation", .parse = leme_config_parse_animation},
    {.name = "style",
     .kind = LEME_CONFIG_BLOCK_REPEAT,
     .parse_state = leme_config_parse_style_block},
    {.name = "keyboard", .parse = leme_config_parse_keyboard},
    {.name = "output",
     .kind = LEME_CONFIG_BLOCK_REPEAT,
     .parse_state = leme_config_parse_output_block},
    {.name = "cursor", .parse = leme_config_parse_cursor},
    {.name = "gestures"},
    {.name = "publication", .parse = leme_config_parse_publication},
    {.name = "pointer",
     .kind = LEME_CONFIG_BLOCK_REPEAT,
     .parse_state = leme_config_parse_pointer_block},
    {.name = "window",
     .kind = LEME_CONFIG_BLOCK_REPEAT,
     .parse = leme_config_parse_window_rule},
    {.name = "scratchpad",
     .kind = LEME_CONFIG_BLOCK_REPEAT,
     .parse = leme_config_parse_scratchpad},
    {.name = "binds",
     .kind = LEME_CONFIG_BLOCK_REPEAT,
     .parse_state = leme_config_parse_binds_block},
    {.name = "bind_group",
     .kind = LEME_CONFIG_BLOCK_REPEAT,
     .parse_state = leme_config_parse_bind_group_block},
    {.name = "config_errors", .parse = leme_config_parse_config_errors},
    {.name = "env", .parse = leme_config_parse_env},
    {.name = "exec", .parse = leme_config_parse_exec},
    {.name = "vars", .kind = LEME_CONFIG_BLOCK_EXPANDED},
    {.name = "lists", .kind = LEME_CONFIG_BLOCK_EXPANDED},
    {.name = "desktop", .parse = leme_config_parse_desktop},
    {.name = "desktop_menu",
     .kind = LEME_CONFIG_BLOCK_REPEAT,
     .parse_state = leme_config_parse_desktop_menu_block},
};

#define LEME_CONFIG_BLOCK_COUNT LEME_OPTION_COUNT(leme_config_blocks)

static void leme_config_block_error(char **error, const char *path,
                                    const struct leme_scfg_directive *block) {
  const char *names[LEME_CONFIG_BLOCK_COUNT];
  const char *nearest;

  for (size_t index = 0; index < LEME_CONFIG_BLOCK_COUNT; index++) {
    names[index] = leme_config_blocks[index].name;
    if (strcmp(block->name, names[index]) == 0) {
      leme_config_set_error(error, "%s:%d: duplicate block %s", path,
                            block->lineno, block->name);
      return;
    }
  }
  nearest =
      leme_config_nearest_key(block->name, names, LEME_CONFIG_BLOCK_COUNT);
  if (nearest != NULL) {
    leme_config_set_error(
        error,
        "%s:%d: unknown block %s; a block with a similar name exists: `%s`",
        path, block->lineno, block->name, nearest);
  } else {
    leme_config_set_error(error, "%s:%d: unknown block %s", path, block->lineno,
                          block->name);
  }
}

static bool leme_config_parse_style_block(
    struct leme_config *config, const struct leme_scfg_directive *directive,
    struct leme_config_load_state *state, const char *path, char **error) {
  if (directive->params_len != 0) {
    return leme_config_parse_style_rule(config, directive, path, error);
  }
  if (state->have_style) {
    leme_config_block_error(error, path, directive);
    return false;
  }
  state->have_style = true;
  return leme_config_parse_style(config, directive, path, error);
}

struct leme_config *leme_config_load(const char *path, char **error) {
  struct leme_config *config = calloc(1, sizeof(*config));
  struct leme_config *loaded = NULL;
  struct leme_scfg_source source = {0};
  struct leme_scfg_result parsed = {0};
  struct leme_scfg_block expanded = {0};
  struct leme_env_file env_file = {0};
  struct leme_config_load_state state = {0};
  bool seen[LEME_CONFIG_BLOCK_COUNT] = {0};
  size_t index;

  if (config == NULL) {
    leme_config_set_error(error, "%s:1: out of memory", path);
    return NULL;
  }
  config->refcount = 1;
  config->path = strdup(path);
  if (config->path == NULL) {
    leme_config_set_error(error, "%s:1: out of memory", path);
    goto cleanup;
  }
  config->initial_tags = 3;
  config->desktop_minimized_strip = true;
  config->desktop_launchers = true;
  config->desktop_container_modifier = WLR_MODIFIER_SHIFT;
  if (!leme_config_desktop_menu_defaults(config)) {
    leme_config_set_error(error, "%s:1: out of memory", path);
    goto cleanup;
  }
  config->max_tags = 9;
  config->config_errors = (struct leme_config_errors){
      .show = true,
      .position = LEME_BANNER_TOP,
      .timeout = 0,
  };
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
  leme_config_set_style_defaults(config);
  leme_config_set_output_defaults(config);
  leme_config_set_pointer_defaults(config);
  if (!leme_scfg_source_load(&source, path)) {
    leme_config_set_error(error, "%s:1: %s", path,
                          errno == ENOENT
                              ? "configuration file not found"
                              : "unable to read the configuration file");
    goto cleanup;
  }
  if (!leme_scfg_parse(&source, &parsed)) {
    leme_config_report_syntax(&source, path, &parsed, error);
    goto cleanup;
  }
  if (!leme_source_table_add(&config->sources, &source, path, NULL)) {
    leme_config_set_error(error, "%s:1: out of memory", path);
    goto cleanup;
  }
  if (!leme_env_file_load(&env_file, path, &config->diagnostics, error) ||
      !leme_config_expand_templates(&parsed.block, &expanded,
                                    &config->diagnostics,
                                    leme_source_table_get(&config->sources, 0),
                                    path, &config->trails, &env_file, error)) {
    goto cleanup;
  }
  for (index = 0; index < expanded.directives_len; index++) {
    if (strcmp(expanded.directives[index].name, "gestures") == 0) {
      if (!leme_config_parse_gestures(config, &expanded.directives[index], path,
                                      error)) {
        goto cleanup;
      }
      break;
    }
  }
  for (index = 0; index < expanded.directives_len; index++) {
    const struct leme_scfg_directive *directive = &expanded.directives[index];
    size_t block = 0;

    while (block < LEME_CONFIG_BLOCK_COUNT &&
           strcmp(directive->name, leme_config_blocks[block].name) != 0) {
      block++;
    }
    if (block == LEME_CONFIG_BLOCK_COUNT ||
        leme_config_blocks[block].kind == LEME_CONFIG_BLOCK_EXPANDED ||
        (leme_config_blocks[block].kind == LEME_CONFIG_BLOCK_ONCE &&
         seen[block])) {
      leme_config_block_error(error, path, directive);
      goto cleanup;
    }
    seen[block] = true;
    const struct leme_config_block *spec = &leme_config_blocks[block];

    if (spec->parse != NULL && !spec->parse(config, directive, path, error)) {
      goto cleanup;
    }
    if (spec->parse_state != NULL &&
        !spec->parse_state(config, directive, &state, path, error)) {
      goto cleanup;
    }
  }
  if (!leme_config_resolve_binds(config, &state.scope, path, error)) {
    goto cleanup;
  }
  if (!leme_config_drop_invalid_binds(config) ||
      !leme_config_drop_invalid_desktop_menu(config) ||
      (config->keyboard_layout_count == 0 &&
       !leme_config_set_default_keyboard(config))) {
    leme_config_set_error(error, "%s:1: out of memory", path);
    goto cleanup;
  }
  loaded = config;

cleanup:
  leme_bind_scope_finish(&state.scope);
  leme_scfg_block_finish(&expanded);
  leme_env_file_finish(&env_file);
  leme_scfg_result_finish(&parsed);
  leme_scfg_source_finish(&source);
  if (loaded == NULL) {
    leme_config_destroy(config);
  }
  return loaded;
}
