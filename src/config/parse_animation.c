#include "config/parse-internal.h"

#include "config/config.h"
#include "config/internal.h"
#include "config/option.h"
#include "config/scfg.h"

#include <stdio.h>
#include <string.h>

#define LEME_ANIMATION_DURATION_MAX 1000u

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

struct leme_spring_parse {
  struct leme_animation_spring spring;
  const struct leme_scfg_directive *seen[3];
};

static enum leme_option_result
leme_config_parse_spring_value(const struct leme_option_parse *parse) {
  struct leme_spring_parse *state = parse->target;
  const size_t key = (size_t)(parse->option - parse->block->options);
  double value = 0.0;

  if (parse->entry->params_len != 1 ||
      !leme_config_parse_decimal(parse->entry->params[0], &value)) {
    return leme_option_reject(parse, -1, NULL, "`%s` expects one decimal",
                              parse->option->name);
  }
  state->seen[key] = parse->entry;
  *(double *)((char *)&state->spring + parse->option->offset) = value;
  return LEME_OPTION_ACCEPTED;
}

static const struct leme_option leme_spring_options[] = {
    {.name = "damping_ratio",
     .kind = LEME_OPTION_CUSTOM,
     .offset = offsetof(struct leme_animation_spring, damping_ratio),
     .flags = LEME_OPTION_MULTI,
     .parse = leme_config_parse_spring_value},
    {.name = "stiffness",
     .kind = LEME_OPTION_CUSTOM,
     .offset = offsetof(struct leme_animation_spring, stiffness),
     .flags = LEME_OPTION_MULTI,
     .parse = leme_config_parse_spring_value},
    {.name = "epsilon",
     .kind = LEME_OPTION_CUSTOM,
     .offset = offsetof(struct leme_animation_spring, epsilon),
     .flags = LEME_OPTION_MULTI,
     .parse = leme_config_parse_spring_value},
};

static const struct leme_option_block leme_spring_block = {
    .name = "spring",
    .options = leme_spring_options,
    .count = LEME_OPTION_COUNT(leme_spring_options),
};

static bool leme_config_parse_spring(struct leme_config *config,
                                     const struct leme_scfg_directive *entry,
                                     enum leme_animation_kind *kind,
                                     struct leme_animation_spring *out) {
  struct leme_spring_parse state = {
      .spring =
          {
              .damping_ratio = 1.0,
              .stiffness = 800.0,
              .epsilon = 0.0001,
          },
  };
  uint32_t settle;
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
  const enum leme_option_result result = leme_option_parse_block(
      config, entry, &leme_spring_block, &state, NULL, NULL);
  if (result == LEME_OPTION_FATAL) {
    return false;
  }
  ok = ok && result == LEME_OPTION_ACCEPTED;
  if (ok && !(state.spring.damping_ratio > 0.0)) {
    const struct leme_reject_extra extra = {
        .help = "a spring with no damping never settles",
    };
    if (!leme_config_reject_detailed(
            config, state.seen[0] != NULL ? state.seen[0] : entry,
            state.seen[0] != NULL ? 0 : -1, &extra,
            "damping_ratio must be greater than zero")) {
      return false;
    }
    ok = false;
  }
  if (ok && !(state.spring.stiffness > 0.0)) {
    if (!leme_config_reject(config,
                            state.seen[1] != NULL ? state.seen[1] : entry,
                            state.seen[1] != NULL ? 0 : -1,
                            "stiffness must be greater than zero")) {
      return false;
    }
    ok = false;
  }
  if (ok && !(state.spring.epsilon > 0.0 && state.spring.epsilon < 1.0)) {
    const struct leme_reject_extra extra = {
        .help = "epsilon is an amplitude, and must sit between 0 and 1",
    };
    if (!leme_config_reject_detailed(
            config, state.seen[2] != NULL ? state.seen[2] : entry,
            state.seen[2] != NULL ? 0 : -1, &extra,
            "epsilon must be above 0 and below 1")) {
      return false;
    }
    ok = false;
  }
  if (ok && state.spring.epsilon < 0.000001) {
    state.spring.epsilon = 0.000001;
  }
  if (!ok) {
    return true;
  }
  settle = leme_animation_spring_duration_ms(&state.spring, 0.0);
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
  *out = state.spring;
  return true;
}

enum leme_animation_seen {
  LEME_ANIMATION_SEEN_OPACITY_CURVE = 1 << 0,
};

static enum leme_option_result
leme_config_parse_curve_option(const struct leme_option_parse *parse) {
  const struct leme_reject_extra extra = {
      .help = "valid presets are `linear`, `ease_in`, `ease_out`, and "
              "`ease_in_out`",
  };
  struct leme_animation_curve curve = {0};

  if (parse->entry->children.directives_len != 0) {
    return leme_option_reject(parse, -1, NULL, "`%s` takes no block",
                              parse->option->name);
  }
  if (!leme_config_parse_animation_curve(parse->entry, &curve)) {
    return leme_option_reject(parse, 0, &extra,
                              "`%s` expects a preset name or four decimals",
                              parse->option->name);
  }
  memcpy((char *)parse->target + parse->option->offset, &curve, sizeof(curve));
  return LEME_OPTION_ACCEPTED;
}

static enum leme_option_result
leme_config_parse_duration_option(const struct leme_option_parse *parse) {
  int value = 0;

  if (!leme_config_parse_nonnegative(parse->entry->params[0], &value)) {
    return leme_option_reject(parse, 0, NULL,
                              "`duration` expects a nonnegative integer");
  }
  if ((uint32_t)value > LEME_ANIMATION_DURATION_MAX) {
    if (!leme_config_reject(parse->config, parse->entry, 0,
                            "duration is clamped to %u milliseconds",
                            LEME_ANIMATION_DURATION_MAX)) {
      return LEME_OPTION_FATAL;
    }
    value = (int)LEME_ANIMATION_DURATION_MAX;
  }
  *(uint32_t *)((char *)parse->target + parse->option->offset) =
      (uint32_t)value;
  return LEME_OPTION_ACCEPTED;
}

static enum leme_option_result
leme_config_parse_effect_option(const struct leme_option_parse *parse) {
  struct leme_animation_settings *settings = parse->target;
  uint32_t effects = 0;

  if (parse->entry->children.directives_len != 0) {
    return leme_option_reject(parse, -1, NULL, "`effect` takes no block");
  }
  if (parse->entry->params_len == 0 ||
      !leme_config_parse_animation_effects(parse->entry, &effects)) {
    return leme_option_reject(parse, 0, NULL,
                              "`effect` expects `fade`, `scale` or `none`");
  }
  settings->effects = effects;
  return LEME_OPTION_ACCEPTED;
}

static enum leme_option_result
leme_config_parse_event_spring(const struct leme_option_parse *parse) {
  struct leme_animation_settings *settings = parse->target;

  return leme_config_parse_spring(parse->config, parse->entry, &settings->kind,
                                  &settings->spring)
             ? LEME_OPTION_ACCEPTED
             : LEME_OPTION_FATAL;
}

static enum leme_option_result
leme_config_parse_not_applicable(const struct leme_option_parse *parse) {
  return leme_option_reject(parse, -1, NULL,
                            "`%s` does not apply to the `%s` animation",
                            parse->option->name, parse->block->name);
}

static const struct leme_option leme_animation_window_options[] = {
    {.name = "effect",
     .kind = LEME_OPTION_CUSTOM,
     .flags = LEME_OPTION_MULTI,
     .parse = leme_config_parse_effect_option},
    {.name = "curve",
     .kind = LEME_OPTION_CUSTOM,
     .offset = offsetof(struct leme_animation_settings, curve),
     .flags = LEME_OPTION_MULTI,
     .parse = leme_config_parse_curve_option},
    {.name = "opacity_curve",
     .kind = LEME_OPTION_CUSTOM,
     .offset = offsetof(struct leme_animation_settings, opacity_curve),
     .flags = LEME_OPTION_MULTI,
     .field = LEME_ANIMATION_SEEN_OPACITY_CURVE,
     .parse = leme_config_parse_curve_option},
    {.name = "duration",
     .kind = LEME_OPTION_CUSTOM,
     .offset = offsetof(struct leme_animation_settings, duration_ms),
     .parse = leme_config_parse_duration_option},
    {.name = "scale_from",
     .kind = LEME_OPTION_DECIMAL,
     .offset = offsetof(struct leme_animation_settings, scale_from),
     .min = 0.1,
     .max = 2.0},
    {.name = "spring",
     .kind = LEME_OPTION_CUSTOM,
     .flags = LEME_OPTION_MULTI,
     .parse = leme_config_parse_event_spring},
};

static const struct leme_option leme_animation_move_options[] = {
    {.name = "effect",
     .kind = LEME_OPTION_CUSTOM,
     .flags = LEME_OPTION_MULTI,
     .parse = leme_config_parse_not_applicable},
    {.name = "curve",
     .kind = LEME_OPTION_CUSTOM,
     .offset = offsetof(struct leme_animation_settings, curve),
     .flags = LEME_OPTION_MULTI,
     .parse = leme_config_parse_curve_option},
    {.name = "opacity_curve",
     .kind = LEME_OPTION_CUSTOM,
     .flags = LEME_OPTION_MULTI,
     .parse = leme_config_parse_not_applicable},
    {.name = "duration",
     .kind = LEME_OPTION_CUSTOM,
     .offset = offsetof(struct leme_animation_settings, duration_ms),
     .parse = leme_config_parse_duration_option},
    {.name = "scale_from",
     .kind = LEME_OPTION_CUSTOM,
     .flags = LEME_OPTION_MULTI,
     .parse = leme_config_parse_not_applicable},
    {.name = "spring",
     .kind = LEME_OPTION_CUSTOM,
     .flags = LEME_OPTION_MULTI,
     .parse = leme_config_parse_event_spring},
};

static bool leme_config_parse_animation_event(
    struct leme_config *config, const struct leme_scfg_directive *directive,
    bool window_effects, struct leme_animation_settings *settings) {
  const struct leme_option_block block = {
      .name = directive->name,
      .options = window_effects ? leme_animation_window_options
                                : leme_animation_move_options,
      .count = LEME_OPTION_COUNT(leme_animation_window_options),
  };
  uint32_t seen = 0;

  settings->duration_ms = 150;
  settings->curve = leme_animation_curve_preset(LEME_ANIMATION_CURVE_EASE_OUT);
  settings->opacity_curve = settings->curve;
  settings->effects = window_effects ? LEME_ANIMATION_EFFECT_FADE : 0;
  settings->scale_from = window_effects ? 0.92 : 1.0;
  settings->kind = LEME_ANIMATION_KIND_EASING;
  settings->spring = (struct leme_animation_spring){0};
  if (leme_option_parse_block(config, directive, &block, settings, &seen,
                              NULL) == LEME_OPTION_FATAL) {
    return false;
  }
  if ((seen & LEME_ANIMATION_SEEN_OPACITY_CURVE) == 0) {
    settings->opacity_curve = settings->curve;
  }
  settings->configured = (settings->kind == LEME_ANIMATION_KIND_SPRING ||
                          settings->duration_ms > 0) &&
                         (!window_effects || settings->effects != 0);
  return true;
}

static enum leme_option_result
leme_config_parse_workspace_spring(const struct leme_option_parse *parse) {
  struct leme_workspace_animation_settings *settings = parse->target;

  return leme_config_parse_spring(parse->config, parse->entry, &settings->kind,
                                  &settings->spring)
             ? LEME_OPTION_ACCEPTED
             : LEME_OPTION_FATAL;
}

static enum leme_option_result leme_config_parse_workspace_opacity_curve(
    const struct leme_option_parse *parse) {
  return leme_option_reject(parse, -1, NULL,
                            "opacity_curve is not supported in workspace "
                            "animation; opacity follows transition travel");
}

_Static_assert(sizeof(enum leme_workspace_animation_style) == sizeof(unsigned),
               "choice options are stored as unsigned");

static const struct leme_option leme_workspace_animation_options[] = {
    {.name = "curve",
     .kind = LEME_OPTION_CUSTOM,
     .offset = offsetof(struct leme_workspace_animation_settings, curve),
     .flags = LEME_OPTION_MULTI,
     .parse = leme_config_parse_curve_option},
    {.name = "duration",
     .kind = LEME_OPTION_CUSTOM,
     .offset = offsetof(struct leme_workspace_animation_settings, duration_ms),
     .parse = leme_config_parse_duration_option},
    {.name = "style",
     .kind = LEME_OPTION_CHOICE,
     .offset = offsetof(struct leme_workspace_animation_settings, style),
     LEME_OPTION_CHOICES(leme_choice_workspace_style)},
    {.name = "distance",
     .kind = LEME_OPTION_DECIMAL,
     .offset = offsetof(struct leme_workspace_animation_settings, distance),
     .min = 0.0,
     .max = 1.0},
    {.name = "spring",
     .kind = LEME_OPTION_CUSTOM,
     .flags = LEME_OPTION_MULTI,
     .parse = leme_config_parse_workspace_spring},
    {.name = "opacity_curve",
     .kind = LEME_OPTION_CUSTOM,
     .flags = LEME_OPTION_MULTI,
     .parse = leme_config_parse_workspace_opacity_curve},
};

static const struct leme_option_block leme_workspace_animation_block = {
    .name = "workspace",
    .options = leme_workspace_animation_options,
    .count = LEME_OPTION_COUNT(leme_workspace_animation_options),
};

static bool leme_config_parse_workspace_animation(
    struct leme_config *config, const struct leme_scfg_directive *directive,
    struct leme_workspace_animation_settings *settings) {
  *settings = (struct leme_workspace_animation_settings){
      .configured = true,
      .duration_ms = 180,
      .style = LEME_WORKSPACE_ANIMATION_GLIDE_FADE,
      .distance = 0.15,
      .curve = leme_animation_curve_preset(LEME_ANIMATION_CURVE_EASE_OUT),
  };
  if (leme_option_parse_block(config, directive,
                              &leme_workspace_animation_block, settings, NULL,
                              NULL) == LEME_OPTION_FATAL) {
    return false;
  }
  settings->opacity_curve = settings->curve;
  settings->configured =
      settings->kind == LEME_ANIMATION_KIND_SPRING || settings->duration_ms > 0;
  return true;
}

static enum leme_option_result
leme_config_parse_animation_block(const struct leme_option_parse *parse) {
  const struct leme_scfg_directive *entry = parse->entry;
  struct leme_config *config = parse->config;

  if (entry->params_len != 0) {
    return leme_option_reject(parse, -1, NULL, "malformed %s block",
                              entry->name);
  }
  if (strcmp(entry->name, "workspace") == 0) {
    return leme_config_parse_workspace_animation(config, entry,
                                                 &config->workspace_animation)
               ? LEME_OPTION_ACCEPTED
               : LEME_OPTION_FATAL;
  }
  const enum leme_animation_event event =
      strcmp(entry->name, "open") == 0    ? LEME_ANIMATION_OPEN
      : strcmp(entry->name, "close") == 0 ? LEME_ANIMATION_CLOSE
                                          : LEME_ANIMATION_MOVE;
  return leme_config_parse_animation_event(config, entry,
                                           event != LEME_ANIMATION_MOVE,
                                           &config->animation[event])
             ? LEME_OPTION_ACCEPTED
             : LEME_OPTION_FATAL;
}

static const struct leme_option leme_animation_options[] = {
    {.name = "workspace",
     .kind = LEME_OPTION_CUSTOM,
     .flags = LEME_OPTION_MULTI,
     .parse = leme_config_parse_animation_block},
    {.name = "open",
     .kind = LEME_OPTION_CUSTOM,
     .flags = LEME_OPTION_MULTI,
     .parse = leme_config_parse_animation_block},
    {.name = "close",
     .kind = LEME_OPTION_CUSTOM,
     .flags = LEME_OPTION_MULTI,
     .parse = leme_config_parse_animation_block},
    {.name = "move",
     .kind = LEME_OPTION_CUSTOM,
     .flags = LEME_OPTION_MULTI,
     .parse = leme_config_parse_animation_block},
};

static const struct leme_option_block leme_animation_block = {
    .name = "animation",
    .options = leme_animation_options,
    .count = LEME_OPTION_COUNT(leme_animation_options),
};

bool leme_config_parse_animation(struct leme_config *config,
                                 const struct leme_scfg_directive *directive,
                                 const char *path, char **error) {
  if (directive->params_len != 0) {
    leme_config_set_error(error, "%s:%d: animation takes no arguments", path,
                          directive->lineno);
    return false;
  }
  return leme_config_parse_options(config, directive, &leme_animation_block,
                                   config, NULL, NULL, path, error);
}
