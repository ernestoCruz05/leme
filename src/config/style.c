#include "config/style.h"

#include "config/style-internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define S(state) LEME_DESK_STATE_BIT(LEME_DESK_STATE_##state)
#define P(property) (UINT32_C(1) << LEME_DESK_PROPERTY_##property)

#define LEME_STYLE_STATE(id, name)
#define LEME_STYLE_PROPERTY(id, name, kind, low, high)
#define LEME_STYLE_ELEMENT(id, name, extends, inside, states, properties)      \
  [LEME_DESK_ELEMENT_##id] = {name, LEME_DESK_ELEMENT_##extends,               \
                              LEME_DESK_ELEMENT_##inside, states, properties},
const struct leme_desk_style_element_info
    leme_desk_style_elements[LEME_DESK_ELEMENT_COUNT] = {
#include "config/style.def"
};
#undef LEME_STYLE_STATE
#undef LEME_STYLE_PROPERTY
#undef LEME_STYLE_ELEMENT

#define LEME_STYLE_STATE(id, name)
#define LEME_STYLE_PROPERTY(id, name, kind, low, high)                         \
  [LEME_DESK_PROPERTY_##id] = {name, LEME_DESK_STYLE_##kind, low, high},
#define LEME_STYLE_ELEMENT(id, name, extends, inside, states, properties)
const struct leme_desk_style_property_info
    leme_desk_style_properties[LEME_DESK_PROPERTY_COUNT] = {
#include "config/style.def"
};
#undef LEME_STYLE_STATE
#undef LEME_STYLE_PROPERTY
#undef LEME_STYLE_ELEMENT

#define LEME_STYLE_STATE(id, name) [LEME_DESK_STATE_##id - 1] = name,
#define LEME_STYLE_PROPERTY(id, name, kind, low, high)
#define LEME_STYLE_ELEMENT(id, name, extends, inside, states, properties)
const char *const leme_desk_style_state_names[LEME_DESK_STATE_COUNT - 1] = {
#include "config/style.def"
};
#undef LEME_STYLE_STATE
#undef LEME_STYLE_PROPERTY
#undef LEME_STYLE_ELEMENT

#undef S
#undef P

const char *const leme_desk_style_button_names[3] = {
    [LEME_TITLEBAR_BUTTON_MINIMIZE] = "minimize",
    [LEME_TITLEBAR_BUTTON_MAXIMIZE] = "maximize",
    [LEME_TITLEBAR_BUTTON_CLOSE] = "close",
};

uint32_t leme_desk_style_bit(enum leme_desk_property property) {
  return UINT32_C(1) << property;
}

static void leme_desk_style_rgba(float color[4], uint32_t rgba) {
  const float alpha = (float)(rgba & 0xffu) / 255.0f;
  color[0] = (float)((rgba >> 24) & 0xffu) / 255.0f * alpha;
  color[1] = (float)((rgba >> 16) & 0xffu) / 255.0f * alpha;
  color[2] = (float)((rgba >> 8) & 0xffu) / 255.0f * alpha;
  color[3] = alpha;
}

float *leme_desk_style_color_slot(struct leme_desk_look *look,
                                  enum leme_desk_property property) {
  switch (property) {
  case LEME_DESK_PROPERTY_BACKGROUND:
    return look->background;
  case LEME_DESK_PROPERTY_COLOR:
    return look->color;
  case LEME_DESK_PROPERTY_BORDER_COLOR:
    return look->border_color;
  default:
    return NULL;
  }
}

int *leme_desk_style_pixels_slot(struct leme_desk_look *look,
                                 enum leme_desk_property property) {
  switch (property) {
  case LEME_DESK_PROPERTY_RADIUS:
    return &look->radius;
  case LEME_DESK_PROPERTY_WIDTH:
    return &look->width;
  case LEME_DESK_PROPERTY_HEIGHT:
    return &look->height;
  case LEME_DESK_PROPERTY_PADDING:
    return &look->padding;
  case LEME_DESK_PROPERTY_SIZE:
    return &look->size;
  case LEME_DESK_PROPERTY_SPACING:
    return &look->spacing;
  case LEME_DESK_PROPERTY_GAP:
    return &look->gap;
  default:
    return NULL;
  }
}

static void leme_desk_style_color(struct leme_desk_look *look,
                                  enum leme_desk_property property,
                                  const float color[4]) {
  memcpy(leme_desk_style_color_slot(look, property), color, sizeof(float[4]));
  look->set |= leme_desk_style_bit(property);
}

static void leme_desk_style_hex(struct leme_desk_look *look,
                                enum leme_desk_property property,
                                uint32_t rgba) {
  float color[4] = {0};
  leme_desk_style_rgba(color, rgba);
  leme_desk_style_color(look, property, color);
}

static void leme_desk_style_pixels(struct leme_desk_look *look,
                                   enum leme_desk_property property,
                                   int value) {
  *leme_desk_style_pixels_slot(look, property) = value;
  look->set |= leme_desk_style_bit(property);
}

static void leme_desk_style_copy(struct leme_desk_look *look,
                                 const struct leme_desk_look *source,
                                 enum leme_desk_property property) {
  switch (property) {
  case LEME_DESK_PROPERTY_BACKGROUND:
    memcpy(look->background, source->background, sizeof(look->background));
    break;
  case LEME_DESK_PROPERTY_COLOR:
    memcpy(look->color, source->color, sizeof(look->color));
    break;
  case LEME_DESK_PROPERTY_BORDER_COLOR:
    memcpy(look->border_color, source->border_color,
           sizeof(look->border_color));
    break;
  case LEME_DESK_PROPERTY_FONT:
    look->font = source->font;
    break;
  case LEME_DESK_PROPERTY_RADIUS:
    look->radius = source->radius;
    break;
  case LEME_DESK_PROPERTY_WIDTH:
    look->width = source->width;
    break;
  case LEME_DESK_PROPERTY_HEIGHT:
    look->height = source->height;
    break;
  case LEME_DESK_PROPERTY_PADDING:
    look->padding = source->padding;
    break;
  case LEME_DESK_PROPERTY_SIZE:
    look->size = source->size;
    break;
  case LEME_DESK_PROPERTY_SPACING:
    look->spacing = source->spacing;
    break;
  case LEME_DESK_PROPERTY_GAP:
    look->gap = source->gap;
    break;
  case LEME_DESK_PROPERTY_OPACITY:
    look->opacity = source->opacity;
    break;
  case LEME_DESK_PROPERTY_ALIGN:
    look->align = source->align;
    break;
  case LEME_DESK_PROPERTY_LAYOUT:
    look->layout = source->layout;
    break;
  case LEME_DESK_PROPERTY_SHADOW:
    look->shadow = source->shadow;
    break;
  case LEME_DESK_PROPERTY_ICON:
    look->icon = source->icon;
    break;
  case LEME_DESK_PROPERTY_THEME:
    look->theme = source->theme;
    break;
  case LEME_DESK_PROPERTY_COUNT:
  default:
    return;
  }
  look->set |= leme_desk_style_bit(property);
}

static void leme_desk_style_overlay(struct leme_desk_look *look,
                                    const struct leme_desk_look *source) {
  for (size_t index = 0; index < LEME_DESK_PROPERTY_COUNT; index++) {
    const enum leme_desk_property property = (enum leme_desk_property)index;
    if ((source->set & leme_desk_style_bit(property)) != 0) {
      leme_desk_style_copy(look, source, property);
    }
  }
}

static const struct leme_desk_style_rule *
leme_desk_style_rule(const struct leme_config *config,
                     enum leme_desk_element element,
                     enum leme_desk_state state) {
  if (config == NULL || config->desk_style == NULL) {
    return NULL;
  }
  return &config->desk_style->rules[element][state];
}

static void leme_desk_style_scale(float color[4], const float source[4],
                                  float factor) {
  for (size_t i = 0; i < 4; i++) {
    color[i] = source[i] * factor;
  }
}

static void leme_desk_style_ring(const struct leme_config *config,
                                 float ring[4]) {
  const struct leme_desk_style_rule *rule = leme_desk_style_rule(
      config, LEME_DESK_ELEMENT_TITLEBAR, LEME_DESK_STATE_INACTIVE);
  float base[4] = {0};
  if (rule != NULL &&
      (rule->look.set & leme_desk_style_bit(LEME_DESK_PROPERTY_COLOR)) != 0) {
    memcpy(base, rule->look.color, sizeof(base));
  } else {
    leme_desk_style_rgba(base, 0xa1a9b1ffu);
  }
  leme_desk_style_scale(ring, base, 0.3f);
}

static void leme_desk_style_accent(const struct leme_config *config,
                                   float factor, float color[4]) {
  float active[4] = {0};
  if (config != NULL) {
    memcpy(active, config->border_active, sizeof(active));
  } else {
    leme_desk_style_rgba(active, 0x296bb8ffu);
  }
  leme_desk_style_scale(color, active, factor);
}

static void leme_desk_style_panel(const struct leme_config *config,
                                  struct leme_desk_look *look) {
  float ring[4] = {0};
  leme_desk_style_ring(config, ring);
  const int corner = config == NULL ? 0 : config->corner_radius;
  leme_desk_style_hex(look, LEME_DESK_PROPERTY_BACKGROUND, 0x2a2e32ffu);
  leme_desk_style_hex(look, LEME_DESK_PROPERTY_COLOR, 0xfcfcfcffu);
  leme_desk_style_color(look, LEME_DESK_PROPERTY_BORDER_COLOR, ring);
  look->font = "Sans 10";
  look->set |= leme_desk_style_bit(LEME_DESK_PROPERTY_FONT);
  leme_desk_style_pixels(look, LEME_DESK_PROPERTY_RADIUS,
                         corner < 6 ? corner : 6);
}

static void leme_desk_style_default_normal(const struct leme_config *config,
                                           enum leme_desk_element element,
                                           struct leme_desk_look *look) {
  float color[4] = {0};
  switch (element) {
  case LEME_DESK_ELEMENT_TITLEBAR:
    leme_desk_style_hex(look, LEME_DESK_PROPERTY_BACKGROUND, 0x2a2e32ffu);
    leme_desk_style_hex(look, LEME_DESK_PROPERTY_COLOR, 0xfcfcfcffu);
    leme_desk_style_pixels(look, LEME_DESK_PROPERTY_HEIGHT, 32);
    look->font = "Sans 10";
    look->align = LEME_TITLEBAR_ALIGN_CENTER;
    look->layout = (struct leme_desk_layout){
        .buttons = {LEME_TITLEBAR_BUTTON_MINIMIZE,
                    LEME_TITLEBAR_BUTTON_MAXIMIZE, LEME_TITLEBAR_BUTTON_CLOSE},
        .count = 3,
    };
    look->set |= leme_desk_style_bit(LEME_DESK_PROPERTY_FONT) |
                 leme_desk_style_bit(LEME_DESK_PROPERTY_ALIGN) |
                 leme_desk_style_bit(LEME_DESK_PROPERTY_LAYOUT);
    return;
  case LEME_DESK_ELEMENT_BUTTON:
    leme_desk_style_hex(look, LEME_DESK_PROPERTY_BACKGROUND, 0);
    leme_desk_style_pixels(look, LEME_DESK_PROPERTY_RADIUS, 0);
    leme_desk_style_pixels(look, LEME_DESK_PROPERTY_WIDTH, 32);
    leme_desk_style_pixels(look, LEME_DESK_PROPERTY_SIZE, 32);
    look->opacity = 1.0;
    look->set |= leme_desk_style_bit(LEME_DESK_PROPERTY_OPACITY);
    return;
  case LEME_DESK_ELEMENT_MENU:
  case LEME_DESK_ELEMENT_PROMPT:
    leme_desk_style_panel(config, look);
    return;
  case LEME_DESK_ELEMENT_MENU_ITEM:
    leme_desk_style_hex(look, LEME_DESK_PROPERTY_BACKGROUND, 0);
    leme_desk_style_hex(look, LEME_DESK_PROPERTY_BORDER_COLOR, 0);
    return;
  case LEME_DESK_ELEMENT_MENU_SEPARATOR:
    leme_desk_style_ring(config, color);
    leme_desk_style_color(look, LEME_DESK_PROPERTY_COLOR, color);
    return;
  case LEME_DESK_ELEMENT_PROMPT_FIELD:
  case LEME_DESK_ELEMENT_PROMPT_BUTTON:
    leme_desk_style_ring(config, color);
    leme_desk_style_hex(look, LEME_DESK_PROPERTY_BACKGROUND, 0);
    leme_desk_style_color(look, LEME_DESK_PROPERTY_BORDER_COLOR, color);
    return;
  case LEME_DESK_ELEMENT_STRIP:
    leme_desk_style_hex(look, LEME_DESK_PROPERTY_BACKGROUND, 0x000000ffu);
    leme_desk_style_hex(look, LEME_DESK_PROPERTY_COLOR, 0xccccccffu);
    return;
  case LEME_DESK_ELEMENT_ICON:
    leme_desk_style_hex(look, LEME_DESK_PROPERTY_BACKGROUND, 0);
    leme_desk_style_hex(look, LEME_DESK_PROPERTY_BORDER_COLOR, 0);
    leme_desk_style_hex(look, LEME_DESK_PROPERTY_COLOR, 0xffffffffu);
    leme_desk_style_pixels(look, LEME_DESK_PROPERTY_SIZE, 48);
    leme_desk_style_pixels(look, LEME_DESK_PROPERTY_SPACING, 96);
    leme_desk_style_pixels(look, LEME_DESK_PROPERTY_PADDING, 16);
    leme_desk_style_pixels(look, LEME_DESK_PROPERTY_WIDTH, 88);
    leme_desk_style_pixels(look, LEME_DESK_PROPERTY_GAP, 8);
    look->font = "Sans 10";
    look->theme = LEME_DESK_STYLE_ICON_THEME;
    look->shadow = true;
    look->set |= leme_desk_style_bit(LEME_DESK_PROPERTY_FONT) |
                 leme_desk_style_bit(LEME_DESK_PROPERTY_THEME) |
                 leme_desk_style_bit(LEME_DESK_PROPERTY_SHADOW);
    return;
  case LEME_DESK_ELEMENT_SELECTION:
    leme_desk_style_accent(config, 0.45f, color);
    leme_desk_style_color(look, LEME_DESK_PROPERTY_BACKGROUND, color);
    leme_desk_style_accent(config, 0.8f, color);
    leme_desk_style_color(look, LEME_DESK_PROPERTY_BORDER_COLOR, color);
    return;
  default:
    return;
  }
}

static void leme_desk_style_default(const struct leme_config *config,
                                    enum leme_desk_element element,
                                    enum leme_desk_state state,
                                    struct leme_desk_look *look) {
  float color[4] = {0};
  if (state == LEME_DESK_STATE_NORMAL) {
    leme_desk_style_default_normal(config, element, look);
  } else if (element == LEME_DESK_ELEMENT_TITLEBAR &&
             state == LEME_DESK_STATE_INACTIVE) {
    leme_desk_style_hex(look, LEME_DESK_PROPERTY_BACKGROUND, 0x202326ffu);
    leme_desk_style_hex(look, LEME_DESK_PROPERTY_COLOR, 0xa1a9b1ffu);
  } else if (element == LEME_DESK_ELEMENT_BUTTON &&
             state == LEME_DESK_STATE_DISABLED) {
    look->opacity = 0.35;
    look->set |= leme_desk_style_bit(LEME_DESK_PROPERTY_OPACITY);
  } else if ((element == LEME_DESK_ELEMENT_MENU_ITEM &&
              state == LEME_DESK_STATE_HOVER) ||
             (element == LEME_DESK_ELEMENT_ICON &&
              state == LEME_DESK_STATE_DROP)) {
    leme_desk_style_accent(config, 0.45f, color);
    leme_desk_style_color(look, LEME_DESK_PROPERTY_BACKGROUND, color);
    leme_desk_style_accent(config, 0.8f, color);
    leme_desk_style_color(look, LEME_DESK_PROPERTY_BORDER_COLOR, color);
  } else if (element == LEME_DESK_ELEMENT_MENU_ITEM &&
             state == LEME_DESK_STATE_DISABLED) {
    leme_desk_style_hex(look, LEME_DESK_PROPERTY_COLOR, 0xa1a9b1ffu);
  } else if (element == LEME_DESK_ELEMENT_STRIP &&
             state == LEME_DESK_STATE_DISABLED) {
    leme_desk_style_hex(look, LEME_DESK_PROPERTY_COLOR, 0x4d4d4dffu);
  } else if (((element == LEME_DESK_ELEMENT_PROMPT_FIELD ||
               element == LEME_DESK_ELEMENT_PROMPT_BUTTON) &&
              state == LEME_DESK_STATE_FOCUS) ||
             (element == LEME_DESK_ELEMENT_ICON &&
              state == LEME_DESK_STATE_SELECTED)) {
    leme_desk_style_accent(config, 1.0f, color);
    leme_desk_style_color(look, LEME_DESK_PROPERTY_BORDER_COLOR, color);
  }
}

static void leme_desk_style_apply(const struct leme_config *config,
                                  enum leme_desk_element element,
                                  enum leme_desk_state state,
                                  struct leme_desk_look *look) {
  struct leme_desk_look base = {0};
  leme_desk_style_default(config, element, state, &base);
  leme_desk_style_overlay(look, &base);
  const struct leme_desk_style_rule *rule =
      leme_desk_style_rule(config, element, state);
  if (rule != NULL) {
    leme_desk_style_overlay(look, &rule->look);
  }
}

void leme_desk_style_resolve(const struct leme_config *config,
                             enum leme_desk_element element, uint32_t states,
                             struct leme_desk_look *look) {
  *look = (struct leme_desk_look){0};
  if ((size_t)element >= LEME_DESK_ELEMENT_COUNT) {
    return;
  }
  const struct leme_desk_style_element_info *info =
      &leme_desk_style_elements[element];
  if (info->inside != LEME_DESK_ELEMENT_NONE) {
    struct leme_desk_look parent = {0};
    leme_desk_style_resolve(config, info->inside, states, &parent);
    const uint32_t inherited = leme_desk_style_bit(LEME_DESK_PROPERTY_COLOR) |
                               leme_desk_style_bit(LEME_DESK_PROPERTY_FONT);
    memcpy(look->color, parent.color, sizeof(look->color));
    look->font = parent.font;
    look->set = parent.set & inherited;
  }
  for (size_t index = 0; index < LEME_DESK_STATE_COUNT; index++) {
    const enum leme_desk_state state = (enum leme_desk_state)index;
    if (state != LEME_DESK_STATE_NORMAL &&
        (states & LEME_DESK_STATE_BIT(state)) == 0) {
      continue;
    }
    if (info->extends != LEME_DESK_ELEMENT_NONE) {
      leme_desk_style_apply(config, info->extends, state, look);
    }
    leme_desk_style_apply(config, element, state, look);
  }
}

int leme_desk_style_titlebar_height(const struct leme_config *config) {
  struct leme_desk_look look = {0};
  leme_desk_style_resolve(config, LEME_DESK_ELEMENT_TITLEBAR, 0, &look);
  return look.height;
}

int leme_desk_style_button_width(const struct leme_config *config) {
  struct leme_desk_look look = {0};
  leme_desk_style_resolve(config, LEME_DESK_ELEMENT_BUTTON, 0, &look);
  return look.width;
}

void leme_desk_style_titlebar(const struct leme_config *config,
                              struct leme_titlebar_style *style) {
  struct leme_desk_look active = {0};
  struct leme_desk_look inactive = {0};
  leme_desk_style_resolve(config, LEME_DESK_ELEMENT_TITLEBAR, 0, &active);
  leme_desk_style_resolve(config, LEME_DESK_ELEMENT_TITLEBAR,
                          LEME_DESK_STATE_BIT(LEME_DESK_STATE_INACTIVE),
                          &inactive);
  *style = (struct leme_titlebar_style){
      .align = active.align,
      .button_count = active.layout.count,
      .left = active.layout.left,
      .height = active.height,
      .button_width = leme_desk_style_button_width(config),
  };
  memcpy(style->background_active, active.background,
         sizeof(style->background_active));
  memcpy(style->background_inactive, inactive.background,
         sizeof(style->background_inactive));
  memcpy(style->text_active, active.color, sizeof(style->text_active));
  memcpy(style->text_inactive, inactive.color, sizeof(style->text_inactive));
  memcpy(style->buttons, active.layout.buttons, sizeof(style->buttons));
  (void)snprintf(style->font, sizeof(style->font), "%s", active.font);
}

void leme_desk_style_destroy(struct leme_desk_style *style) {
  if (style == NULL) {
    return;
  }
  for (size_t element = 0; element < LEME_DESK_ELEMENT_COUNT; element++) {
    for (size_t state = 0; state < LEME_DESK_STATE_COUNT; state++) {
      struct leme_desk_style_rule *rule = &style->rules[element][state];
      free(rule->font);
      free(rule->icon);
      free(rule->theme);
    }
  }
  free(style);
}
