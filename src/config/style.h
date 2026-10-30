#ifndef LEME_CONFIG_STYLE_H
#define LEME_CONFIG_STYLE_H

#include "config/config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LEME_STYLE_STATE(id, name)
#define LEME_STYLE_PROPERTY(id, name, kind, low, high)
#define LEME_STYLE_ELEMENT(id, name, extends, inside, states, properties)      \
  LEME_DESK_ELEMENT_##id,
enum leme_desk_element {
#include "config/style.def"
  LEME_DESK_ELEMENT_COUNT,
  LEME_DESK_ELEMENT_NONE = LEME_DESK_ELEMENT_COUNT,
};
#undef LEME_STYLE_STATE
#undef LEME_STYLE_PROPERTY
#undef LEME_STYLE_ELEMENT

#define LEME_STYLE_STATE(id, name) LEME_DESK_STATE_##id,
#define LEME_STYLE_PROPERTY(id, name, kind, low, high)
#define LEME_STYLE_ELEMENT(id, name, extends, inside, states, properties)
enum leme_desk_state {
  LEME_DESK_STATE_NORMAL,
#include "config/style.def"
  LEME_DESK_STATE_COUNT,
};
#undef LEME_STYLE_STATE
#undef LEME_STYLE_PROPERTY
#undef LEME_STYLE_ELEMENT

#define LEME_STYLE_STATE(id, name)
#define LEME_STYLE_PROPERTY(id, name, kind, low, high) LEME_DESK_PROPERTY_##id,
#define LEME_STYLE_ELEMENT(id, name, extends, inside, states, properties)
enum leme_desk_property {
#include "config/style.def"
  LEME_DESK_PROPERTY_COUNT,
};
#undef LEME_STYLE_STATE
#undef LEME_STYLE_PROPERTY
#undef LEME_STYLE_ELEMENT

#define LEME_DESK_STATE_BIT(state) (UINT32_C(1) << (state))
#define LEME_DESK_STYLE_ICON_THEME "Adwaita"

struct leme_desk_style;

struct leme_desk_layout {
  enum leme_titlebar_button buttons[3];
  size_t count;
  size_t left;
};

struct leme_desk_look {
  uint32_t set;
  float background[4];
  float color[4];
  float border_color[4];
  const char *font;
  const char *icon;
  const char *theme;
  int radius;
  int width;
  int height;
  int padding;
  int size;
  int spacing;
  int gap;
  double opacity;
  enum leme_titlebar_align align;
  struct leme_desk_layout layout;
  bool shadow;
};

void leme_desk_style_resolve(const struct leme_config *config,
                             enum leme_desk_element element, uint32_t states,
                             struct leme_desk_look *look);
void leme_desk_style_titlebar(const struct leme_config *config,
                              struct leme_titlebar_style *style);
int leme_desk_style_titlebar_height(const struct leme_config *config);
int leme_desk_style_button_width(const struct leme_config *config);
void leme_desk_style_destroy(struct leme_desk_style *style);

#endif
