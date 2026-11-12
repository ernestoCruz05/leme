#include "config/option.h"

#include "config/internal.h"
#include "config/scfg.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static enum leme_option_result leme_option_rejected(bool reported) {
  return reported ? LEME_OPTION_REJECTED : LEME_OPTION_FATAL;
}

enum leme_option_result
leme_option_reject(const struct leme_option_parse *parse, int param,
                   const struct leme_reject_extra *extra, const char *format,
                   ...) {
  va_list arguments;
  char message[256];

  va_start(arguments, format);
  const int written = vsnprintf(message, sizeof(message), format, arguments);
  va_end(arguments);
  if (written < 0) {
    return LEME_OPTION_FATAL;
  }
  return leme_option_rejected(leme_config_reject_detailed(
      parse->config, parse->entry, param, extra, "%s", message));
}

static bool leme_option_parse_integer(const char *text, long long *value) {
  char *end;

  errno = 0;
  const long long parsed = strtoll(text, &end, 10);
  if (errno != 0 || text[0] == '\0' || *end != '\0') {
    return false;
  }
  *value = parsed;
  return true;
}

static bool leme_option_in_bounds(const struct leme_option *option,
                                  double value) {
  const bool above = (option->flags & LEME_OPTION_MIN_OPEN) != 0
                         ? value > option->min
                         : value >= option->min;
  const bool below = (option->flags & LEME_OPTION_MAX_OPEN) != 0
                         ? value < option->max
                         : value <= option->max;
  return above && below;
}

static double leme_option_integer_limit(enum leme_option_kind kind) {
  return kind == LEME_OPTION_U16 ? UINT16_MAX : INT_MAX;
}

static void leme_option_append_unit(const struct leme_option *option, char *out,
                                    size_t size) {
  const size_t used = strlen(out);

  if (option->unit != NULL) {
    (void)snprintf(out + used, size - used, " %s", option->unit);
  }
}

static void leme_option_describe_integer(const struct leme_option *option,
                                         char *out, size_t size) {
  const bool unbounded = option->max >= leme_option_integer_limit(option->kind);

  if (unbounded && option->min == 0.0) {
    (void)snprintf(out, size, "a nonnegative integer");
  } else if (unbounded && option->min == 1.0) {
    (void)snprintf(out, size, "a positive integer");
  } else if (unbounded) {
    (void)snprintf(out, size, "an integer of at least %.0f", option->min);
  } else {
    (void)snprintf(out, size, "an integer from %.0f through %.0f", option->min,
                   option->max);
  }
  leme_option_append_unit(option, out, size);
}

static void leme_option_describe_decimal(const struct leme_option *option,
                                         char *out, size_t size) {
  const bool min_open = (option->flags & LEME_OPTION_MIN_OPEN) != 0;
  const bool max_open = (option->flags & LEME_OPTION_MAX_OPEN) != 0;
  const bool has_min = option->min != -HUGE_VAL;
  const bool has_max = option->max != HUGE_VAL;

  if (!has_min && !has_max) {
    (void)snprintf(out, size, "a decimal");
  } else if (!has_max) {
    (void)snprintf(out, size,
                   min_open ? "a decimal above %g" : "a decimal of at least %g",
                   option->min);
  } else if (!has_min) {
    (void)snprintf(out, size,
                   max_open ? "a decimal below %g" : "a decimal of at most %g",
                   option->max);
  } else if (!min_open && !max_open) {
    (void)snprintf(out, size, "a decimal from %g through %g", option->min,
                   option->max);
  } else {
    (void)snprintf(out, size, "a decimal %s %g and %s %g",
                   min_open ? "above" : "of at least", option->min,
                   max_open ? "below" : "at most", option->max);
  }
  leme_option_append_unit(option, out, size);
}

static void leme_option_describe_choices(const struct leme_option *option,
                                         char *out, size_t size) {
  size_t used = 0;

  out[0] = '\0';
  if (option->choice_count > 2) {
    const int written = snprintf(out, size, "one of ");
    assert(written > 0 && (size_t)written < size);
    used = (size_t)written;
  }
  for (size_t i = 0; i < option->choice_count; i++) {
    const char *separator = i == 0                          ? ""
                            : i + 1 == option->choice_count ? " or "
                                                            : ", ";
    const int written = snprintf(out + used, size - used, "%s`%s`", separator,
                                 option->choices[i]);
    assert(written > 0 && (size_t)written < size - used);
    used += (size_t)written;
  }
}

static enum leme_option_result
leme_option_parse_number(const struct leme_option_parse *parse,
                         const struct leme_option *option, void *slot) {
  const char *text = parse->entry->params[0];
  char expects[96];
  long long value = 0;

  if (!leme_option_parse_integer(text, &value) ||
      !leme_option_in_bounds(option, (double)value)) {
    leme_option_describe_integer(option, expects, sizeof(expects));
    return leme_option_rejected(leme_config_reject(parse->config, parse->entry,
                                                   0, "`%s` expects %s",
                                                   option->name, expects));
  }
  switch (option->kind) {
  case LEME_OPTION_U16:
    *(uint16_t *)slot = (uint16_t)value;
    break;
  case LEME_OPTION_U32:
    *(uint32_t *)slot = (uint32_t)value;
    break;
  default:
    *(int *)slot = (int)value;
    break;
  }
  return LEME_OPTION_ACCEPTED;
}

static enum leme_option_result
leme_option_parse_value(const struct leme_option_parse *parse,
                        const struct leme_option *option) {
  const char *text = parse->entry->params[0];
  void *slot = (char *)parse->target + option->offset;
  char expects[256];

  switch (option->kind) {
  case LEME_OPTION_BOOL: {
    bool value = false;

    if (!leme_config_parse_boolean(text, &value)) {
      return leme_option_rejected(
          leme_config_reject(parse->config, parse->entry, 0,
                             "`%s` expects `true` or `false`", option->name));
    }
    *(bool *)slot = value;
    return LEME_OPTION_ACCEPTED;
  }
  case LEME_OPTION_INT:
  case LEME_OPTION_U16:
  case LEME_OPTION_U32:
    return leme_option_parse_number(parse, option, slot);
  case LEME_OPTION_DECIMAL: {
    double value = 0.0;

    if (!leme_config_parse_decimal(text, &value) ||
        !leme_option_in_bounds(option, value)) {
      leme_option_describe_decimal(option, expects, sizeof(expects));
      return leme_option_rejected(
          leme_config_reject(parse->config, parse->entry, 0, "`%s` expects %s",
                             option->name, expects));
    }
    *(double *)slot = value;
    return LEME_OPTION_ACCEPTED;
  }
  case LEME_OPTION_CHOICE: {
    unsigned choice = 0;

    while (choice < option->choice_count &&
           strcmp(text, option->choices[choice]) != 0) {
      choice++;
    }
    if (choice == option->choice_count) {
      leme_option_describe_choices(option, expects, sizeof(expects));
      return leme_option_rejected(
          leme_config_reject(parse->config, parse->entry, 0, "`%s` expects %s",
                             option->name, expects));
    }
    memcpy(slot, &choice, sizeof(choice));
    return LEME_OPTION_ACCEPTED;
  }
  case LEME_OPTION_COLOR: {
    float value[4] = {0};

    if (!leme_config_parse_color(text, value)) {
      return leme_option_rejected(leme_config_reject(
          parse->config, parse->entry, 0,
          "`%s` expects `#RRGGBB` or `#RRGGBBAA`", option->name));
    }
    memcpy(slot, value, sizeof(value));
    return LEME_OPTION_ACCEPTED;
  }
  case LEME_OPTION_STRING: {
    char *copy = strdup(text);
    char **string = slot;

    if (copy == NULL) {
      return LEME_OPTION_FATAL;
    }
    free(*string);
    *string = copy;
    return LEME_OPTION_ACCEPTED;
  }
  case LEME_OPTION_CUSTOM:
    break;
  }
  assert(false);
  return LEME_OPTION_FATAL;
}

static bool
leme_option_reject_unknown(struct leme_config *config,
                           const struct leme_option_block *block,
                           const struct leme_scfg_directive *entry) {
  const char *names[LEME_OPTION_BLOCK_MAX];
  char help[128] = {0};
  struct leme_reject_extra extra = {0};

  for (size_t i = 0; i < block->count; i++) {
    names[i] = block->options[i].name;
  }
  const char *nearest =
      leme_config_nearest_key(entry->name, names, block->count);
  if (nearest != NULL) {
    (void)snprintf(help, sizeof(help),
                   "a directive with a similar name exists: `%s`", nearest);
    extra.help = help;
  }
  return leme_config_reject_detailed(config, entry, -1, &extra,
                                     "unknown directive `%s` in `%s`",
                                     entry->name, block->name);
}

enum leme_option_result
leme_option_parse_block(struct leme_config *config,
                        const struct leme_scfg_directive *directive,
                        const struct leme_option_block *block, void *target,
                        uint32_t *fields, void *data) {
  const struct leme_scfg_directive *seen[LEME_OPTION_BLOCK_MAX] = {0};
  bool rejected = false;

  assert(block->count <= LEME_OPTION_BLOCK_MAX);
  for (size_t index = 0; index < directive->children.directives_len; index++) {
    const struct leme_scfg_directive *entry =
        &directive->children.directives[index];
    size_t key = 0;

    while (key < block->count &&
           strcmp(entry->name, block->options[key].name) != 0) {
      key++;
    }
    if (key == block->count) {
      if (!leme_option_reject_unknown(config, block, entry)) {
        return LEME_OPTION_FATAL;
      }
      rejected = true;
      continue;
    }
    const struct leme_option *option = &block->options[key];
    const struct leme_option_parse parse = {
        .config = config,
        .block = block,
        .option = option,
        .entry = entry,
        .target = target,
        .data = data,
    };

    if (seen[key] != NULL && (option->flags & LEME_OPTION_REPEAT) == 0) {
      const struct leme_reject_extra extra = {
          .secondary = seen[key],
          .secondary_label = "first defined here",
      };
      if (!leme_config_reject_detailed(config, entry, -1, &extra,
                                       "duplicate directive `%s` in `%s`",
                                       option->name, block->name)) {
        return LEME_OPTION_FATAL;
      }
      rejected = true;
      continue;
    }
    if ((option->flags & LEME_OPTION_MULTI) == 0 &&
        (entry->params_len != 1 || entry->children.directives_len != 0)) {
      if (!leme_config_reject(config, entry, -1, "`%s` in `%s` takes one value",
                              option->name, block->name)) {
        return LEME_OPTION_FATAL;
      }
      rejected = true;
      continue;
    }
    const enum leme_option_result result =
        option->kind == LEME_OPTION_CUSTOM
            ? option->parse(&parse)
            : leme_option_parse_value(&parse, option);
    if (result == LEME_OPTION_FATAL) {
      return LEME_OPTION_FATAL;
    }
    rejected = rejected || result == LEME_OPTION_REJECTED;
    if (result == LEME_OPTION_ACCEPTED ||
        (block->flags & LEME_OPTION_BLOCK_FIRST_WINS) != 0) {
      seen[key] = entry;
    }
    if (result == LEME_OPTION_ACCEPTED && fields != NULL) {
      *fields |= option->field;
    }
  }
  return rejected ? LEME_OPTION_REJECTED : LEME_OPTION_ACCEPTED;
}

bool leme_option_block_valid(const struct leme_option_block *block,
                             const void *target, uint32_t fields) {
  for (size_t index = 0; index < block->count; index++) {
    const struct leme_option *option = &block->options[index];
    const void *slot = (const char *)target + option->offset;
    double value = 0.0;
    unsigned choice = 0;

    if (option->field != 0 && (fields & option->field) == 0) {
      continue;
    }
    switch (option->kind) {
    case LEME_OPTION_INT:
      value = *(const int *)slot;
      break;
    case LEME_OPTION_U16:
      value = *(const uint16_t *)slot;
      break;
    case LEME_OPTION_U32:
      value = *(const uint32_t *)slot;
      break;
    case LEME_OPTION_DECIMAL:
      value = *(const double *)slot;
      break;
    case LEME_OPTION_CHOICE:
      memcpy(&choice, slot, sizeof(choice));
      if (choice >= option->choice_count) {
        return false;
      }
      continue;
    default:
      continue;
    }
    if (!isfinite(value) || !leme_option_in_bounds(option, value)) {
      return false;
    }
  }
  return true;
}

bool leme_option_in_range(const struct leme_option *option,
                          const union leme_option_value *value) {
  switch (option->kind) {
  case LEME_OPTION_INT:
  case LEME_OPTION_U16:
  case LEME_OPTION_U32:
    return leme_option_in_bounds(option, (double)value->integer);
  case LEME_OPTION_DECIMAL:
    return isfinite(value->decimal) &&
           leme_option_in_bounds(option, value->decimal);
  case LEME_OPTION_CHOICE:
    return value->choice < option->choice_count;
  default:
    return true;
  }
}

bool leme_option_equal(const struct leme_option *option, const void *target,
                       const union leme_option_value *value) {
  const void *slot = (const char *)target + option->offset;
  unsigned choice = 0;

  switch (option->kind) {
  case LEME_OPTION_BOOL:
    return *(const bool *)slot == value->boolean;
  case LEME_OPTION_INT:
    return *(const int *)slot == value->integer;
  case LEME_OPTION_U16:
    return *(const uint16_t *)slot == value->integer;
  case LEME_OPTION_U32:
    return *(const uint32_t *)slot == value->integer;
  case LEME_OPTION_DECIMAL:
    return *(const double *)slot == value->decimal;
  case LEME_OPTION_CHOICE:
    memcpy(&choice, slot, sizeof(choice));
    return choice == value->choice;
  case LEME_OPTION_COLOR: {
    const float *color = slot;

    return color[0] == value->color[0] && color[1] == value->color[1] &&
           color[2] == value->color[2] && color[3] == value->color[3];
  }
  case LEME_OPTION_STRING: {
    const char *current = *(char *const *)slot;

    return (current == NULL && value->string == NULL) ||
           (current != NULL && value->string != NULL &&
            strcmp(current, value->string) == 0);
  }
  case LEME_OPTION_CUSTOM:
    break;
  }
  assert(false);
  return false;
}

void leme_option_store(const struct leme_option *option, void *target,
                       union leme_option_value *value) {
  void *slot = (char *)target + option->offset;

  switch (option->kind) {
  case LEME_OPTION_BOOL:
    *(bool *)slot = value->boolean;
    return;
  case LEME_OPTION_INT:
    *(int *)slot = (int)value->integer;
    return;
  case LEME_OPTION_U16:
    *(uint16_t *)slot = (uint16_t)value->integer;
    return;
  case LEME_OPTION_U32:
    *(uint32_t *)slot = (uint32_t)value->integer;
    return;
  case LEME_OPTION_DECIMAL:
    *(double *)slot = value->decimal;
    return;
  case LEME_OPTION_CHOICE:
    memcpy(slot, &value->choice, sizeof(value->choice));
    return;
  case LEME_OPTION_COLOR:
    memcpy(slot, value->color, sizeof(value->color));
    return;
  case LEME_OPTION_STRING: {
    char **string = slot;

    free(*string);
    *string = value->string;
    value->string = NULL;
    return;
  }
  case LEME_OPTION_CUSTOM:
    break;
  }
  assert(false);
}
