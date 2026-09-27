#include "config/public-internal.h"
#include "public/schema.h"
#include "public/value-internal.h"
#include "workspace/public.h"

static size_t field_count(uint32_t fields) {
  size_t count = 0;
  while (fields != 0) {
    fields &= fields - UINT32_C(1);
    ++count;
  }
  return count;
}

static enum leme_public_status put_name(struct leme_public_builder *b,
                                        struct leme_public_value *record,
                                        struct leme_public_text key,
                                        const char *name) {
  if (name == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  return leme_public_put_cstr(b, record, key, name);
}

static enum leme_public_status strings_value(struct leme_public_builder *b,
                                             char *const *strings, size_t count,
                                             struct leme_public_value **out) {
  if (count != 0 && strings == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  struct leme_public_value *array = NULL;
  if (leme_public_array(b, count, &array) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  for (size_t i = 0; i < count; ++i) {
    if (strings[i] == NULL)
      return leme_public_fail(b, LEME_PUBLIC_INVALID);
    struct leme_public_value *text = NULL;
    if (leme_config_public_text(b, strings[i], &text) != LEME_PUBLIC_OK ||
        leme_public_array_set(b, array, i, text) != LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
  }
  *out = array;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status tag_assignments(struct leme_public_builder *b,
                                               const struct leme_tag_rule *rule,
                                               struct leme_public_value **out) {
  const uint32_t known = LEME_TAG_FIELD_LAYOUT | LEME_TAG_FIELD_DROP_MODE |
                         LEME_TAG_FIELD_MFACT | LEME_TAG_FIELD_NMASTER |
                         LEME_TAG_FIELD_GAP | LEME_TAG_FIELD_SPLIT_RATIO |
                         LEME_TAG_FIELD_COLLAPSE_WIDTH;
  if ((rule->fields & ~known) != 0)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  const struct leme_tag_settings *settings = &rule->settings;
  struct leme_public_value *record = NULL;
  if (leme_public_object(b, field_count(rule->fields), &record) !=
          LEME_PUBLIC_OK ||
      ((rule->fields & LEME_TAG_FIELD_LAYOUT) != 0 &&
       put_name(b, record, LEME_PUBLIC_TEXT("layout"),
                leme_layout_public_name(settings->layout)) != LEME_PUBLIC_OK) ||
      ((rule->fields & LEME_TAG_FIELD_DROP_MODE) != 0 &&
       put_name(b, record, LEME_PUBLIC_TEXT("drop_mode"),
                leme_config_public_drop_mode(settings->drop_mode)) !=
           LEME_PUBLIC_OK) ||
      ((rule->fields & LEME_TAG_FIELD_MFACT) != 0 &&
       leme_public_put_number(b, record, LEME_PUBLIC_TEXT("mfact"),
                              settings->mfact) != LEME_PUBLIC_OK) ||
      ((rule->fields & LEME_TAG_FIELD_NMASTER) != 0 &&
       leme_public_put_int(b, record, LEME_PUBLIC_TEXT("nmaster"),
                           settings->nmaster) != LEME_PUBLIC_OK) ||
      ((rule->fields & LEME_TAG_FIELD_GAP) != 0 &&
       leme_public_put_int(b, record, LEME_PUBLIC_TEXT("gap"), settings->gap) !=
           LEME_PUBLIC_OK) ||
      ((rule->fields & LEME_TAG_FIELD_SPLIT_RATIO) != 0 &&
       leme_public_put_number(b, record, LEME_PUBLIC_TEXT("split_ratio"),
                              settings->split_ratio) != LEME_PUBLIC_OK) ||
      ((rule->fields & LEME_TAG_FIELD_COLLAPSE_WIDTH) != 0 &&
       leme_public_put_int(b, record, LEME_PUBLIC_TEXT("collapse_width"),
                           settings->collapse_width) != LEME_PUBLIC_OK))
    return leme_public_builder_status(b);
  *out = record;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status tags_value(struct leme_public_builder *b,
                                          const struct leme_config *config,
                                          struct leme_public_value **out) {
  if (config->tag_rule_count != 0 && config->tag_rules == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  struct leme_public_value *tags = NULL, *array = NULL;
  if (leme_public_object(b, 1, &tags) != LEME_PUBLIC_OK ||
      leme_public_array(b, config->tag_rule_count, &array) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  for (size_t i = 0; i < config->tag_rule_count; ++i) {
    const struct leme_tag_rule *rule = &config->tag_rules[i];
    if (rule->id_count == 0 || rule->ids == NULL)
      return leme_public_fail(b, LEME_PUBLIC_INVALID);
    struct leme_public_value *record = NULL, *ids = NULL, *settings = NULL;
    if (leme_public_object(b, 2, &record) != LEME_PUBLIC_OK ||
        leme_public_array(b, rule->id_count, &ids) != LEME_PUBLIC_OK ||
        tag_assignments(b, rule, &settings) != LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
    for (size_t n = 0; n < rule->id_count; ++n) {
      struct leme_public_value *id = NULL;
      if (leme_public_integer(b, rule->ids[n], &id) != LEME_PUBLIC_OK ||
          leme_public_array_set(b, ids, n, id) != LEME_PUBLIC_OK)
        return leme_public_builder_status(b);
    }
    if (leme_public_object_set(b, record, LEME_PUBLIC_TEXT("ids"), ids) !=
            LEME_PUBLIC_OK ||
        leme_public_object_set(b, record, LEME_PUBLIC_TEXT("settings"),
                               settings) != LEME_PUBLIC_OK ||
        leme_public_array_set(b, array, i, record) != LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
  }
  if (leme_public_object_set(b, tags, LEME_PUBLIC_TEXT("rules"), array) !=
      LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  *out = tags;
  return LEME_PUBLIC_OK;
}

static const char *transform_name(enum leme_output_transform transform) {
  switch (transform) {
  case LEME_OUTPUT_TRANSFORM_NORMAL:
    return "normal";
  case LEME_OUTPUT_TRANSFORM_90:
    return "90";
  case LEME_OUTPUT_TRANSFORM_180:
    return "180";
  case LEME_OUTPUT_TRANSFORM_270:
    return "270";
  }
  return NULL;
}

static const char *relation_name(enum leme_output_relation relation) {
  switch (relation) {
  case LEME_OUTPUT_RELATION_NONE:
    return NULL;
  case LEME_OUTPUT_RELATION_LEFT_OF:
    return "left_of";
  case LEME_OUTPUT_RELATION_RIGHT_OF:
    return "right_of";
  case LEME_OUTPUT_RELATION_TOP_OF:
    return "top_of";
  case LEME_OUTPUT_RELATION_BOTTOM_OF:
    return "bottom_of";
  }
  return NULL;
}

static enum leme_public_status
output_value(struct leme_public_builder *b,
             const struct leme_output_config *output,
             struct leme_public_value **out) {
  if (output->has_refresh && !output->has_mode)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  const bool relative = output->relation != LEME_OUTPUT_RELATION_NONE;
  const size_t count = 1u + (size_t)output->has_mode +
                       (size_t)output->has_position +
                       (output->configured ? 2u : 0u) + (relative ? 2u : 0u);
  struct leme_public_value *record = NULL;
  if (leme_public_object(b, count, &record) != LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("name"), output->name) !=
          LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  if (output->has_mode) {
    struct leme_public_value *mode = NULL;
    if (leme_public_object(b, 3, &mode) != LEME_PUBLIC_OK ||
        leme_public_put_int(b, mode, LEME_PUBLIC_TEXT("width"),
                            output->width) != LEME_PUBLIC_OK ||
        leme_public_put_int(b, mode, LEME_PUBLIC_TEXT("height"),
                            output->height) != LEME_PUBLIC_OK ||
        (output->has_refresh
             ? leme_public_put_int(b, mode, LEME_PUBLIC_TEXT("refresh_mhz"),
                                   output->refresh_mhz)
             : leme_public_put_null(b, mode,
                                    LEME_PUBLIC_TEXT("refresh_mhz"))) !=
            LEME_PUBLIC_OK ||
        leme_public_object_set(b, record, LEME_PUBLIC_TEXT("mode"), mode) !=
            LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
  }
  if (output->has_position) {
    struct leme_public_value *position = NULL;
    if (leme_public_object(b, 2, &position) != LEME_PUBLIC_OK ||
        leme_public_put_int(b, position, LEME_PUBLIC_TEXT("x"), output->x) !=
            LEME_PUBLIC_OK ||
        leme_public_put_int(b, position, LEME_PUBLIC_TEXT("y"), output->y) !=
            LEME_PUBLIC_OK ||
        leme_public_object_set(b, record, LEME_PUBLIC_TEXT("position"),
                               position) != LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
  }
  if (relative && (put_name(b, record, LEME_PUBLIC_TEXT("relative_to"),
                            output->relative_to) != LEME_PUBLIC_OK ||
                   put_name(b, record, LEME_PUBLIC_TEXT("relation"),
                            relation_name(output->relation)) != LEME_PUBLIC_OK))
    return leme_public_builder_status(b);
  if (output->configured &&
      (leme_public_put_number(b, record, LEME_PUBLIC_TEXT("scale"),
                              (double)output->scale) != LEME_PUBLIC_OK ||
       put_name(b, record, LEME_PUBLIC_TEXT("transform"),
                transform_name(output->transform)) != LEME_PUBLIC_OK))
    return leme_public_builder_status(b);
  *out = record;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status outputs_value(struct leme_public_builder *b,
                                             const struct leme_config *config,
                                             struct leme_public_value **out) {
  if (config->output_count != 0 && config->outputs == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  struct leme_public_value *array = NULL;
  if (leme_public_array(b, config->output_count, &array) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  for (size_t i = 0; i < config->output_count; ++i) {
    struct leme_public_value *record = NULL;
    if (output_value(b, &config->outputs[i], &record) != LEME_PUBLIC_OK ||
        leme_public_array_set(b, array, i, record) != LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
  }
  *out = array;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status
pointer_assignments(struct leme_public_builder *b,
                    const struct leme_pointer_settings *settings,
                    struct leme_public_value **out) {
  const uint32_t known = LEME_POINTER_PROFILE | LEME_POINTER_SPEED |
                         LEME_POINTER_NATURAL_SCROLL |
                         LEME_POINTER_LEFT_HANDED | LEME_POINTER_TAP;
  if ((settings->fields & ~known) != 0)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  struct leme_public_value *record = NULL;
  if (leme_public_object(b, field_count(settings->fields), &record) !=
          LEME_PUBLIC_OK ||
      ((settings->fields & LEME_POINTER_PROFILE) != 0 &&
       put_name(b, record, LEME_PUBLIC_TEXT("accel_profile"),
                leme_config_public_profile(settings->profile)) !=
           LEME_PUBLIC_OK) ||
      ((settings->fields & LEME_POINTER_SPEED) != 0 &&
       leme_public_put_number(b, record, LEME_PUBLIC_TEXT("accel_speed"),
                              settings->speed) != LEME_PUBLIC_OK) ||
      ((settings->fields & LEME_POINTER_NATURAL_SCROLL) != 0 &&
       leme_public_put_bool(b, record, LEME_PUBLIC_TEXT("natural_scroll"),
                            settings->natural_scroll) != LEME_PUBLIC_OK) ||
      ((settings->fields & LEME_POINTER_LEFT_HANDED) != 0 &&
       leme_public_put_bool(b, record, LEME_PUBLIC_TEXT("left_handed"),
                            settings->left_handed) != LEME_PUBLIC_OK) ||
      ((settings->fields & LEME_POINTER_TAP) != 0 &&
       leme_public_put_bool(b, record, LEME_PUBLIC_TEXT("tap"),
                            settings->tap) != LEME_PUBLIC_OK))
    return leme_public_builder_status(b);
  *out = record;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status pointers_value(struct leme_public_builder *b,
                                              const struct leme_config *config,
                                              struct leme_public_value **out) {
  if (config->pointer_rule_count != 0 && config->pointer_rules == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  struct leme_public_value *pointer = NULL, *array = NULL;
  if (leme_public_object(b, 1, &pointer) != LEME_PUBLIC_OK ||
      leme_public_array(b, config->pointer_rule_count, &array) !=
          LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  for (size_t i = 0; i < config->pointer_rule_count; ++i) {
    struct leme_public_value *record = NULL, *settings = NULL;
    if (leme_public_object(b, 2, &record) != LEME_PUBLIC_OK ||
        pointer_assignments(b, &config->pointer_rules[i].settings, &settings) !=
            LEME_PUBLIC_OK ||
        leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("name"),
                             config->pointer_rules[i].name) != LEME_PUBLIC_OK ||
        leme_public_object_set(b, record, LEME_PUBLIC_TEXT("settings"),
                               settings) != LEME_PUBLIC_OK ||
        leme_public_array_set(b, array, i, record) != LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
  }
  if (leme_public_object_set(b, pointer, LEME_PUBLIC_TEXT("rules"), array) !=
      LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  *out = pointer;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status keyboard_value(struct leme_public_builder *b,
                                              const struct leme_config *config,
                                              struct leme_public_value **out) {
  if (config->keyboard_layout_count != 0 && config->keyboard_layouts == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  struct leme_public_value *keyboard = NULL, *array = NULL, *options = NULL;
  if (leme_public_object(b, 4, &keyboard) != LEME_PUBLIC_OK ||
      leme_public_array(b, config->keyboard_layout_count, &array) !=
          LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  for (size_t i = 0; i < config->keyboard_layout_count; ++i) {
    struct leme_public_value *record = NULL;
    if (leme_public_object(b, 2, &record) != LEME_PUBLIC_OK ||
        leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("name"),
                             config->keyboard_layouts[i].name) !=
            LEME_PUBLIC_OK ||
        leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("variant"),
                             config->keyboard_layouts[i].variant) !=
            LEME_PUBLIC_OK ||
        leme_public_array_set(b, array, i, record) != LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
  }
  if (leme_public_object_set(b, keyboard, LEME_PUBLIC_TEXT("layouts"), array) !=
          LEME_PUBLIC_OK ||
      strings_value(b, config->keyboard_options, config->keyboard_option_count,
                    &options) != LEME_PUBLIC_OK ||
      leme_public_object_set(b, keyboard, LEME_PUBLIC_TEXT("options"),
                             options) != LEME_PUBLIC_OK ||
      leme_public_put_int(b, keyboard, LEME_PUBLIC_TEXT("repeat_rate"),
                          config->keyboard_repeat_rate) != LEME_PUBLIC_OK ||
      leme_public_put_int(b, keyboard, LEME_PUBLIC_TEXT("repeat_delay"),
                          config->keyboard_repeat_delay) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  *out = keyboard;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status
window_assignments(struct leme_public_builder *b,
                   const struct leme_window_rule *rule,
                   struct leme_public_value **out) {
  const uint32_t known = LEME_WINDOW_RULE_TAG | LEME_WINDOW_RULE_FLOATING |
                         LEME_WINDOW_RULE_FULLSCREEN | LEME_WINDOW_RULE_OUTPUT |
                         LEME_WINDOW_RULE_OPACITY;
  if ((rule->fields & ~known) != 0)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  struct leme_public_value *record = NULL;
  if (leme_public_object(b, field_count(rule->fields), &record) !=
          LEME_PUBLIC_OK ||
      ((rule->fields & LEME_WINDOW_RULE_TAG) != 0 &&
       leme_public_put_int(b, record, LEME_PUBLIC_TEXT("tag"), rule->tag_id) !=
           LEME_PUBLIC_OK) ||
      ((rule->fields & LEME_WINDOW_RULE_FLOATING) != 0 &&
       leme_public_put_bool(b, record, LEME_PUBLIC_TEXT("floating"),
                            rule->floating) != LEME_PUBLIC_OK) ||
      ((rule->fields & LEME_WINDOW_RULE_FULLSCREEN) != 0 &&
       leme_public_put_bool(b, record, LEME_PUBLIC_TEXT("fullscreen"),
                            rule->fullscreen) != LEME_PUBLIC_OK) ||
      ((rule->fields & LEME_WINDOW_RULE_OUTPUT) != 0 &&
       leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("output"),
                            rule->output) != LEME_PUBLIC_OK) ||
      ((rule->fields & LEME_WINDOW_RULE_OPACITY) != 0 &&
       leme_public_put_number(b, record, LEME_PUBLIC_TEXT("opacity"),
                              rule->opacity) != LEME_PUBLIC_OK))
    return leme_public_builder_status(b);
  *out = record;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status windows_value(struct leme_public_builder *b,
                                             const struct leme_config *config,
                                             struct leme_public_value **out) {
  if (config->window_rule_count != 0 && config->window_rules == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  struct leme_public_value *array = NULL;
  if (leme_public_array(b, config->window_rule_count, &array) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  for (size_t i = 0; i < config->window_rule_count; ++i) {
    const struct leme_window_rule *rule = &config->window_rules[i];
    if (rule->identity_count == 0)
      return leme_public_fail(b, LEME_PUBLIC_INVALID);
    struct leme_public_value *record = NULL, *identities = NULL,
                             *settings = NULL;
    if (leme_public_object(b, 3, &record) != LEME_PUBLIC_OK ||
        strings_value(b, rule->identities, rule->identity_count, &identities) !=
            LEME_PUBLIC_OK ||
        window_assignments(b, rule, &settings) != LEME_PUBLIC_OK ||
        leme_public_object_set(b, record, LEME_PUBLIC_TEXT("identities"),
                               identities) != LEME_PUBLIC_OK ||
        leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("title"),
                             rule->title) != LEME_PUBLIC_OK ||
        leme_public_object_set(b, record, LEME_PUBLIC_TEXT("settings"),
                               settings) != LEME_PUBLIC_OK ||
        leme_public_array_set(b, array, i, record) != LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
  }
  *out = array;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status
scratchpads_value(struct leme_public_builder *b,
                  const struct leme_config *config,
                  struct leme_public_value **out) {
  if (config->scratchpad_count != 0 && config->scratchpads == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  struct leme_public_value *array = NULL;
  if (leme_public_array(b, config->scratchpad_count, &array) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  for (size_t i = 0; i < config->scratchpad_count; ++i) {
    const struct leme_scratchpad_config *scratchpad = &config->scratchpads[i];
    struct leme_public_value *record = NULL, *redacted = NULL;
    if (leme_public_object(b, 5, &record) != LEME_PUBLIC_OK ||
        leme_config_public_redacted(b, &redacted) != LEME_PUBLIC_OK ||
        leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("name"),
                             scratchpad->name) != LEME_PUBLIC_OK ||
        leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("identity"),
                             scratchpad->identity) != LEME_PUBLIC_OK ||
        leme_public_put_number(b, record, LEME_PUBLIC_TEXT("width"),
                               scratchpad->width) != LEME_PUBLIC_OK ||
        leme_public_put_number(b, record, LEME_PUBLIC_TEXT("height"),
                               scratchpad->height) != LEME_PUBLIC_OK ||
        leme_public_object_set(b, record, LEME_PUBLIC_TEXT("spawn"),
                               redacted) != LEME_PUBLIC_OK ||
        leme_public_array_set(b, array, i, record) != LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
  }
  *out = array;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status
environment_value(struct leme_public_builder *b,
                  const struct leme_config *config,
                  struct leme_public_value **out) {
  if (config->environment_count != 0 && config->environment == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  struct leme_public_value *record = NULL, *names = NULL, *redacted = NULL;
  if (leme_public_object(b, 3, &record) != LEME_PUBLIC_OK ||
      leme_public_array(b, config->environment_count, &names) !=
          LEME_PUBLIC_OK ||
      leme_config_public_redacted(b, &redacted) != LEME_PUBLIC_OK ||
      leme_config_public_count(b, record, LEME_PUBLIC_TEXT("count"),
                               config->environment_count) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  for (size_t i = 0; i < config->environment_count; ++i) {
    struct leme_public_value *name = NULL;
    if (config->environment[i].name == NULL)
      return leme_public_fail(b, LEME_PUBLIC_INVALID);
    if (leme_config_public_text(b, config->environment[i].name, &name) !=
            LEME_PUBLIC_OK ||
        leme_public_array_set(b, names, i, name) != LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
  }
  if (leme_public_object_set(b, record, LEME_PUBLIC_TEXT("names"), names) !=
          LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("values"), redacted) !=
          LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  *out = record;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status startup_value(struct leme_public_builder *b,
                                             const struct leme_config *config,
                                             struct leme_public_value **out) {
  if (config->startup_count != 0 && config->startup == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  struct leme_public_value *record = NULL, *redacted = NULL;
  if (leme_public_object(b, 2, &record) != LEME_PUBLIC_OK ||
      leme_config_public_redacted(b, &redacted) != LEME_PUBLIC_OK ||
      leme_config_public_count(b, record, LEME_PUBLIC_TEXT("count"),
                               config->startup_count) != LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("argv"), redacted) !=
          LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  *out = record;
  return LEME_PUBLIC_OK;
}

enum leme_public_status
leme_config_public_rules(struct leme_public_builder *b,
                         const struct leme_config *config,
                         struct leme_public_value **out) {
  if (out == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  *out = NULL;
  if (config == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  const struct {
    struct leme_public_text name;
    enum leme_public_status (*capture)(struct leme_public_builder *,
                                       const struct leme_config *,
                                       struct leme_public_value **);
  } components[] = {{LEME_PUBLIC_TEXT("tags"), tags_value},
                    {LEME_PUBLIC_TEXT("outputs"), outputs_value},
                    {LEME_PUBLIC_TEXT("pointer"), pointers_value},
                    {LEME_PUBLIC_TEXT("keyboard"), keyboard_value},
                    {LEME_PUBLIC_TEXT("window_rules"), windows_value},
                    {LEME_PUBLIC_TEXT("scratchpads"), scratchpads_value},
                    {LEME_PUBLIC_TEXT("modes"), leme_config_public_modes},
                    {LEME_PUBLIC_TEXT("environment"), environment_value},
                    {LEME_PUBLIC_TEXT("startup"), startup_value}};
  struct leme_public_value *record = NULL;
  if (leme_public_object(b, sizeof(components) / sizeof(components[0]),
                         &record) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  for (size_t i = 0; i < sizeof(components) / sizeof(components[0]); ++i) {
    struct leme_public_value *value = NULL;
    if (components[i].capture(b, config, &value) != LEME_PUBLIC_OK ||
        leme_public_object_set(b, record, components[i].name, value) !=
            LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
  }
  const enum leme_public_status status = leme_public_settings_validate(record);
  if (status != LEME_PUBLIC_OK)
    return leme_public_fail(b, status);
  *out = record;
  return LEME_PUBLIC_OK;
}
