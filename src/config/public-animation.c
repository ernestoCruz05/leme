#include "config/public-internal.h"
#include "public/schema.h"
#include "public/value-internal.h"

static const char *kind_name(enum leme_animation_kind kind) {
  switch (kind) {
  case LEME_ANIMATION_KIND_EASING:
    return "easing";
  case LEME_ANIMATION_KIND_SPRING:
    return "spring";
  }
  return NULL;
}

static const char *style_name(enum leme_workspace_animation_style style) {
  switch (style) {
  case LEME_WORKSPACE_ANIMATION_FULL_SLIDE:
    return "full_slide";
  case LEME_WORKSPACE_ANIMATION_GLIDE_FADE:
    return "glide_fade";
  }
  return NULL;
}

static enum leme_public_status
curve_value(struct leme_public_builder *b,
            const struct leme_animation_curve *curve,
            struct leme_public_value **out) {
  struct leme_public_value *record = NULL;
  if (leme_public_object(b, 4, &record) != LEME_PUBLIC_OK ||
      leme_public_put_number(b, record, LEME_PUBLIC_TEXT("x1"), curve->x1) !=
          LEME_PUBLIC_OK ||
      leme_public_put_number(b, record, LEME_PUBLIC_TEXT("y1"), curve->y1) !=
          LEME_PUBLIC_OK ||
      leme_public_put_number(b, record, LEME_PUBLIC_TEXT("x2"), curve->x2) !=
          LEME_PUBLIC_OK ||
      leme_public_put_number(b, record, LEME_PUBLIC_TEXT("y2"), curve->y2) !=
          LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  *out = record;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status
spring_value(struct leme_public_builder *b,
             const struct leme_animation_spring *spring,
             struct leme_public_value **out) {
  struct leme_public_value *record = NULL;
  if (leme_public_object(b, 3, &record) != LEME_PUBLIC_OK ||
      leme_public_put_number(b, record, LEME_PUBLIC_TEXT("damping_ratio"),
                             spring->damping_ratio) != LEME_PUBLIC_OK ||
      leme_public_put_number(b, record, LEME_PUBLIC_TEXT("stiffness"),
                             spring->stiffness) != LEME_PUBLIC_OK ||
      leme_public_put_number(b, record, LEME_PUBLIC_TEXT("epsilon"),
                             spring->epsilon) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  *out = record;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status
interpolation(struct leme_public_builder *b, struct leme_public_value *record,
              enum leme_animation_kind kind, bool configured,
              const struct leme_animation_curve *curve,
              const struct leme_animation_curve *opacity_curve,
              const struct leme_animation_spring *spring) {
  const char *name = kind_name(kind);
  if (name == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  struct leme_public_value *motion = NULL, *opacity = NULL, *physics = NULL;
  const bool easing = configured && kind == LEME_ANIMATION_KIND_EASING;
  const bool physical = configured && kind == LEME_ANIMATION_KIND_SPRING;
  if ((easing ? curve_value(b, curve, &motion)
              : leme_public_null(b, &motion)) != LEME_PUBLIC_OK ||
      (easing ? curve_value(b, opacity_curve, &opacity)
              : leme_public_null(b, &opacity)) != LEME_PUBLIC_OK ||
      (physical ? spring_value(b, spring, &physics)
                : leme_public_null(b, &physics)) != LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("kind"), name) !=
          LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("curve"), motion) !=
          LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("opacity_curve"),
                             opacity) != LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("spring"), physics) !=
          LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  return LEME_PUBLIC_OK;
}

static enum leme_public_status effects_value(struct leme_public_builder *b,
                                             uint32_t effects,
                                             struct leme_public_value **out) {
  const uint32_t known =
      LEME_ANIMATION_EFFECT_FADE | LEME_ANIMATION_EFFECT_SCALE;
  if ((effects & ~known) != 0)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  const bool fade = (effects & LEME_ANIMATION_EFFECT_FADE) != 0;
  const bool scale = (effects & LEME_ANIMATION_EFFECT_SCALE) != 0;
  struct leme_public_value *array = NULL;
  if (leme_public_array(b, (size_t)fade + (size_t)scale, &array) !=
      LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  if (fade) {
    struct leme_public_value *name = NULL;
    if (leme_config_public_text(b, "fade", &name) != LEME_PUBLIC_OK ||
        leme_public_array_set(b, array, 0, name) != LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
  }
  if (scale) {
    struct leme_public_value *name = NULL;
    if (leme_config_public_text(b, "scale", &name) != LEME_PUBLIC_OK ||
        leme_public_array_set(b, array, fade ? 1u : 0u, name) != LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
  }
  *out = array;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status
event_value(struct leme_public_builder *b,
            const struct leme_animation_settings *settings,
            struct leme_public_value **out) {
  struct leme_public_value *record = NULL, *effects = NULL;
  if (leme_public_object(b, 8, &record) != LEME_PUBLIC_OK ||
      interpolation(b, record, settings->kind, settings->configured,
                    &settings->curve, &settings->opacity_curve,
                    &settings->spring) != LEME_PUBLIC_OK ||
      effects_value(b, settings->effects, &effects) != LEME_PUBLIC_OK ||
      leme_public_put_bool(b, record, LEME_PUBLIC_TEXT("configured"),
                           settings->configured) != LEME_PUBLIC_OK ||
      leme_public_put_int(b, record, LEME_PUBLIC_TEXT("duration_ms"),
                          settings->duration_ms) != LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("effects"), effects) !=
          LEME_PUBLIC_OK ||
      (settings->configured
           ? leme_public_put_number(b, record, LEME_PUBLIC_TEXT("scale_from"),
                                    settings->scale_from)
           : leme_public_put_null(b, record, LEME_PUBLIC_TEXT("scale_from"))) !=
          LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  *out = record;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status
workspace_value(struct leme_public_builder *b,
                const struct leme_workspace_animation_settings *settings,
                struct leme_public_value **out) {
  const char *style = style_name(settings->style);
  if (style == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  struct leme_public_value *record = NULL;
  if (leme_public_object(b, 8, &record) != LEME_PUBLIC_OK ||
      interpolation(b, record, settings->kind, settings->configured,
                    &settings->curve, &settings->opacity_curve,
                    &settings->spring) != LEME_PUBLIC_OK ||
      leme_public_put_bool(b, record, LEME_PUBLIC_TEXT("configured"),
                           settings->configured) != LEME_PUBLIC_OK ||
      leme_public_put_int(b, record, LEME_PUBLIC_TEXT("duration_ms"),
                          settings->duration_ms) != LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("style"), style) !=
          LEME_PUBLIC_OK ||
      leme_public_put_number(b, record, LEME_PUBLIC_TEXT("distance"),
                             settings->distance) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  *out = record;
  return LEME_PUBLIC_OK;
}

enum leme_public_status
leme_config_public_animation(struct leme_public_builder *b,
                             const struct leme_config *config,
                             struct leme_public_value **out) {
  if (out == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  *out = NULL;
  if (config == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  struct leme_public_value *record = NULL, *open = NULL, *close = NULL,
                           *workspace = NULL, *wrapper = NULL;
  if (leme_public_object(b, 3, &record) != LEME_PUBLIC_OK ||
      event_value(b, &config->animation[LEME_ANIMATION_OPEN], &open) !=
          LEME_PUBLIC_OK ||
      event_value(b, &config->animation[LEME_ANIMATION_CLOSE], &close) !=
          LEME_PUBLIC_OK ||
      workspace_value(b, &config->workspace_animation, &workspace) !=
          LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("open"), open) !=
          LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("close"), close) !=
          LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("workspace"),
                             workspace) != LEME_PUBLIC_OK ||
      leme_public_object(b, 1, &wrapper) != LEME_PUBLIC_OK ||
      leme_public_object_set(b, wrapper, LEME_PUBLIC_TEXT("animation"),
                             record) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  const enum leme_public_status status = leme_public_settings_validate(wrapper);
  if (status != LEME_PUBLIC_OK)
    return leme_public_fail(b, status);
  *out = record;
  return LEME_PUBLIC_OK;
}
