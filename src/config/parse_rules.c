#include "config/parse-internal.h"

#include "config/config.h"
#include "config/internal.h"
#include "config/option.h"
#include "config/scfg.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/util/log.h>

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
  } else if (strcmp(text, "desktop") == 0) {
    *kind = LEME_LAYOUT_DESKTOP;
  } else {
    return false;
  }
  return true;
}

static bool
leme_config_parse_tag_rule(struct leme_config *config,
                           const struct leme_scfg_directive *directive,
                           const char *path, char **error);

enum leme_tags_seen {
  LEME_TAGS_SEEN_MAXIMUM = 1 << 0,
};

static enum leme_option_result
leme_config_parse_tag_entry(const struct leme_option_parse *parse) {
  const struct leme_config_parse_context *context = parse->data;
  const uint32_t *seen = context->state;

  if ((*seen & LEME_TAGS_SEEN_MAXIMUM) == 0) {
    return leme_option_reject(parse, -1, NULL, "tag requires maximum first");
  }
  return leme_config_parse_tag_rule(parse->config, parse->entry, context->path,
                                    context->error)
             ? LEME_OPTION_ACCEPTED
             : LEME_OPTION_FATAL;
}

_Static_assert(sizeof(enum leme_drop_mode) == sizeof(unsigned) &&
                   sizeof(enum leme_layout_kind) == sizeof(unsigned),
               "choice options are stored as unsigned");

static const struct leme_option leme_tags_options[] = {
    {.name = "initial",
     .kind = LEME_OPTION_U16,
     .offset = offsetof(struct leme_config, initial_tags),
     .min = 1,
     .max = UINT16_MAX},
    {.name = "maximum",
     .kind = LEME_OPTION_U16,
     .offset = offsetof(struct leme_config, max_tags),
     .min = 1,
     .max = 64,
     .field = LEME_TAGS_SEEN_MAXIMUM},
    {.name = "drop_mode",
     .kind = LEME_OPTION_CHOICE,
     .offset = offsetof(struct leme_config, tag_defaults.drop_mode),
     LEME_OPTION_CHOICES(leme_choice_drop_mode)},
    {.name = "layout",
     .kind = LEME_OPTION_CHOICE,
     .offset = offsetof(struct leme_config, tag_defaults.layout),
     LEME_OPTION_CHOICES(leme_choice_layout)},
    {.name = "tag",
     .kind = LEME_OPTION_CUSTOM,
     .flags = LEME_OPTION_MULTI | LEME_OPTION_REPEAT,
     .parse = leme_config_parse_tag_entry},
};

static const struct leme_option_block leme_tags_block = {
    .name = "tags",
    .options = leme_tags_options,
    .count = LEME_OPTION_COUNT(leme_tags_options),
};

bool leme_config_parse_tags(struct leme_config *config,
                            const struct leme_scfg_directive *directive,
                            const char *path, char **error) {
  uint32_t seen = 0;

  if (directive->params_len != 0) {
    leme_config_set_error(error, "%s:%d: tags takes no arguments", path,
                          directive->lineno);
    return false;
  }
  return leme_config_parse_options(config, directive, &leme_tags_block, config,
                                   &seen, &seen, path, error);
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

enum leme_scratchpad_seen {
  LEME_SCRATCHPAD_SEEN_IDENTITY = 1 << 0,
  LEME_SCRATCHPAD_SEEN_SPAWN = 1 << 1,
};

static enum leme_option_result
leme_config_parse_scratchpad_identity(const struct leme_option_parse *parse) {
  const struct leme_scfg_directive *entry = parse->entry;
  struct leme_scratchpad_config *scratchpad = parse->target;

  if (entry->params_len != 1 || entry->children.directives_len != 0 ||
      entry->params[0][0] == '\0') {
    return leme_option_reject(parse, 0, NULL,
                              "identity requires one nonempty value");
  }
  scratchpad->identity = strdup(entry->params[0]);
  return scratchpad->identity == NULL ? LEME_OPTION_FATAL
                                      : LEME_OPTION_ACCEPTED;
}

static enum leme_option_result
leme_config_parse_scratchpad_spawn(const struct leme_option_parse *parse) {
  const struct leme_scfg_directive *entry = parse->entry;
  struct leme_scratchpad_config *scratchpad = parse->target;

  if (entry->params_len == 0 || entry->params[0][0] == '\0' ||
      entry->children.directives_len != 0) {
    return leme_option_reject(parse, 0, NULL,
                              "spawn requires a nonempty command");
  }
  scratchpad->spawn = leme_config_copy_argv(entry->params[0], entry->params + 1,
                                            entry->params_len - 1);
  return scratchpad->spawn == NULL ? LEME_OPTION_FATAL : LEME_OPTION_ACCEPTED;
}

static const struct leme_option leme_scratchpad_options[] = {
    {.name = "identity",
     .kind = LEME_OPTION_CUSTOM,
     .flags = LEME_OPTION_MULTI,
     .field = LEME_SCRATCHPAD_SEEN_IDENTITY,
     .parse = leme_config_parse_scratchpad_identity},
    {.name = "spawn",
     .kind = LEME_OPTION_CUSTOM,
     .flags = LEME_OPTION_MULTI,
     .field = LEME_SCRATCHPAD_SEEN_SPAWN,
     .parse = leme_config_parse_scratchpad_spawn},
    {.name = "width",
     .kind = LEME_OPTION_DECIMAL,
     .offset = offsetof(struct leme_scratchpad_config, width),
     .min = 0.1,
     .max = 1.0},
    {.name = "height",
     .kind = LEME_OPTION_DECIMAL,
     .offset = offsetof(struct leme_scratchpad_config, height),
     .min = 0.1,
     .max = 1.0},
    {.name = "spawn_timeout",
     .kind = LEME_OPTION_INT,
     .offset = offsetof(struct leme_scratchpad_config, spawn_timeout_ms),
     .min = 1000,
     .max = 600000,
     .unit = "ms"},
};

static const struct leme_option_block leme_scratchpad_block = {
    .name = "scratchpad",
    .options = leme_scratchpad_options,
    .count = LEME_OPTION_COUNT(leme_scratchpad_options),
};

bool leme_config_parse_scratchpad(struct leme_config *config,
                                  const struct leme_scfg_directive *directive,
                                  const char *path, char **error) {
  const uint32_t required =
      LEME_SCRATCHPAD_SEEN_IDENTITY | LEME_SCRATCHPAD_SEEN_SPAWN;
  struct leme_scratchpad_config scratchpad = {
      .width = 0.6,
      .height = 0.6,
      .spawn_timeout_ms = 10000,
      .line = directive->lineno,
  };
  struct leme_config_parse_context context = {.path = path, .error = error};
  uint32_t seen = 0;

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
  const enum leme_option_result result = leme_option_parse_block(
      config, directive, &leme_scratchpad_block, &scratchpad, &seen, &context);
  if (result == LEME_OPTION_FATAL) {
    leme_config_finish_scratchpad(&scratchpad);
    leme_config_set_error(error, "%s:%d: out of memory", path,
                          directive->lineno);
    return false;
  }
  bool complete = result == LEME_OPTION_ACCEPTED;

  if ((seen & required) != required) {
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

static const struct leme_option leme_window_options[] = {
    {.name = "title",
     .kind = LEME_OPTION_STRING,
     .offset = offsetof(struct leme_window_rule, title)},
    {.name = "tag",
     .kind = LEME_OPTION_U16,
     .offset = offsetof(struct leme_window_rule, tag_id),
     .min = 1,
     .max = UINT16_MAX,
     .field = LEME_WINDOW_RULE_TAG},
    {.name = "floating",
     .kind = LEME_OPTION_BOOL,
     .offset = offsetof(struct leme_window_rule, floating),
     .field = LEME_WINDOW_RULE_FLOATING},
    {.name = "fullscreen",
     .kind = LEME_OPTION_BOOL,
     .offset = offsetof(struct leme_window_rule, fullscreen),
     .field = LEME_WINDOW_RULE_FULLSCREEN},
    {.name = "output",
     .kind = LEME_OPTION_STRING,
     .offset = offsetof(struct leme_window_rule, output),
     .field = LEME_WINDOW_RULE_OUTPUT},
    {.name = "opacity",
     .kind = LEME_OPTION_DECIMAL,
     .offset = offsetof(struct leme_window_rule, opacity),
     .min = 0.0,
     .max = 1.0,
     .field = LEME_WINDOW_RULE_OPACITY},
};

static const struct leme_option_block leme_window_block = {
    .name = "window",
    .options = leme_window_options,
    .count = LEME_OPTION_COUNT(leme_window_options),
};

bool leme_config_parse_window_rule(struct leme_config *config,
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

  return leme_config_parse_options(config, directive, &leme_window_block, rule,
                                   &rule->fields, NULL, path, error);
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

_Static_assert(sizeof(enum leme_secondary_gpu) == sizeof(unsigned),
               "choice options are stored as unsigned");

static const struct leme_option leme_output_policy_options[] = {
    {.name = "cross_output_focus",
     .kind = LEME_OPTION_BOOL,
     .offset = offsetof(struct leme_config, output_policy.cross_output_focus),
     .effect = LEME_OPTION_EFFECT_STORE},
    {.name = "cross_output_move",
     .kind = LEME_OPTION_BOOL,
     .offset = offsetof(struct leme_config, output_policy.cross_output_move),
     .effect = LEME_OPTION_EFFECT_STORE},
    {.name = "cross_output_drag",
     .kind = LEME_OPTION_BOOL,
     .offset = offsetof(struct leme_config, output_policy.cross_output_drag),
     .effect = LEME_OPTION_EFFECT_STORE},
    {.name = "warp_cursor",
     .kind = LEME_OPTION_BOOL,
     .offset = offsetof(struct leme_config, output_policy.warp_cursor),
     .effect = LEME_OPTION_EFFECT_STORE},
    {.name = "secondary_gpu",
     .kind = LEME_OPTION_CHOICE,
     .offset = offsetof(struct leme_config, output_policy.secondary_gpu),
     LEME_OPTION_CHOICES(leme_choice_secondary_gpu)},
};

const struct leme_option_block leme_output_policy_block = {
    .name = "output",
    .options = leme_output_policy_options,
    .count = LEME_OPTION_COUNT(leme_output_policy_options),
};

static bool
leme_config_parse_output_policy(struct leme_config *config,
                                const struct leme_scfg_directive *directive,
                                const char *path, char **error) {
  return leme_config_parse_options(config, directive, &leme_output_policy_block,
                                   config, NULL, NULL, path, error);
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

bool leme_config_parse_output(struct leme_config *config,
                              const struct leme_scfg_directive *directive,
                              const char *path, char **error,
                              bool *have_policy) {
  enum {
    OUTPUT_MODE = 1 << 0,
    OUTPUT_REFRESH = 1 << 1,
    OUTPUT_SCALE = 1 << 2,
    OUTPUT_TRANSFORM = 1 << 3,
    OUTPUT_POSITION = 1 << 4,
    OUTPUT_RELATION = 1 << 5,
    OUTPUT_ENABLED = 1 << 6,
    OUTPUT_ADAPTIVE_SYNC = 1 << 7,
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
    } else if (strcmp(entry->name, "enabled") == 0) {
      bool enabled;

      field = OUTPUT_ENABLED;
      if (!leme_config_parse_boolean(entry->params[0], &enabled)) {
        leme_config_set_error(error,
                              "%s:%d: output enabled must be true or false",
                              path, entry->lineno);
        return false;
      }
      target->disabled = !enabled;
    } else if (strcmp(entry->name, "adaptive_sync") == 0) {
      field = OUTPUT_ADAPTIVE_SYNC;
      if (!leme_config_parse_boolean(entry->params[0],
                                     &target->adaptive_sync)) {
        leme_config_set_error(
            error, "%s:%d: output adaptive_sync must be true or false", path,
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
