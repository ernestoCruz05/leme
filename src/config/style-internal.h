#ifndef LEME_CONFIG_STYLE_INTERNAL_H
#define LEME_CONFIG_STYLE_INTERNAL_H

#include "config/style.h"

enum leme_desk_style_kind {
  LEME_DESK_STYLE_COLOR,
  LEME_DESK_STYLE_TEXT,
  LEME_DESK_STYLE_PIXELS,
  LEME_DESK_STYLE_DECIMAL,
  LEME_DESK_STYLE_ALIGN,
  LEME_DESK_STYLE_LAYOUT,
  LEME_DESK_STYLE_BOOLEAN,
  LEME_DESK_STYLE_PATH,
};

struct leme_desk_style_element_info {
  const char *name;
  enum leme_desk_element extends;
  enum leme_desk_element inside;
  uint32_t states;
  uint32_t properties;
};

struct leme_desk_style_property_info {
  const char *name;
  enum leme_desk_style_kind kind;
  int low;
  int high;
};

#define LEME_DESK_STYLE_PATH_MAX 4096

struct leme_desk_style_rule {
  struct leme_desk_look look;
  char *font;
  char *icon;
  char *theme;
  int lineno;
};

struct leme_desk_style {
  struct leme_desk_style_rule rules[LEME_DESK_ELEMENT_COUNT]
                                   [LEME_DESK_STATE_COUNT];
};

extern const struct leme_desk_style_element_info
    leme_desk_style_elements[LEME_DESK_ELEMENT_COUNT];
extern const struct leme_desk_style_property_info
    leme_desk_style_properties[LEME_DESK_PROPERTY_COUNT];
extern const char *const leme_desk_style_state_names[LEME_DESK_STATE_COUNT - 1];
extern const char *const leme_desk_style_button_names[3];

uint32_t leme_desk_style_bit(enum leme_desk_property property);
float *leme_desk_style_color_slot(struct leme_desk_look *look,
                                  enum leme_desk_property property);
int *leme_desk_style_pixels_slot(struct leme_desk_look *look,
                                 enum leme_desk_property property);

#endif
