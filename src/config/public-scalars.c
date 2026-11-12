#include "config/public-internal.h"
#include "config/config.h"
#include "config/internal.h"
#include "config/style.h"
#include "public/value-internal.h"
#include "public/schema.h"
#include "workspace/public.h"

#include <math.h>
#include <wlr/types/wlr_keyboard.h>

const char *leme_config_public_drop_mode(enum leme_drop_mode mode) {
  switch (mode) {
  case LEME_DROP_MODE_SIMPLE:
    return "simple";
  case LEME_DROP_MODE_EDGES:
    return "edges";
  }
  return NULL;
}

const char *
leme_config_public_profile(enum leme_pointer_accel_profile profile) {
  switch (profile) {
  case LEME_POINTER_ACCEL_ADAPTIVE:
    return "adaptive";
  case LEME_POINTER_ACCEL_FLAT:
    return "flat";
  }
  return NULL;
}

static enum leme_public_status
put_pointer_unset(struct leme_public_builder *b,
                  struct leme_public_value *record, struct leme_public_text key,
                  bool nulls) {
  return nulls ? leme_public_put_null(b, record, key) : LEME_PUBLIC_OK;
}

static enum leme_public_status
put_pointer_bool(struct leme_public_builder *b,
                 struct leme_public_value *record, struct leme_public_text key,
                 bool set, bool nulls, bool value) {
  return set ? leme_public_put_bool(b, record, key, value)
             : put_pointer_unset(b, record, key, nulls);
}

static enum leme_public_status
put_pointer_choice(struct leme_public_builder *b,
                   struct leme_public_value *record,
                   struct leme_public_text key, bool set, bool nulls,
                   enum leme_pointer_choice choice, unsigned value) {
  if (!set)
    return put_pointer_unset(b, record, key, nulls);
  const char *name = leme_pointer_choice_name(choice, value);
  if (name == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  return leme_public_put_cstr(b, record, key, name);
}

static enum leme_public_status put_pointer_number(
    struct leme_public_builder *b, struct leme_public_value *record,
    struct leme_public_text key, bool set, bool nulls, double value) {
  return set ? leme_public_put_number(b, record, key, value)
             : put_pointer_unset(b, record, key, nulls);
}

enum leme_public_status leme_config_public_pointer_options(
    struct leme_public_builder *b, struct leme_public_value *record,
    const struct leme_pointer_settings *settings, bool nulls) {
  const uint32_t fields = settings->fields;
  if (put_pointer_bool(b, record, LEME_PUBLIC_TEXT("disable_while_typing"),
                       (fields & LEME_POINTER_DISABLE_WHILE_TYPING) != 0, nulls,
                       settings->disable_while_typing) != LEME_PUBLIC_OK ||
      put_pointer_choice(b, record, LEME_PUBLIC_TEXT("click_method"),
                         (fields & LEME_POINTER_CLICK_METHOD) != 0, nulls,
                         LEME_POINTER_CHOICE_CLICK_METHOD,
                         settings->click_method) != LEME_PUBLIC_OK ||
      put_pointer_choice(b, record, LEME_PUBLIC_TEXT("scroll_method"),
                         (fields & LEME_POINTER_SCROLL_METHOD) != 0, nulls,
                         LEME_POINTER_CHOICE_SCROLL_METHOD,
                         settings->scroll_method) != LEME_PUBLIC_OK ||
      put_pointer_choice(b, record, LEME_PUBLIC_TEXT("scroll_button"),
                         (fields & LEME_POINTER_SCROLL_BUTTON) != 0, nulls,
                         LEME_POINTER_CHOICE_SCROLL_BUTTON,
                         settings->scroll_button) != LEME_PUBLIC_OK ||
      put_pointer_bool(b, record, LEME_PUBLIC_TEXT("middle_emulation"),
                       (fields & LEME_POINTER_MIDDLE_EMULATION) != 0, nulls,
                       settings->middle_emulation) != LEME_PUBLIC_OK ||
      put_pointer_choice(b, record, LEME_PUBLIC_TEXT("tap_button_map"),
                         (fields & LEME_POINTER_TAP_BUTTON_MAP) != 0, nulls,
                         LEME_POINTER_CHOICE_TAP_BUTTON_MAP,
                         settings->tap_button_map) != LEME_PUBLIC_OK ||
      put_pointer_bool(b, record, LEME_PUBLIC_TEXT("tap_drag"),
                       (fields & LEME_POINTER_TAP_DRAG) != 0, nulls,
                       settings->tap_drag) != LEME_PUBLIC_OK ||
      put_pointer_choice(b, record, LEME_PUBLIC_TEXT("drag_lock"),
                         (fields & LEME_POINTER_DRAG_LOCK) != 0, nulls,
                         LEME_POINTER_CHOICE_DRAG_LOCK,
                         settings->drag_lock) != LEME_PUBLIC_OK ||
      put_pointer_number(b, record, LEME_PUBLIC_TEXT("drag_fingers"),
                         (fields & LEME_POINTER_DRAG_FINGERS) != 0, nulls,
                         settings->drag_fingers) != LEME_PUBLIC_OK ||
      put_pointer_number(b, record, LEME_PUBLIC_TEXT("scroll_factor"),
                         (fields & LEME_POINTER_SCROLL_FACTOR) != 0, nulls,
                         settings->scroll_factor) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  return LEME_PUBLIC_OK;
}

static const char *container_modifier_name(uint32_t modifier) {
  switch (modifier) {
  case WLR_MODIFIER_SHIFT:
    return "shift";
  case WLR_MODIFIER_CTRL:
    return "ctrl";
  case WLR_MODIFIER_ALT:
    return "alt";
  case WLR_MODIFIER_LOGO:
    return "super";
  default:
    return NULL;
  }
}

static enum leme_public_status put_enum(struct leme_public_builder *b,
                                        struct leme_public_value *record,
                                        struct leme_public_text key,
                                        const char *name) {
  if (name == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  return leme_public_put_cstr(b, record, key, name);
}

static enum leme_public_status put_color(struct leme_public_builder *b,
                                         struct leme_public_value *record,
                                         struct leme_public_text key,
                                         const float rgba[4]) {
  static const char digits[] = "0123456789ABCDEF";
  char color[9] = {'#'};
  for (size_t i = 0; i < 4; ++i) {
    const double channel = (double)rgba[i];
    if (!isfinite(channel) || channel < 0 || channel > 1)
      return leme_public_fail(b, LEME_PUBLIC_INVALID);
    const unsigned byte = (unsigned)floor(channel * 255.0 + 0.5);
    color[1 + i * 2] = digits[byte / 16];
    color[2 + i * 2] = digits[byte % 16];
  }
  return leme_public_put_text(
      b, record, key, (struct leme_public_text){color, sizeof(color)}, false);
}

static enum leme_public_status
outer_gaps_value(struct leme_public_builder *b, const struct leme_gaps *gaps,
                 struct leme_public_value **out) {
  struct leme_public_value *record = NULL;
  if (leme_public_object(b, 4, &record) != LEME_PUBLIC_OK ||
      leme_public_put_int(b, record, LEME_PUBLIC_TEXT("top"), gaps->top) !=
          LEME_PUBLIC_OK ||
      leme_public_put_int(b, record, LEME_PUBLIC_TEXT("right"), gaps->right) !=
          LEME_PUBLIC_OK ||
      leme_public_put_int(b, record, LEME_PUBLIC_TEXT("bottom"),
                          gaps->bottom) != LEME_PUBLIC_OK ||
      leme_public_put_int(b, record, LEME_PUBLIC_TEXT("left"), gaps->left) !=
          LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  *out = record;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status
titlebar_value(struct leme_public_builder *b,
               const struct leme_titlebar_style *style,
               struct leme_public_value **out) {
  static const char *const button_names[] = {"minimize", "maximize", "close"};
  struct leme_public_value *titlebar = NULL, *buttons = NULL;
  if (style->button_count > LEME_ARRAY_LENGTH(style->buttons))
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  if (leme_public_array(b, style->button_count, &buttons) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  for (size_t i = 0; i < style->button_count; ++i) {
    struct leme_public_value *name = NULL;
    if ((size_t)style->buttons[i] >= LEME_ARRAY_LENGTH(button_names))
      return leme_public_fail(b, LEME_PUBLIC_INVALID);
    if (leme_config_public_text(b, button_names[style->buttons[i]], &name) !=
            LEME_PUBLIC_OK ||
        leme_public_array_set(b, buttons, i, name) != LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
  }
  if (leme_public_object(b, 7, &titlebar) != LEME_PUBLIC_OK ||
      put_color(b, titlebar, LEME_PUBLIC_TEXT("background_active"),
                style->background_active) != LEME_PUBLIC_OK ||
      put_color(b, titlebar, LEME_PUBLIC_TEXT("background_inactive"),
                style->background_inactive) != LEME_PUBLIC_OK ||
      put_color(b, titlebar, LEME_PUBLIC_TEXT("text_active"),
                style->text_active) != LEME_PUBLIC_OK ||
      put_color(b, titlebar, LEME_PUBLIC_TEXT("text_inactive"),
                style->text_inactive) != LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, titlebar, LEME_PUBLIC_TEXT("font"),
                           style->font) != LEME_PUBLIC_OK ||
      put_enum(b, titlebar, LEME_PUBLIC_TEXT("align"),
               style->align == LEME_TITLEBAR_ALIGN_CENTER
                   ? "center"
                   : "left") != LEME_PUBLIC_OK ||
      leme_public_object_set(b, titlebar, LEME_PUBLIC_TEXT("buttons"),
                             buttons) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  *out = titlebar;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status style_value(struct leme_public_builder *b,
                                           const struct leme_config *config,
                                           struct leme_public_value **out) {
  struct leme_public_value *style = NULL, *outer = NULL, *titlebar = NULL;
  struct leme_titlebar_style titlebar_style = {0};
  leme_desk_style_titlebar(config, &titlebar_style);
  if (outer_gaps_value(b, &config->gap_outer, &outer) != LEME_PUBLIC_OK ||
      titlebar_value(b, &titlebar_style, &titlebar) != LEME_PUBLIC_OK ||
      leme_public_object(b, 12, &style) != LEME_PUBLIC_OK ||
      leme_public_put_int(b, style, LEME_PUBLIC_TEXT("gap"), config->gap) !=
          LEME_PUBLIC_OK ||
      leme_public_object_set(b, style, LEME_PUBLIC_TEXT("gap_outer"), outer) !=
          LEME_PUBLIC_OK ||
      leme_public_put_bool(b, style, LEME_PUBLIC_TEXT("smart_gaps"),
                           config->smart_gaps) != LEME_PUBLIC_OK ||
      leme_public_put_int(b, style, LEME_PUBLIC_TEXT("border_width"),
                          config->border_width) != LEME_PUBLIC_OK ||
      leme_public_put_int(b, style, LEME_PUBLIC_TEXT("corner_radius"),
                          config->corner_radius) != LEME_PUBLIC_OK ||
      leme_public_put_int(b, style, LEME_PUBLIC_TEXT("blur"), config->blur) !=
          LEME_PUBLIC_OK ||
      put_color(b, style, LEME_PUBLIC_TEXT("border_active"),
                config->border_active) != LEME_PUBLIC_OK ||
      put_color(b, style, LEME_PUBLIC_TEXT("border_inactive"),
                config->border_inactive) != LEME_PUBLIC_OK ||
      leme_public_put_number(b, style, LEME_PUBLIC_TEXT("opacity_active"),
                             config->opacity_active) != LEME_PUBLIC_OK ||
      leme_public_put_number(b, style, LEME_PUBLIC_TEXT("opacity_inactive"),
                             config->opacity_inactive) != LEME_PUBLIC_OK ||
      put_enum(b, style, LEME_PUBLIC_TEXT("fullscreen_covers"),
               leme_choice_fullscreen_covers[config->fullscreen_covers]) !=
          LEME_PUBLIC_OK ||
      leme_public_object_set(b, style, LEME_PUBLIC_TEXT("titlebar"),
                             titlebar) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  *out = style;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status tags_value(struct leme_public_builder *b,
                                          const struct leme_config *config,
                                          struct leme_public_value **out) {
  const struct leme_tag_settings *settings = &config->tag_defaults;
  struct leme_public_value *tags = NULL, *defaults = NULL;
  if (config->initial_tags > config->max_tags)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  if (leme_public_object(b, 3, &tags) != LEME_PUBLIC_OK ||
      leme_public_object(b, 6, &defaults) != LEME_PUBLIC_OK ||
      leme_public_put_int(b, tags, LEME_PUBLIC_TEXT("initial"),
                          config->initial_tags) != LEME_PUBLIC_OK ||
      leme_public_put_int(b, tags, LEME_PUBLIC_TEXT("maximum"),
                          config->max_tags) != LEME_PUBLIC_OK ||
      put_enum(b, defaults, LEME_PUBLIC_TEXT("layout"),
               leme_layout_public_name(settings->layout)) != LEME_PUBLIC_OK ||
      put_enum(b, defaults, LEME_PUBLIC_TEXT("drop_mode"),
               leme_config_public_drop_mode(settings->drop_mode)) !=
          LEME_PUBLIC_OK ||
      leme_public_put_number(b, defaults, LEME_PUBLIC_TEXT("mfact"),
                             settings->mfact) != LEME_PUBLIC_OK ||
      leme_public_put_int(b, defaults, LEME_PUBLIC_TEXT("nmaster"),
                          settings->nmaster) != LEME_PUBLIC_OK ||
      leme_public_put_int(b, defaults, LEME_PUBLIC_TEXT("gap"),
                          settings->has_gap ? settings->gap : config->gap) !=
          LEME_PUBLIC_OK ||
      leme_public_put_number(b, defaults, LEME_PUBLIC_TEXT("split_ratio"),
                             settings->split_ratio) != LEME_PUBLIC_OK ||
      leme_public_object_set(b, tags, LEME_PUBLIC_TEXT("defaults"), defaults) !=
          LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  *out = tags;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status
output_policy_value(struct leme_public_builder *b,
                    const struct leme_config *config,
                    struct leme_public_value **out) {
  struct leme_public_value *policy = NULL;
  if (leme_public_object(b, 5, &policy) != LEME_PUBLIC_OK ||
      leme_public_put_bool(b, policy, LEME_PUBLIC_TEXT("cross_output_focus"),
                           config->output_policy.cross_output_focus) !=
          LEME_PUBLIC_OK ||
      leme_public_put_bool(b, policy, LEME_PUBLIC_TEXT("cross_output_move"),
                           config->output_policy.cross_output_move) !=
          LEME_PUBLIC_OK ||
      leme_public_put_bool(b, policy, LEME_PUBLIC_TEXT("cross_output_drag"),
                           config->output_policy.cross_output_drag) !=
          LEME_PUBLIC_OK ||
      leme_public_put_bool(b, policy, LEME_PUBLIC_TEXT("warp_cursor"),
                           config->output_policy.warp_cursor) !=
          LEME_PUBLIC_OK ||
      put_enum(
          b, policy, LEME_PUBLIC_TEXT("secondary_gpu"),
          leme_choice_secondary_gpu[config->output_policy.secondary_gpu]) !=
          LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  *out = policy;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status pointer_value(struct leme_public_builder *b,
                                             const struct leme_config *config,
                                             struct leme_public_value **out) {
  const struct leme_pointer_settings *settings = &config->pointer_defaults;
  struct leme_public_value *pointer = NULL;
  if (leme_public_object(b, 16, &pointer) != LEME_PUBLIC_OK ||
      put_enum(b, pointer, LEME_PUBLIC_TEXT("accel_profile"),
               leme_config_public_profile(settings->profile)) !=
          LEME_PUBLIC_OK ||
      leme_public_put_number(b, pointer, LEME_PUBLIC_TEXT("accel_speed"),
                             settings->speed) != LEME_PUBLIC_OK ||
      leme_public_put_bool(b, pointer, LEME_PUBLIC_TEXT("natural_scroll"),
                           settings->natural_scroll) != LEME_PUBLIC_OK ||
      leme_public_put_bool(b, pointer, LEME_PUBLIC_TEXT("left_handed"),
                           settings->left_handed) != LEME_PUBLIC_OK ||
      leme_public_put_bool(b, pointer, LEME_PUBLIC_TEXT("tap"),
                           settings->tap) != LEME_PUBLIC_OK ||
      leme_public_put_bool(b, pointer, LEME_PUBLIC_TEXT("focus_follows_mouse"),
                           config->focus_follows_mouse) != LEME_PUBLIC_OK ||
      leme_config_public_pointer_options(b, pointer, settings, true) !=
          LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  *out = pointer;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status gestures_value(struct leme_public_builder *b,
                                              const struct leme_config *config,
                                              struct leme_public_value **out) {
  const struct leme_workspace_switch_gesture_settings *settings =
      &config->gestures.workspace_switch;
  struct leme_public_value *gestures = NULL, *workspace = NULL, *edges = NULL;
  if (leme_public_object(b, 2, &gestures) != LEME_PUBLIC_OK ||
      leme_public_object(b, 7, &workspace) != LEME_PUBLIC_OK ||
      put_enum(b, workspace, LEME_PUBLIC_TEXT("mode"),
               leme_choice_gesture_mode[settings->mode]) != LEME_PUBLIC_OK ||
      leme_public_put_int(b, workspace, LEME_PUBLIC_TEXT("fingers"),
                          settings->fingers) != LEME_PUBLIC_OK ||
      leme_public_put_number(b, workspace, LEME_PUBLIC_TEXT("distance"),
                             settings->distance) != LEME_PUBLIC_OK ||
      leme_public_put_number(b, workspace, LEME_PUBLIC_TEXT("threshold"),
                             settings->threshold) != LEME_PUBLIC_OK ||
      leme_public_put_number(b, workspace, LEME_PUBLIC_TEXT("deceleration"),
                             settings->deceleration) != LEME_PUBLIC_OK ||
      leme_public_put_int(b, workspace, LEME_PUBLIC_TEXT("velocity_window"),
                          settings->velocity_window_ms) != LEME_PUBLIC_OK ||
      leme_public_put_bool(b, workspace, LEME_PUBLIC_TEXT("occupied"),
                           settings->occupied) != LEME_PUBLIC_OK ||
      leme_public_object_set(b, gestures, LEME_PUBLIC_TEXT("workspace_switch"),
                             workspace) != LEME_PUBLIC_OK ||
      leme_public_object(b, 2, &edges) != LEME_PUBLIC_OK ||
      leme_public_put_number(b, edges, LEME_PUBLIC_TEXT("size"),
                             config->gestures.edges.size) != LEME_PUBLIC_OK ||
      leme_public_put_number(b, edges, LEME_PUBLIC_TEXT("step"),
                             config->gestures.edges.step) != LEME_PUBLIC_OK ||
      leme_public_object_set(b, gestures, LEME_PUBLIC_TEXT("edges"), edges) !=
          LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  *out = gestures;
  return LEME_PUBLIC_OK;
}

enum leme_public_status
leme_config_public_scalars(struct leme_public_builder *b,
                           const struct leme_config *config,
                           struct leme_public_value **out) {
  if (out == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  *out = NULL;
  if (config == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  struct leme_public_value *settings = NULL, *style = NULL, *tags = NULL,
                           *policy = NULL;
  struct leme_public_value *cursor = NULL, *pointer = NULL, *gestures = NULL,
                           *publication = NULL, *errors = NULL, *desktop = NULL;
  if (leme_public_object(b, 9, &settings) != LEME_PUBLIC_OK ||
      leme_public_object(b, 2, &desktop) != LEME_PUBLIC_OK ||
      leme_public_put_bool(b, desktop, LEME_PUBLIC_TEXT("minimized_strip"),
                           config->desktop_minimized_strip) != LEME_PUBLIC_OK ||
      put_enum(b, desktop, LEME_PUBLIC_TEXT("container_modifier"),
               container_modifier_name(config->desktop_container_modifier)) !=
          LEME_PUBLIC_OK ||
      leme_public_object_set(b, settings, LEME_PUBLIC_TEXT("desktop"),
                             desktop) != LEME_PUBLIC_OK ||
      style_value(b, config, &style) != LEME_PUBLIC_OK ||
      tags_value(b, config, &tags) != LEME_PUBLIC_OK ||
      output_policy_value(b, config, &policy) != LEME_PUBLIC_OK ||
      pointer_value(b, config, &pointer) != LEME_PUBLIC_OK ||
      gestures_value(b, config, &gestures) != LEME_PUBLIC_OK ||
      leme_public_object(b, 2, &cursor) != LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, cursor, LEME_PUBLIC_TEXT("theme"),
                           config->cursor.theme) != LEME_PUBLIC_OK ||
      leme_public_put_int(b, cursor, LEME_PUBLIC_TEXT("size"),
                          config->cursor.size) != LEME_PUBLIC_OK ||
      leme_public_object(b, 1, &publication) != LEME_PUBLIC_OK ||
      put_enum(b, publication, LEME_PUBLIC_TEXT("activation"),
               leme_choice_activation[config->publication.activation]) !=
          LEME_PUBLIC_OK ||
      leme_public_object(b, 3, &errors) != LEME_PUBLIC_OK ||
      leme_public_put_bool(b, errors, LEME_PUBLIC_TEXT("show"),
                           config->config_errors.show) != LEME_PUBLIC_OK ||
      put_enum(b, errors, LEME_PUBLIC_TEXT("position"),
               leme_choice_banner[config->config_errors.position]) !=
          LEME_PUBLIC_OK ||
      leme_public_put_int(b, errors, LEME_PUBLIC_TEXT("timeout"),
                          config->config_errors.timeout) != LEME_PUBLIC_OK ||
      leme_public_object_set(b, settings, LEME_PUBLIC_TEXT("style"), style) !=
          LEME_PUBLIC_OK ||
      leme_public_object_set(b, settings, LEME_PUBLIC_TEXT("tags"), tags) !=
          LEME_PUBLIC_OK ||
      leme_public_object_set(b, settings, LEME_PUBLIC_TEXT("output_policy"),
                             policy) != LEME_PUBLIC_OK ||
      leme_public_object_set(b, settings, LEME_PUBLIC_TEXT("cursor"), cursor) !=
          LEME_PUBLIC_OK ||
      leme_public_object_set(b, settings, LEME_PUBLIC_TEXT("pointer"),
                             pointer) != LEME_PUBLIC_OK ||
      leme_public_object_set(b, settings, LEME_PUBLIC_TEXT("gestures"),
                             gestures) != LEME_PUBLIC_OK ||
      leme_public_object_set(b, settings, LEME_PUBLIC_TEXT("publication"),
                             publication) != LEME_PUBLIC_OK ||
      leme_public_object_set(b, settings, LEME_PUBLIC_TEXT("config_errors"),
                             errors) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  *out = settings;
  return LEME_PUBLIC_OK;
}
