#include "config/config.h"
#include "config/internal.h"
#include "config/scfg.h"

#include "core/command.h"

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

struct leme_variable {
    char *name;
    char *value;
};

static bool
leme_config_reject(struct leme_config *config, int lineno,
    const char *format, ...)
{
    va_list arguments;
    char message[256];
    int written;

    va_start(arguments, format);
    written = vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    if (written < 0) {
        return false;
    }
    return leme_diagnostics_add(&config->diagnostics, lineno, "%s", message);
}

struct leme_variables {
    struct leme_variable *entries;
    size_t count;
};

enum leme_bind_state {
    LEME_BIND_UNVISITED,
    LEME_BIND_RESOLVING,
    LEME_BIND_RESOLVED,
};

struct leme_bind_group {
    char *name;
    struct leme_binding *bindings;
    size_t binding_count;
    char **inherits;
    size_t inherit_count;
    bool is_mode;
    bool escape_exits;
    int lineno;
    enum leme_bind_state state;
    const struct leme_binding **resolved;
    const char **origin;
    size_t resolved_count;
};

struct leme_bind_scope {
    struct leme_bind_group *groups;
    size_t count;
};
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon-keysyms.h>

static bool
leme_config_parse_u16(const char *text, uint16_t *value)
{
    char *end;
    unsigned long parsed;

    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || text[0] == '\0' || *end != '\0' ||
            parsed == 0 || parsed > UINT16_MAX) {
        return false;
    }
    *value = (uint16_t)parsed;
    return true;
}

static bool
leme_config_parse_u16_allow_zero(const char *text, uint16_t *value)
{
    char *end;
    unsigned long parsed;

    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || text[0] == '\0' || *end != '\0' ||
            parsed > UINT16_MAX) {
        return false;
    }
    *value = (uint16_t)parsed;
    return true;
}

static bool
leme_config_parse_layout_kind(const char *text, enum leme_layout_kind *kind)
{
    if (strcmp(text, "dwindle") == 0) {
        *kind = LEME_LAYOUT_DWINDLE;
    } else if (strcmp(text, "master_stack") == 0) {
        *kind = LEME_LAYOUT_MASTER_STACK;
    } else if (strcmp(text, "accordion") == 0) {
        *kind = LEME_LAYOUT_ACCORDION;
    } else {
        return false;
    }
    return true;
}

static bool
leme_config_parse_tag_rule(struct leme_config *config,
    const struct leme_scfg_directive *directive, const char *path, char **error);

static bool
leme_config_parse_window_rule(struct leme_config *config,
    const struct leme_scfg_directive *directive, const char *path, char **error);

static bool
leme_config_parse_scratchpad(struct leme_config *config,
    const struct leme_scfg_directive *directive, const char *path, char **error);

static bool
leme_config_parse_boolean(const char *text, bool *value);

static bool
leme_config_parse_decimal(const char *text, double *value);

static bool
leme_config_parse_nonnegative(const char *text, int *value)
{
    char *end;
    long parsed;

    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno != 0 || text[0] == '\0' || *end != '\0' ||
            parsed < 0 || parsed > INT_MAX) {
        return false;
    }
    *value = (int)parsed;
    return true;
}

static bool
leme_config_parse_tags(struct leme_config *config,
    const struct leme_scfg_directive *directive, const char *path, char **error)
{
    bool have_initial = false;
    bool have_maximum = false;
    bool have_drop_mode = false;
    bool have_layout = false;
    size_t index;

    if (directive->params_len != 0) {
        leme_config_set_error(error, "%s:%d: tags takes no arguments",
            path, directive->lineno);
        return false;
    }
    for (index = 0; index < directive->children.directives_len; index++) {
        const struct leme_scfg_directive *entry =
            &directive->children.directives[index];
        uint16_t value;

        if (strcmp(entry->name, "initial") == 0 && !have_initial) {
            if (entry->params_len != 1 ||
                    entry->children.directives_len != 0 ||
                    !leme_config_parse_u16(entry->params[0], &value)) {
                if (!leme_config_reject(config, entry->lineno,
                        "initial requires one positive integer")) {
                    return false;
                }
                continue;
            }
            config->initial_tags = value;
            have_initial = true;
        } else if (strcmp(entry->name, "maximum") == 0 && !have_maximum) {
            if (entry->params_len != 1 ||
                    entry->children.directives_len != 0 ||
                    !leme_config_parse_u16(entry->params[0], &value)) {
                if (!leme_config_reject(config, entry->lineno,
                        "maximum requires one positive integer")) {
                    return false;
                }
                continue;
            }
            config->max_tags = value;
            have_maximum = true;
        } else if (strcmp(entry->name, "drop_mode") == 0 &&
                !have_drop_mode) {
            if (entry->params_len != 1 ||
                    entry->children.directives_len != 0) {
                if (!leme_config_reject(config, entry->lineno,
                        "drop_mode must be simple or edges")) {
                    return false;
                }
                continue;
            }
            if (strcmp(entry->params[0], "simple") == 0) {
                config->drop_mode = LEME_DROP_MODE_SIMPLE;
            } else if (strcmp(entry->params[0], "edges") == 0) {
                config->drop_mode = LEME_DROP_MODE_EDGES;
            } else {
                if (!leme_config_reject(config, entry->lineno,
                        "drop_mode must be simple or edges")) {
                    return false;
                }
                continue;
            }
            config->tag_defaults.drop_mode = config->drop_mode;
            have_drop_mode = true;
        } else if (strcmp(entry->name, "layout") == 0 && !have_layout) {
            if (entry->params_len != 1 ||
                    entry->children.directives_len != 0 ||
                    !leme_config_parse_layout_kind(entry->params[0],
                        &config->tag_defaults.layout)) {
                if (!leme_config_reject(config, entry->lineno,
                        "layout must be dwindle, master_stack, "
                        "or accordion")) {
                    return false;
                }
                continue;
            }
            have_layout = true;
        } else if (strcmp(entry->name, "tag") == 0) {
            if (!have_maximum) {
                if (!leme_config_reject(config, entry->lineno,
                        "tag requires maximum first")) {
                    return false;
                }
                continue;
            }
            if (!leme_config_parse_tag_rule(config, entry, path, error)) {
                return false;
            }
        } else if (!leme_config_reject(config, entry->lineno,
                "duplicate or unknown tags directive %s", entry->name)) {
            return false;
        }
    }
    return true;
}

static bool
leme_config_parse_color(const char *text, float value[4])
{
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

static bool
leme_config_parse_opacity(const char *text, double *value)
{
    char *end;
    double parsed;

    errno = 0;
    parsed = strtod(text, &end);
    if (errno != 0 || text[0] == '\0' || *end != '\0' ||
            !isfinite(parsed) || parsed < 0.0 || parsed > 1.0) {
        return false;
    }
    *value = parsed;
    return true;
}

static bool
leme_config_scratchpad_name_valid(const char *name)
{
    return name[0] != '\0' && strpbrk(name, " \t\n\r\f\v") == NULL;
}

static void
leme_config_finish_scratchpad(struct leme_scratchpad_config *scratchpad)
{
    free(scratchpad->name);
    free(scratchpad->identity);
    leme_config_free_argv(scratchpad->spawn);
    *scratchpad = (struct leme_scratchpad_config){0};
}

static bool
leme_config_scratchpad_exists(const struct leme_config *config,
    const struct leme_scratchpad_config *scratchpad)
{
    size_t index;

    for (index = 0; index < config->scratchpad_count; index++) {
        const struct leme_scratchpad_config *previous =
            &config->scratchpads[index];

        if (strcmp(previous->name, scratchpad->name) == 0 ||
                strcmp(previous->identity, scratchpad->identity) == 0) {
            return true;
        }
    }
    return false;
}

static bool
leme_config_parse_scratchpad(struct leme_config *config,
    const struct leme_scfg_directive *directive, const char *path, char **error)
{
    struct leme_scratchpad_config scratchpad = {
        .width = 0.6,
        .height = 0.6,
        .line = directive->lineno,
    };
    bool have_identity = false;
    bool have_spawn = false;
    bool have_width = false;
    bool have_height = false;
    bool complete = true;
    size_t index;

    if (directive->params_len != 1 ||
            !leme_config_scratchpad_name_valid(directive->params_len == 1 ?
                directive->params[0] : "")) {
        return leme_config_reject(config, directive->lineno,
            "scratchpad requires one nonempty name without whitespace");
    }
    scratchpad.name = strdup(directive->params[0]);
    if (scratchpad.name == NULL) {
        leme_config_set_error(error, "%s:%d: out of memory",
            path, directive->lineno);
        return false;
    }
    for (index = 0; index < directive->children.directives_len; index++) {
        const struct leme_scfg_directive *entry =
            &directive->children.directives[index];
        double dimension;

        if (strcmp(entry->name, "identity") == 0) {
            if (have_identity || entry->params_len != 1 ||
                    entry->children.directives_len != 0 ||
                    entry->params[0][0] == '\0') {
                if (!leme_config_reject(config, entry->lineno,
                        "identity requires one nonempty value")) {
                    leme_config_finish_scratchpad(&scratchpad);
                    return false;
                }
                complete = false;
                continue;
            }
            scratchpad.identity = strdup(entry->params[0]);
            if (scratchpad.identity == NULL) {
                leme_config_finish_scratchpad(&scratchpad);
                leme_config_set_error(error, "%s:%d: out of memory",
                    path, entry->lineno);
                return false;
            }
            have_identity = true;
        } else if (strcmp(entry->name, "spawn") == 0) {
            if (have_spawn || entry->params_len == 0 ||
                    entry->params[0][0] == '\0' ||
                    entry->children.directives_len != 0) {
                if (!leme_config_reject(config, entry->lineno,
                        "spawn requires a nonempty command")) {
                    leme_config_finish_scratchpad(&scratchpad);
                    return false;
                }
                complete = false;
                continue;
            }
            scratchpad.spawn = leme_config_copy_argv(entry->params[0],
                entry->params + 1, entry->params_len - 1);
            if (scratchpad.spawn == NULL) {
                leme_config_finish_scratchpad(&scratchpad);
                leme_config_set_error(error, "%s:%d: out of memory",
                    path, entry->lineno);
                return false;
            }
            have_spawn = true;
        } else if (strcmp(entry->name, "width") == 0 ||
                strcmp(entry->name, "height") == 0) {
            const bool width = strcmp(entry->name, "width") == 0;
            bool *have_dimension = width ? &have_width : &have_height;

            if (*have_dimension || entry->params_len != 1 ||
                    entry->children.directives_len != 0 ||
                    !leme_config_parse_decimal(entry->params[0], &dimension) ||
                    dimension < 0.1 || dimension > 1.0) {
                if (!leme_config_reject(config, entry->lineno,
                        "%s requires a finite decimal from 0.1 through 1.0",
                        entry->name)) {
                    leme_config_finish_scratchpad(&scratchpad);
                    return false;
                }
                complete = false;
                continue;
            }
            if (width) {
                scratchpad.width = dimension;
            } else {
                scratchpad.height = dimension;
            }
            *have_dimension = true;
        } else {
            if (!leme_config_reject(config, entry->lineno,
                    "unknown scratchpad property %s", entry->name)) {
                leme_config_finish_scratchpad(&scratchpad);
                return false;
            }
            complete = false;
        }
    }
    if (!have_identity || !have_spawn) {
        if (!leme_config_reject(config, directive->lineno,
                "scratchpad requires identity and spawn")) {
            leme_config_finish_scratchpad(&scratchpad);
            return false;
        }
        complete = false;
    }
    if (complete && leme_config_scratchpad_exists(config, &scratchpad)) {
        if (!leme_config_reject(config, directive->lineno,
                "duplicate scratchpad name or identity %s", scratchpad.name)) {
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
        leme_config_set_error(error, "%s:%d: too many scratchpads",
            path, directive->lineno);
        return false;
    }
    {
        struct leme_scratchpad_config *scratchpads = realloc(
            config->scratchpads, (config->scratchpad_count + 1) *
            sizeof(*scratchpads));

        if (scratchpads == NULL) {
            leme_config_finish_scratchpad(&scratchpad);
            leme_config_set_error(error, "%s:%d: out of memory",
                path, directive->lineno);
            return false;
        }
        config->scratchpads = scratchpads;
    }
    config->scratchpads[config->scratchpad_count++] = scratchpad;
    return true;
}

static bool
leme_config_parse_window_rule(struct leme_config *config,
    const struct leme_scfg_directive *directive, const char *path, char **error)
{
    struct leme_window_rule *rules;
    struct leme_window_rule *rule;
    char *identity;
    size_t index;

    if (directive->params_len != 1) {
        return leme_config_reject(config, directive->lineno,
            "window requires one application identity");
    }
    identity = strdup(directive->params[0]);
    if (identity == NULL) {
        leme_config_set_error(error, "%s:%d: out of memory",
            path, directive->lineno);
        return false;
    }
    rules = realloc(config->window_rules,
        (config->window_rule_count + 1) * sizeof(*rules));
    if (rules == NULL) {
        free(identity);
        leme_config_set_error(error, "%s:%d: out of memory",
            path, directive->lineno);
        return false;
    }
    config->window_rules = rules;
    rule = &config->window_rules[config->window_rule_count];
    *rule = (struct leme_window_rule){
        .identity = identity,
    };
    config->window_rule_count++;

    for (index = 0; index < directive->children.directives_len; index++) {
        const struct leme_scfg_directive *entry =
            &directive->children.directives[index];
        int value;
        double decimal;
        bool flag;

        if (entry->params_len != 1 || entry->children.directives_len != 0) {
            if (!leme_config_reject(config, entry->lineno,
                    "%s requires one value", entry->name)) {
                return false;
            }
            continue;
        }
        if (strcmp(entry->name, "title") == 0 && rule->title == NULL) {
            rule->title = strdup(entry->params[0]);
            if (rule->title == NULL) {
                leme_config_set_error(error, "%s:%d: out of memory",
                    path, entry->lineno);
                return false;
            }
        } else if (strcmp(entry->name, "tag") == 0 &&
                (rule->fields & LEME_WINDOW_RULE_TAG) == 0) {
            if (!leme_config_parse_nonnegative(entry->params[0], &value) ||
                    value < 1 || value > UINT16_MAX) {
                if (!leme_config_reject(config, entry->lineno,
                        "tag requires a positive integer")) {
                    return false;
                }
                continue;
            }
            rule->tag_id = (uint16_t)value;
            rule->fields |= LEME_WINDOW_RULE_TAG;
        } else if (strcmp(entry->name, "floating") == 0 &&
                (rule->fields & LEME_WINDOW_RULE_FLOATING) == 0) {
            if (!leme_config_parse_boolean(entry->params[0], &flag)) {
                if (!leme_config_reject(config, entry->lineno,
                        "floating expects true or false")) {
                    return false;
                }
                continue;
            }
            rule->floating = flag;
            rule->fields |= LEME_WINDOW_RULE_FLOATING;
        } else if (strcmp(entry->name, "fullscreen") == 0 &&
                (rule->fields & LEME_WINDOW_RULE_FULLSCREEN) == 0) {
            if (!leme_config_parse_boolean(entry->params[0], &flag)) {
                if (!leme_config_reject(config, entry->lineno,
                        "fullscreen expects true or false")) {
                    return false;
                }
                continue;
            }
            rule->fullscreen = flag;
            rule->fields |= LEME_WINDOW_RULE_FULLSCREEN;
        } else if (strcmp(entry->name, "output") == 0 &&
                (rule->fields & LEME_WINDOW_RULE_OUTPUT) == 0) {
            rule->output = strdup(entry->params[0]);
            if (rule->output == NULL) {
                leme_config_set_error(error, "%s:%d: out of memory",
                    path, entry->lineno);
                return false;
            }
            rule->fields |= LEME_WINDOW_RULE_OUTPUT;
        } else if (strcmp(entry->name, "opacity") == 0 &&
                (rule->fields & LEME_WINDOW_RULE_OPACITY) == 0) {
            if (!leme_config_parse_opacity(entry->params[0], &decimal)) {
                if (!leme_config_reject(config, entry->lineno,
                        "opacity expects a decimal from 0.0 through 1.0")) {
                    return false;
                }
                continue;
            }
            rule->opacity = decimal;
            rule->fields |= LEME_WINDOW_RULE_OPACITY;
        } else if (!leme_config_reject(config, entry->lineno,
                "duplicate or unknown window directive %s", entry->name)) {
            return false;
        }
    }
    return true;
}

#define LEME_ANIMATION_DURATION_MAX 1000u

static bool
leme_config_parse_decimal(const char *text, double *value)
{
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

static bool
leme_config_parse_animation_curve(const struct leme_scfg_directive *entry,
    struct leme_animation_curve *curve)
{
    double points[4];
    size_t index;

    if (entry->params_len == 1) {
        const char *text = entry->params[0];

        if (strcmp(text, "linear") == 0) {
            *curve = leme_animation_curve_preset(LEME_ANIMATION_CURVE_LINEAR);
        } else if (strcmp(text, "ease_in") == 0) {
            *curve = leme_animation_curve_preset(LEME_ANIMATION_CURVE_EASE_IN);
        } else if (strcmp(text, "ease_out") == 0) {
            *curve = leme_animation_curve_preset(
                LEME_ANIMATION_CURVE_EASE_OUT);
        } else if (strcmp(text, "ease_in_out") == 0) {
            *curve = leme_animation_curve_preset(
                LEME_ANIMATION_CURVE_EASE_IN_OUT);
        } else {
            return false;
        }
        return true;
    }
    if (entry->params_len != 4) {
        return false;
    }
    for (index = 0; index < LEME_ARRAY_LENGTH(points); index++) {
        if (!leme_config_parse_decimal(entry->params[index],
                &points[index])) {
            return false;
        }
    }
    /*
     * O x tem de ficar em 0..1: a curva é procurada por bissecção nele. O y
     * fica livre, para que um sobreimpulso continue a ser configurável.
     */
    if (points[0] < 0.0 || points[0] > 1.0 ||
            points[2] < 0.0 || points[2] > 1.0) {
        return false;
    }
    *curve = (struct leme_animation_curve){
        points[0], points[1], points[2], points[3],
    };
    return true;
}

static bool
leme_config_parse_animation_effects(const struct leme_scfg_directive *entry,
    uint32_t *effects)
{
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

static bool
leme_config_parse_animation_event(struct leme_config *config,
    const struct leme_scfg_directive *directive,
    struct leme_animation_settings *settings)
{
    bool have_duration = false;
    bool have_curve = false;
    bool have_opacity_curve = false;
    bool have_effect = false;
    bool have_scale_from = false;
    size_t index;

    settings->duration_ms = 150;
    settings->curve =
        leme_animation_curve_preset(LEME_ANIMATION_CURVE_EASE_OUT);
    settings->opacity_curve = settings->curve;
    settings->effects = LEME_ANIMATION_EFFECT_FADE;
    settings->scale_from = 0.92;
    for (index = 0; index < directive->children.directives_len; index++) {
        const struct leme_scfg_directive *entry =
            &directive->children.directives[index];
        struct leme_animation_curve curve;
        uint32_t effects;
        double decimal;
        int value;

        if (entry->children.directives_len != 0) {
            if (!leme_config_reject(config, entry->lineno,
                    "%s takes no block", entry->name)) {
                return false;
            }
            continue;
        }
        if (strcmp(entry->name, "effect") == 0 && !have_effect) {
            if (entry->params_len == 0 ||
                    !leme_config_parse_animation_effects(entry, &effects)) {
                if (!leme_config_reject(config, entry->lineno,
                        "effect expects fade, scale, or none")) {
                    return false;
                }
                continue;
            }
            settings->effects = effects;
            have_effect = true;
            continue;
        }
        /*
         * As curvas aceitam quatro parâmetros, por isso passam à frente da
         * guarda de aridade e tratam a repetição por si.
         */
        if (strcmp(entry->name, "curve") == 0 ||
                strcmp(entry->name, "opacity_curve") == 0) {
            bool opacity = strcmp(entry->name, "opacity_curve") == 0;

            if (opacity ? have_opacity_curve : have_curve) {
                if (!leme_config_reject(config, entry->lineno,
                        "duplicate or unknown animation key %s",
                        entry->name)) {
                    return false;
                }
                continue;
            }
            if (!leme_config_parse_animation_curve(entry, &curve)) {
                if (!leme_config_reject(config, entry->lineno,
                        "%s expects a preset name or four decimals",
                        entry->name)) {
                    return false;
                }
                continue;
            }
            if (opacity) {
                settings->opacity_curve = curve;
                have_opacity_curve = true;
            } else {
                settings->curve = curve;
                have_curve = true;
            }
            continue;
        }
        if (entry->params_len != 1) {
            if (!leme_config_reject(config, entry->lineno,
                    "%s requires one value", entry->name)) {
                return false;
            }
            continue;
        }
        if (strcmp(entry->name, "duration") == 0 && !have_duration) {
            if (!leme_config_parse_nonnegative(entry->params[0], &value)) {
                if (!leme_config_reject(config, entry->lineno,
                        "duration requires one nonnegative integer")) {
                    return false;
                }
                continue;
            }
            if ((uint32_t)value > LEME_ANIMATION_DURATION_MAX) {
                if (!leme_config_reject(config, entry->lineno,
                        "duration is clamped to %u milliseconds",
                        LEME_ANIMATION_DURATION_MAX)) {
                    return false;
                }
                value = (int)LEME_ANIMATION_DURATION_MAX;
            }
            settings->duration_ms = (uint32_t)value;
            have_duration = true;
        } else if (strcmp(entry->name, "scale_from") == 0 &&
                !have_scale_from) {
            if (!leme_config_parse_decimal(entry->params[0], &decimal) ||
                    decimal < 0.1 || decimal > 2.0) {
                if (!leme_config_reject(config, entry->lineno,
                        "scale_from expects a decimal from 0.1 through 2.0")) {
                    return false;
                }
                continue;
            }
            settings->scale_from = decimal;
            have_scale_from = true;
        } else {
            if (!leme_config_reject(config, entry->lineno,
                    "duplicate or unknown animation key %s", entry->name)) {
                return false;
            }
        }
    }
    if (!have_opacity_curve) {
        settings->opacity_curve = settings->curve;
    }
    settings->configured = settings->duration_ms > 0 && settings->effects != 0;
    return true;
}

static bool
leme_config_parse_workspace_animation(struct leme_config *config,
    const struct leme_scfg_directive *directive,
    struct leme_workspace_animation_settings *settings)
{
    bool have_duration = false;
    bool have_style = false;
    bool have_distance = false;
    bool have_curve = false;
    bool have_opacity_curve = false;
    size_t index;

    *settings = (struct leme_workspace_animation_settings){
        .configured = true,
        .duration_ms = 180,
        .style = LEME_WORKSPACE_ANIMATION_GLIDE_FADE,
        .distance = 0.15,
        .curve = leme_animation_curve_preset(
            LEME_ANIMATION_CURVE_EASE_OUT),
    };
    settings->opacity_curve = settings->curve;
    for (index = 0; index < directive->children.directives_len; index++) {
        const struct leme_scfg_directive *entry =
            &directive->children.directives[index];
        struct leme_animation_curve curve;
        double decimal;
        int value;

        if (entry->children.directives_len != 0) {
            if (!leme_config_reject(config, entry->lineno,
                    "%s takes no block", entry->name)) {
                return false;
            }
            continue;
        }
        if (strcmp(entry->name, "curve") == 0 ||
                strcmp(entry->name, "opacity_curve") == 0) {
            const bool opacity =
                strcmp(entry->name, "opacity_curve") == 0;

            if (opacity ? have_opacity_curve : have_curve) {
                if (!leme_config_reject(config, entry->lineno,
                        "duplicate or unknown workspace animation key %s",
                        entry->name)) {
                    return false;
                }
                continue;
            }
            if (!leme_config_parse_animation_curve(entry, &curve)) {
                if (!leme_config_reject(config, entry->lineno,
                        "%s expects a preset name or four decimals",
                        entry->name)) {
                    return false;
                }
                continue;
            }
            if (opacity) {
                settings->opacity_curve = curve;
                have_opacity_curve = true;
            } else {
                settings->curve = curve;
                have_curve = true;
            }
            continue;
        }
        if (entry->params_len != 1) {
            if (!leme_config_reject(config, entry->lineno,
                    "%s requires one value", entry->name)) {
                return false;
            }
            continue;
        }
        if (strcmp(entry->name, "duration") == 0 && !have_duration) {
            if (!leme_config_parse_nonnegative(entry->params[0], &value)) {
                if (!leme_config_reject(config, entry->lineno,
                        "duration requires one nonnegative integer")) {
                    return false;
                }
                continue;
            }
            if ((uint32_t)value > LEME_ANIMATION_DURATION_MAX) {
                if (!leme_config_reject(config, entry->lineno,
                        "duration is clamped to %u milliseconds",
                        LEME_ANIMATION_DURATION_MAX)) {
                    return false;
                }
                value = (int)LEME_ANIMATION_DURATION_MAX;
            }
            settings->duration_ms = (uint32_t)value;
            have_duration = true;
        } else if (strcmp(entry->name, "style") == 0 && !have_style) {
            if (strcmp(entry->params[0], "full_slide") == 0) {
                settings->style = LEME_WORKSPACE_ANIMATION_FULL_SLIDE;
            } else if (strcmp(entry->params[0], "glide_fade") == 0) {
                settings->style = LEME_WORKSPACE_ANIMATION_GLIDE_FADE;
            } else {
                if (!leme_config_reject(config, entry->lineno,
                        "style expects full_slide or glide_fade")) {
                    return false;
                }
                continue;
            }
            have_style = true;
        } else if (strcmp(entry->name, "distance") == 0 && !have_distance) {
            if (!leme_config_parse_decimal(entry->params[0], &decimal) ||
                    decimal < 0.0 || decimal > 1.0) {
                if (!leme_config_reject(config, entry->lineno,
                        "distance expects a decimal from 0.0 through 1.0")) {
                    return false;
                }
                continue;
            }
            settings->distance = decimal;
            have_distance = true;
        } else {
            if (!leme_config_reject(config, entry->lineno,
                    "duplicate or unknown workspace animation key %s",
                    entry->name)) {
                return false;
            }
        }
    }
    if (!have_opacity_curve) {
        settings->opacity_curve = settings->curve;
    }
    settings->configured = settings->duration_ms > 0;
    return true;
}

static bool
leme_config_parse_animation(struct leme_config *config,
    const struct leme_scfg_directive *directive, const char *path, char **error)
{
    bool have[LEME_ANIMATION_EVENT_COUNT] = {false, false};
    bool have_workspace = false;
    size_t index;

    if (directive->params_len != 0) {
        leme_config_set_error(error, "%s:%d: animation takes no arguments",
            path, directive->lineno);
        return false;
    }
    for (index = 0; index < directive->children.directives_len; index++) {
        const struct leme_scfg_directive *entry =
            &directive->children.directives[index];
        enum leme_animation_event event;

        if (strcmp(entry->name, "workspace") == 0) {
            if (have_workspace || entry->params_len != 0) {
                if (!leme_config_reject(config, entry->lineno,
                        "duplicate or malformed workspace block")) {
                    return false;
                }
                continue;
            }
            have_workspace = true;
            if (!leme_config_parse_workspace_animation(config, entry,
                    &config->workspace_animation)) {
                return false;
            }
            continue;
        }
        if (strcmp(entry->name, "open") == 0) {
            event = LEME_ANIMATION_OPEN;
        } else if (strcmp(entry->name, "close") == 0) {
            event = LEME_ANIMATION_CLOSE;
        } else {
            if (!leme_config_reject(config, entry->lineno,
                    "unknown animation block %s", entry->name)) {
                return false;
            }
            continue;
        }
        if (have[event] || entry->params_len != 0) {
            if (!leme_config_reject(config, entry->lineno,
                    "duplicate or malformed %s block", entry->name)) {
                return false;
            }
            continue;
        }
        have[event] = true;
        if (!leme_config_parse_animation_event(config, entry,
                &config->animation[event])) {
            return false;
        }
    }
    return true;
}

static bool
leme_config_parse_style(struct leme_config *config,
    const struct leme_scfg_directive *directive, const char *path, char **error)
{
    bool have_gap = false;
    bool have_border = false;
    bool have_corner_radius = false;
    bool have_blur = false;
    bool have_border_active = false;
    bool have_border_inactive = false;
    bool have_opacity_active = false;
    bool have_opacity_inactive = false;
    size_t index;

    if (directive->params_len != 0) {
        leme_config_set_error(error, "%s:%d: style takes no arguments",
            path, directive->lineno);
        return false;
    }
    for (index = 0; index < directive->children.directives_len; index++) {
        const struct leme_scfg_directive *entry =
            &directive->children.directives[index];
        int value;
        double decimal;

        if (entry->params_len != 1 || entry->children.directives_len != 0) {
            if (!leme_config_reject(config, entry->lineno,
                    "%s requires one value", entry->name)) {
                return false;
            }
            continue;
        }
        if (strcmp(entry->name, "gap") == 0 && !have_gap) {
            if (!leme_config_parse_nonnegative(entry->params[0], &value)) {
                if (!leme_config_reject(config, entry->lineno,
                        "gap requires one nonnegative integer")) {
                    return false;
                }
                continue;
            }
            config->gap = value;
            have_gap = true;
        } else if (strcmp(entry->name, "border_width") == 0 && !have_border) {
            if (!leme_config_parse_nonnegative(entry->params[0], &value)) {
                if (!leme_config_reject(config, entry->lineno,
                        "border_width requires one nonnegative integer")) {
                    return false;
                }
                continue;
            }
            config->border_width = value;
            have_border = true;
        } else if (strcmp(entry->name, "corner_radius") == 0 &&
                !have_corner_radius) {
            if (!leme_config_parse_nonnegative(entry->params[0], &value)) {
                if (!leme_config_reject(config, entry->lineno,
                        "corner_radius requires one nonnegative integer")) {
                    return false;
                }
                continue;
            }
            config->corner_radius = value;
            have_corner_radius = true;
        } else if (strcmp(entry->name, "blur") == 0 && !have_blur) {
            if (!leme_config_parse_nonnegative(entry->params[0], &value) ||
                    value > 64) {
                if (!leme_config_reject(config, entry->lineno,
                        "blur requires an integer from 0 through 64")) {
                    return false;
                }
                continue;
            }
            config->blur = value;
            have_blur = true;
        } else if (strcmp(entry->name, "border_active") == 0 &&
                !have_border_active) {
            if (!leme_config_parse_color(entry->params[0],
                    config->border_active)) {
                if (!leme_config_reject(config, entry->lineno,
                        "border_active expects #RRGGBB or #RRGGBBAA")) {
                    return false;
                }
                continue;
            }
            have_border_active = true;
        } else if (strcmp(entry->name, "border_inactive") == 0 &&
                !have_border_inactive) {
            if (!leme_config_parse_color(entry->params[0],
                    config->border_inactive)) {
                if (!leme_config_reject(config, entry->lineno,
                        "border_inactive expects #RRGGBB or #RRGGBBAA")) {
                    return false;
                }
                continue;
            }
            have_border_inactive = true;
        } else if (strcmp(entry->name, "opacity_active") == 0 &&
                !have_opacity_active) {
            if (!leme_config_parse_opacity(entry->params[0], &decimal)) {
                if (!leme_config_reject(config, entry->lineno,
                        "%s expects a decimal from 0.0 through 1.0",
                        entry->name)) {
                    return false;
                }
                continue;
            }
            config->opacity_active = decimal;
            have_opacity_active = true;
        } else if (strcmp(entry->name, "opacity_inactive") == 0 &&
                !have_opacity_inactive) {
            if (!leme_config_parse_opacity(entry->params[0], &decimal)) {
                if (!leme_config_reject(config, entry->lineno,
                        "%s expects a decimal from 0.0 through 1.0",
                        entry->name)) {
                    return false;
                }
                continue;
            }
            config->opacity_inactive = decimal;
            have_opacity_inactive = true;
        } else if (!leme_config_reject(config, entry->lineno,
                "duplicate or unknown style directive %s", entry->name)) {
            return false;
        }
    }
    return true;
}

static bool
leme_config_parse_output_mode(const char *text, int *width, int *height)
{
    char extra;

    return sscanf(text, "%dx%d%c", width, height, &extra) == 2 &&
        *width > 0 && *height > 0;
}

static bool
leme_config_parse_output_decimal(const char *text, double *value)
{
    char *end;
    double parsed;

    errno = 0;
    parsed = strtod(text, &end);
    if (errno != 0 || text[0] == '\0' || *end != '\0' ||
            !isfinite(parsed) || parsed <= 0.0) {
        return false;
    }
    *value = parsed;
    return true;
}

static bool
leme_config_parse_tag_selector(const char *text, uint16_t maximum,
    uint16_t **ids, size_t *count)
{
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
    const struct leme_scfg_directive *directive, const char *path, char **error)
{
    struct leme_tag_rule *rules;
    struct leme_tag_rule *rule;
    uint16_t *ids = NULL;
    size_t id_count = 0;
    size_t index;

    if (directive->params_len != 1) {
        return leme_config_reject(config, directive->lineno,
            "tag requires one selector");
    }
    if (!leme_config_parse_tag_selector(directive->params[0],
            config->max_tags, &ids, &id_count) || id_count == 0) {
        return leme_config_reject(config, directive->lineno,
            "invalid tag selector %s", directive->params[0]);
    }
    rules = realloc(config->tag_rules,
        (config->tag_rule_count + 1) * sizeof(*rules));
    if (rules == NULL) {
        free(ids);
        leme_config_set_error(error, "%s:%d: out of memory",
            path, directive->lineno);
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
            if (!leme_config_reject(config, entry->lineno,
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
            if (!leme_config_parse_output_decimal(entry->params[0],
                    &number) || number < 0.10 || number > 0.90) {
                goto invalid;
            }
            rule->settings.mfact = number;
        } else if (strcmp(entry->name, "split_ratio") == 0) {
            field = LEME_TAG_FIELD_SPLIT_RATIO;
            if (!leme_config_parse_output_decimal(entry->params[0],
                    &number) || number < 0.10 || number > 0.90) {
                goto invalid;
            }
            rule->settings.split_ratio = number;
        } else if (strcmp(entry->name, "nmaster") == 0) {
            field = LEME_TAG_FIELD_NMASTER;
            if (!leme_config_parse_u16(entry->params[0], &small) ||
                    small < 1 || small > 16) {
                goto invalid;
            }
            rule->settings.nmaster = small;
        } else if (strcmp(entry->name, "gap") == 0) {
            field = LEME_TAG_FIELD_GAP;
            if (!leme_config_parse_u16_allow_zero(entry->params[0],
                    &small)) {
                goto invalid;
            }
            rule->settings.gap = (int)small;
            rule->settings.has_gap = true;
        } else if (strcmp(entry->name, "accordion_collapse_width") == 0) {
            field = LEME_TAG_FIELD_COLLAPSE_WIDTH;
            if (!leme_config_parse_u16(entry->params[0], &small) ||
                    small < 10 || small > 400) {
                goto invalid;
            }
            rule->settings.collapse_width = (int)small;
        } else {
            if (!leme_config_reject(config, entry->lineno,
                    "unknown tag property %s", entry->name)) {
                return false;
            }
            continue;
        }
        if ((rule->fields & field) != 0) {
            if (!leme_config_reject(config, entry->lineno,
                    "duplicate tag property %s", entry->name)) {
                return false;
            }
            continue;
        }
        rule->fields |= field;
        continue;

invalid:
        if (!leme_config_reject(config, entry->lineno,
                "invalid tag property %s", entry->name)) {
            return false;
        }
    }
    return true;
}

static bool
leme_config_parse_output_position(const char *first, const char *second,
    int *x, int *y)
{
    long parsed_x;
    long parsed_y;
    char *end;

    errno = 0;
    parsed_x = strtol(first, &end, 10);
    if (errno != 0 || first[0] == '\0' || *end != '\0' ||
            parsed_x < INT_MIN || parsed_x > INT_MAX) {
        return false;
    }
    errno = 0;
    parsed_y = strtol(second, &end, 10);
    if (errno != 0 || second[0] == '\0' || *end != '\0' ||
            parsed_y < INT_MIN || parsed_y > INT_MAX) {
        return false;
    }
    *x = (int)parsed_x;
    *y = (int)parsed_y;
    return true;
}

static bool
leme_config_output_relation(const char *name,
    enum leme_output_relation *relation)
{
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

static bool
leme_config_parse_publication(struct leme_config *config,
    const struct leme_scfg_directive *directive, const char *path, char **error)
{
    bool have_activation = false;
    size_t index;

    if (directive->params_len != 0) {
        leme_config_set_error(error,
            "%s:%d: publication block takes no parameters",
            path, directive->lineno);
        return false;
    }
    for (index = 0; index < directive->children.directives_len; index++) {
        const struct leme_scfg_directive *entry =
            &directive->children.directives[index];

        if (entry->params_len != 1 || entry->children.directives_len != 0) {
            if (!leme_config_reject(config, entry->lineno,
                    "publication property requires one value")) {
                return false;
            }
            continue;
        }
        if (strcmp(entry->name, "activation") != 0) {
            if (!leme_config_reject(config, entry->lineno,
                    "unknown publication property %s", entry->name)) {
                return false;
            }
            continue;
        }
        if (have_activation) {
            if (!leme_config_reject(config, entry->lineno,
                    "duplicate publication property activation")) {
                return false;
            }
            continue;
        }
        if (strcmp(entry->params[0], "follow") == 0) {
            config->publication.activation = LEME_ACTIVATION_FOLLOW;
        } else if (strcmp(entry->params[0], "urgent") == 0) {
            config->publication.activation = LEME_ACTIVATION_URGENT;
        } else if (strcmp(entry->params[0], "ignore") == 0) {
            config->publication.activation = LEME_ACTIVATION_IGNORE;
        } else {
            if (!leme_config_reject(config, entry->lineno,
                    "activation must be follow, urgent, or ignore")) {
                return false;
            }
            continue;
        }
        have_activation = true;
    }
    return true;
}

static bool
leme_config_parse_output_policy(struct leme_config *config,
    const struct leme_scfg_directive *directive, const char *path, char **error)
{
    enum {
        POLICY_CROSS_FOCUS = 1 << 0,
        POLICY_CROSS_MOVE = 1 << 1,
        POLICY_CROSS_DRAG = 1 << 2,
        POLICY_WARP_CURSOR = 1 << 3,
    };
    uint32_t seen = 0;
    size_t index;

    (void)path;
    (void)error;
    for (index = 0; index < directive->children.directives_len; index++) {
        const struct leme_scfg_directive *entry =
            &directive->children.directives[index];
        uint32_t field;
        bool value;

        if (entry->params_len != 1 || entry->children.directives_len != 0) {
            if (!leme_config_reject(config, entry->lineno,
                    "output policy property requires one value")) {
                return false;
            }
            continue;
        }
        if (strcmp(entry->params[0], "true") == 0) {
            value = true;
        } else if (strcmp(entry->params[0], "false") == 0) {
            value = false;
        } else {
            if (!leme_config_reject(config, entry->lineno,
                    "output policy boolean must be true or false")) {
                return false;
            }
            continue;
        }
        if (strcmp(entry->name, "cross_output_focus") == 0) {
            field = POLICY_CROSS_FOCUS;
            config->output_policy.cross_output_focus = value;
        } else if (strcmp(entry->name, "cross_output_move") == 0) {
            field = POLICY_CROSS_MOVE;
            config->output_policy.cross_output_move = value;
        } else if (strcmp(entry->name, "cross_output_drag") == 0) {
            field = POLICY_CROSS_DRAG;
            config->output_policy.cross_output_drag = value;
        } else if (strcmp(entry->name, "warp_cursor") == 0) {
            field = POLICY_WARP_CURSOR;
            config->output_policy.warp_cursor = value;
        } else {
            if (!leme_config_reject(config, entry->lineno,
                    "unknown output policy property %s", entry->name)) {
                return false;
            }
            continue;
        }
        if ((seen & field) != 0) {
            if (!leme_config_reject(config, entry->lineno,
                    "duplicate output policy property %s", entry->name)) {
                return false;
            }
            continue;
        }
        seen |= field;
    }
    return true;
}

static struct leme_output_config *
leme_config_append_output(struct leme_config *config, const char *name)
{
    struct leme_output_config *outputs = realloc(config->outputs,
        (config->output_count + 1) * sizeof(*outputs));
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

static bool
leme_config_parse_output(struct leme_config *config,
    const struct leme_scfg_directive *directive, const char *path, char **error,
    bool *have_policy)
{
    enum {
        OUTPUT_MODE = 1 << 0,
        OUTPUT_REFRESH = 1 << 1,
        OUTPUT_SCALE = 1 << 2,
        OUTPUT_TRANSFORM = 1 << 3,
        OUTPUT_POSITION = 1 << 4,
        OUTPUT_RELATION = 1 << 5,
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
            leme_config_set_error(error,
                "%s:%d: duplicate global output block",
                path, directive->lineno);
            return false;
        }
        *have_policy = true;
        return leme_config_parse_output_policy(
            config, directive, path, error);
    }
    if (directive->params[0][0] == '\0') {
        leme_config_set_error(error,
            "%s:%d: output connector name cannot be empty",
            path, directive->lineno);
        return false;
    }
    for (index = 0; index < config->output_count; index++) {
        if (strcmp(config->outputs[index].name, directive->params[0]) == 0) {
            leme_config_set_error(error,
                "%s:%d: duplicate output %s",
                path, directive->lineno, directive->params[0]);
            return false;
        }
    }
    target = leme_config_append_output(config, directive->params[0]);
    if (target == NULL) {
        leme_config_set_error(error, "%s:%d: out of memory",
            path, directive->lineno);
        return false;
    }
    for (index = 0; index < directive->children.directives_len; index++) {
        const struct leme_scfg_directive *entry =
            &directive->children.directives[index];
        enum leme_output_relation relation;
        uint32_t field;

        if (entry->children.directives_len != 0) {
            leme_config_set_error(error,
                "%s:%d: output property takes no block",
                path, entry->lineno);
            return false;
        }
        if (strcmp(entry->name, "position") == 0) {
            field = OUTPUT_POSITION;
            if (entry->params_len != 2 ||
                    !leme_config_parse_output_position(entry->params[0],
                        entry->params[1], &target->x, &target->y)) {
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
                leme_config_set_error(error,
                    "%s:%d: %s requires one connector name",
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
                leme_config_set_error(error, "%s:%d: out of memory",
                    path, entry->lineno);
                return false;
            }
            target->relation = relation;
            seen |= field;
            continue;
        }
        if (entry->params_len != 1) {
            leme_config_set_error(error,
                "%s:%d: output property requires one value",
                path, entry->lineno);
            return false;
        }
        if (strcmp(entry->name, "mode") == 0) {
            field = OUTPUT_MODE;
            if (!leme_config_parse_output_mode(entry->params[0],
                    &target->width, &target->height)) {
                leme_config_set_error(error,
                    "%s:%d: invalid output mode", path, entry->lineno);
                return false;
            }
            target->has_mode = true;
        } else if (strcmp(entry->name, "refresh") == 0) {
            double refresh;

            field = OUTPUT_REFRESH;
            if (!leme_config_parse_output_decimal(
                    entry->params[0], &refresh) ||
                    refresh > (double)INT_MAX / 1000.0) {
                leme_config_set_error(error,
                    "%s:%d: invalid output refresh", path, entry->lineno);
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
                leme_config_set_error(error,
                    "%s:%d: invalid output transform", path, entry->lineno);
                return false;
            }
        } else {
            leme_config_set_error(error,
                "%s:%d: unknown output property %s",
                path, entry->lineno, entry->name);
            return false;
        }
        if ((seen & field) != 0) {
            leme_config_set_error(error,
                "%s:%d: duplicate output property %s",
                path, entry->lineno, entry->name);
            return false;
        }
        seen |= field;
    }
    if (target->has_refresh && !target->has_mode) {
        leme_config_set_error(error,
            "%s:%d: output refresh requires mode",
            path, directive->lineno);
        return false;
    }
    if (target->has_position && target->relation != LEME_OUTPUT_RELATION_NONE) {
        wlr_log(WLR_INFO,
            "leme: output %s sets both position and a relative placement, "
            "using position", target->name);
    }
    return true;
}

static bool
leme_config_parse_boolean(const char *text, bool *value)
{
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

static bool
leme_config_parse_pointer_speed(const char *text, double *value)
{
    char *end;
    double parsed;

    errno = 0;
    parsed = strtod(text, &end);
    if (errno != 0 || text[0] == '\0' || *end != '\0' ||
            !isfinite(parsed) || parsed < -1.0 || parsed > 1.0) {
        return false;
    }
    *value = parsed;
    return true;
}

static bool
leme_config_parse_pointer(struct leme_config *config,
    const struct leme_scfg_directive *directive, const char *path, char **error,
    bool *have_global)
{
    struct leme_pointer_settings local = {0};
    struct leme_pointer_settings *settings;
    const char *name = NULL;
    uint32_t seen = 0;
    size_t index;

    if (directive->params_len > 1) {
        leme_config_set_error(error,
            "%s:%d: pointer accepts at most one device name",
            path, directive->lineno);
        return false;
    }
    if (directive->params_len == 0) {
        if (*have_global) {
            leme_config_set_error(error,
                "%s:%d: duplicate global pointer block",
                path, directive->lineno);
            return false;
        }
        *have_global = true;
        settings = &config->pointer_defaults;
    } else {
        name = directive->params[0];
        if (name[0] == '\0') {
            leme_config_set_error(error,
                "%s:%d: pointer device name cannot be empty",
                path, directive->lineno);
            return false;
        }
        for (index = 0; index < config->pointer_rule_count; index++) {
            if (strcmp(config->pointer_rules[index].name, name) == 0) {
                leme_config_set_error(error,
                    "%s:%d: duplicate pointer device %s",
                    path, directive->lineno, name);
                return false;
            }
        }
        settings = &local;
    }
    for (index = 0; index < directive->children.directives_len; index++) {
        const struct leme_scfg_directive *entry =
            &directive->children.directives[index];
        uint32_t field;

        if (entry->params_len != 1 ||
                entry->children.directives_len != 0) {
            if (!leme_config_reject(config, entry->lineno,
                    "pointer property requires one value")) {
                return false;
            }
            continue;
        }
        if (strcmp(entry->name, "accel_profile") == 0) {
            field = LEME_POINTER_PROFILE;
            if (strcmp(entry->params[0], "adaptive") == 0) {
                settings->profile = LEME_POINTER_ACCEL_ADAPTIVE;
            } else if (strcmp(entry->params[0], "flat") == 0) {
                settings->profile = LEME_POINTER_ACCEL_FLAT;
            } else {
                if (!leme_config_reject(config, entry->lineno,
                        "invalid pointer acceleration profile")) {
                    return false;
                }
                continue;
            }
        } else if (strcmp(entry->name, "accel_speed") == 0) {
            field = LEME_POINTER_SPEED;
            if (!leme_config_parse_pointer_speed(
                    entry->params[0], &settings->speed)) {
                if (!leme_config_reject(config, entry->lineno,
                        "pointer speed must be between -1.0 and 1.0")) {
                    return false;
                }
                continue;
            }
        } else if (strcmp(entry->name, "natural_scroll") == 0) {
            field = LEME_POINTER_NATURAL_SCROLL;
            if (!leme_config_parse_boolean(
                    entry->params[0], &settings->natural_scroll)) {
                goto invalid_boolean;
            }
        } else if (strcmp(entry->name, "left_handed") == 0) {
            field = LEME_POINTER_LEFT_HANDED;
            if (!leme_config_parse_boolean(
                    entry->params[0], &settings->left_handed)) {
                goto invalid_boolean;
            }
        } else if (strcmp(entry->name, "tap") == 0) {
            field = LEME_POINTER_TAP;
            if (!leme_config_parse_boolean(entry->params[0], &settings->tap)) {
                goto invalid_boolean;
            }
        } else {
            if (!leme_config_reject(config, entry->lineno,
                    "unknown pointer property %s", entry->name)) {
                return false;
            }
            continue;
        }
        if ((seen & field) != 0) {
            if (!leme_config_reject(config, entry->lineno,
                    "duplicate pointer property %s", entry->name)) {
                return false;
            }
            continue;
        }
        seen |= field;
        settings->fields |= field;
        continue;

invalid_boolean:
        if (!leme_config_reject(config, entry->lineno,
                "pointer boolean must be true or false")) {
            return false;
        }
    }
    if (name != NULL) {
        struct leme_pointer_rule *rules = realloc(config->pointer_rules,
            (config->pointer_rule_count + 1) * sizeof(*rules));
        struct leme_pointer_rule *rule;

        if (rules == NULL) {
            leme_config_set_error(error, "%s:%d: out of memory",
                path, directive->lineno);
            return false;
        }
        config->pointer_rules = rules;
        rule = &config->pointer_rules[config->pointer_rule_count];
        *rule = (struct leme_pointer_rule){
            .name = strdup(name),
            .settings = local,
        };
        config->pointer_rule_count++;
        if (rule->name == NULL) {
            leme_config_set_error(error, "%s:%d: out of memory",
                path, directive->lineno);
            return false;
        }
    }
    return true;
}

static bool
leme_config_parse_config_errors(struct leme_config *config,
    const struct leme_scfg_directive *directive, const char *path, char **error)
{
    size_t index;

    if (directive->params_len != 0) {
        leme_config_set_error(error,
            "%s:%d: config_errors takes no arguments",
            path, directive->lineno);
        return false;
    }
    for (index = 0; index < directive->children.directives_len; index++) {
        const struct leme_scfg_directive *entry =
            &directive->children.directives[index];
        bool flag;
        int value;

        if (entry->params_len != 1 || entry->children.directives_len != 0) {
            if (!leme_config_reject(config, entry->lineno,
                    "%s requires one value", entry->name)) {
                return false;
            }
            continue;
        }
        if (strcmp(entry->name, "show") == 0) {
            if (!leme_config_parse_boolean(entry->params[0], &flag)) {
                if (!leme_config_reject(config, entry->lineno,
                        "show expects true or false")) {
                    return false;
                }
                continue;
            }
            config->config_errors.show = flag;
        } else if (strcmp(entry->name, "position") == 0) {
            if (strcmp(entry->params[0], "top") == 0) {
                config->config_errors.position = LEME_BANNER_TOP;
            } else if (strcmp(entry->params[0], "bottom") == 0) {
                config->config_errors.position = LEME_BANNER_BOTTOM;
            } else if (!leme_config_reject(config, entry->lineno,
                    "position expects top or bottom")) {
                return false;
            }
        } else if (strcmp(entry->name, "timeout") == 0) {
            if (!leme_config_parse_nonnegative(entry->params[0], &value)) {
                if (!leme_config_reject(config, entry->lineno,
                        "timeout expects a nonnegative integer")) {
                    return false;
                }
                continue;
            }
            config->config_errors.timeout = value;
        } else if (!leme_config_reject(config, entry->lineno,
                "unknown config_errors directive %s", entry->name)) {
            return false;
        }
    }
    return true;
}

static bool
leme_config_parse_keyboard(struct leme_config *config,
    const struct leme_scfg_directive *directive, const char *path, char **error)
{
    size_t index;

    if (directive->params_len != 0) {
        leme_config_set_error(error, "%s:%d: keyboard takes no arguments",
            path, directive->lineno);
        return false;
    }
    if (directive->children.directives_len == 0 ||
            directive->children.directives_len > 4) {
        leme_config_set_error(error,
            "%s:%d: keyboard requires between one and four layouts",
            path, directive->lineno);
        return false;
    }
    for (index = 0; index < directive->children.directives_len; index++) {
        const struct leme_scfg_directive *entry =
            &directive->children.directives[index];
        struct leme_keyboard_layout *layouts;
        struct leme_keyboard_layout *layout;
        const char *name;
        const char *variant;
        size_t previous;
        bool duplicate = false;

        if (strcmp(entry->name, "layout") != 0 ||
                entry->params_len < 1 || entry->params_len > 2 ||
                entry->children.directives_len != 0) {
            if (!leme_config_reject(config, entry->lineno,
                    "layout requires a name and optional variant")) {
                return false;
            }
            continue;
        }
        name = entry->params[0];
        variant = entry->params_len == 2 ? entry->params[1] : NULL;
        if (name[0] == '\0' || strchr(name, ',') != NULL ||
                (variant != NULL &&
                    (variant[0] == '\0' || strchr(variant, ',') != NULL))) {
            if (!leme_config_reject(config, entry->lineno,
                    "invalid keyboard layout or variant")) {
                return false;
            }
            continue;
        }
        for (previous = 0; previous < config->keyboard_layout_count;
                previous++) {
            const struct leme_keyboard_layout *existing =
                &config->keyboard_layouts[previous];
            bool variants_match = existing->variant == NULL ?
                variant == NULL : variant != NULL &&
                strcmp(existing->variant, variant) == 0;

            if (strcmp(existing->name, name) == 0 && variants_match) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            if (!leme_config_reject(config, entry->lineno,
                    "duplicate keyboard layout %s", name)) {
                return false;
            }
            continue;
        }
        layouts = realloc(config->keyboard_layouts,
            (config->keyboard_layout_count + 1) * sizeof(*layouts));
        if (layouts == NULL) {
            leme_config_set_error(error, "%s:%d: out of memory",
                path, entry->lineno);
            return false;
        }
        config->keyboard_layouts = layouts;
        layout = &config->keyboard_layouts[config->keyboard_layout_count];
        *layout = (struct leme_keyboard_layout){
            .name = strdup(name),
            .variant = variant == NULL ? NULL : strdup(variant),
        };
        config->keyboard_layout_count++;
        if (layout->name == NULL ||
                (variant != NULL && layout->variant == NULL)) {
            leme_config_set_error(error, "%s:%d: out of memory",
                path, entry->lineno);
            return false;
        }
    }
    return true;
}

static bool
leme_config_parse_key(const char *text, uint32_t *modifiers,
    xkb_keysym_t *keysym)
{
    char *copy = strdup(text);
    char *key;
    char *prefix;
    char *save = NULL;
    char *token;
    char *separator;
    bool valid = true;

    if (copy == NULL) {
        return false;
    }
    *modifiers = 0;
    separator = strrchr(copy, '+');
    if (separator == NULL) {
        key = copy;
        prefix = NULL;
    } else {
        *separator = '\0';
        key = separator + 1;
        prefix = copy;
    }
    if (key[0] == '\0' || (prefix != NULL &&
            (prefix[0] == '\0' || prefix[strlen(prefix) - 1] == '+' ||
            strstr(prefix, "++") != NULL))) {
        valid = false;
    }
    for (token = prefix == NULL ? NULL : strtok_r(prefix, "+", &save);
            valid && token != NULL; token = strtok_r(NULL, "+", &save)) {
        uint32_t modifier;

        if (strcasecmp(token, "SUPER") == 0) {
            modifier = WLR_MODIFIER_LOGO;
        } else if (strcasecmp(token, "ALT") == 0) {
            modifier = WLR_MODIFIER_ALT;
        } else if (strcasecmp(token, "CTRL") == 0 ||
                strcasecmp(token, "CONTROL") == 0) {
            modifier = WLR_MODIFIER_CTRL;
        } else if (strcasecmp(token, "SHIFT") == 0) {
            modifier = WLR_MODIFIER_SHIFT;
        } else {
            valid = false;
            break;
        }
        if ((*modifiers & modifier) != 0) {
            valid = false;
            break;
        }
        *modifiers |= modifier;
    }
    *keysym = valid ? xkb_keysym_from_name(
        key, XKB_KEYSYM_CASE_INSENSITIVE) : XKB_KEY_NoSymbol;
    valid = valid && *keysym != XKB_KEY_NoSymbol;
    free(copy);
    return valid;
}

static bool
leme_config_parse_command(struct leme_command *command,
    const struct leme_scfg_directive *entry, const char *path, char **error)
{
    char *detail = NULL;

    if (entry->params_len == 0) {
        leme_config_set_error(error, "%s:%d: binding requires a command",
            path, entry->lineno);
        return false;
    }
    if (!leme_command_parse(command, entry->params, entry->params_len,
            &detail)) {
        leme_config_set_error(error, "%s:%d: %s", path, entry->lineno,
            detail == NULL ? "invalid command" : detail);
        free(detail);
        return false;
    }
    return true;
}

static bool
leme_variable_name_char(char value, bool first)
{
    if (value == '_' || (value >= 'A' && value <= 'Z') ||
            (value >= 'a' && value <= 'z')) {
        return true;
    }
    return !first && value >= '0' && value <= '9';
}

static const char *
leme_variables_lookup(const struct leme_variables *variables,
    const char *name, size_t length)
{
    size_t index;

    for (index = 0; index < variables->count; index++) {
        if (strlen(variables->entries[index].name) == length &&
                strncmp(variables->entries[index].name, name, length) == 0) {
            return variables->entries[index].value;
        }
    }
    return NULL;
}

static bool
leme_buffer_append(char **buffer, size_t *length, const char *text,
    size_t count)
{
    char *grown = realloc(*buffer, *length + count + 1);

    if (grown == NULL) {
        return false;
    }
    memcpy(grown + *length, text, count);
    *buffer = grown;
    *length += count;
    (*buffer)[*length] = '\0';
    return true;
}

/*
 * A posição aponta para o início do campo, não para o '$' exacto. O índice
 * corre sobre o texto já descodificado, e as aspas e os escapes desalinham-no
 * da origem, por isso apontar mais fino daria colunas erradas.
 */
static char *
leme_config_expand(const char *input,
    const struct leme_variables *variables, const char *path,
    const struct leme_scfg_source *source, struct leme_scfg_span span,
    char **error)
{
    char *output = calloc(1, 1);
    size_t length = 0;
    size_t index = 0;
    int lineno = 0;
    int column = 0;

    leme_scfg_source_locate(source, span, &lineno, &column);
    if (output == NULL) {
        leme_config_set_error(error, "%s:%d:%d: out of memory", path, lineno,
            column);
        return NULL;
    }
    while (input[index] != '\0') {
        const char *name;
        const char *value;
        size_t name_length = 0;
        bool explicit_form;

        if (input[index] != '$') {
            if (!leme_buffer_append(&output, &length, &input[index], 1)) {
                goto out_of_memory;
            }
            index++;
            continue;
        }
        index++;
        if (input[index] == '$') {
            if (!leme_buffer_append(&output, &length, "$", 1)) {
                goto out_of_memory;
            }
            index++;
            continue;
        }
        explicit_form = input[index] == '(';
        if (explicit_form) {
            index++;
        }
        name = &input[index];
        while (leme_variable_name_char(input[index + name_length],
                name_length == 0)) {
            name_length++;
        }
        if (name_length == 0) {
            leme_config_set_error(error,
                "%s:%d:%d: expected a variable name after $", path, lineno,
                column);
            free(output);
            return NULL;
        }
        index += name_length;
        if (explicit_form) {
            if (input[index] != ')') {
                leme_config_set_error(error,
                    "%s:%d:%d: unterminated $( in %s", path, lineno, column,
                    input);
                free(output);
                return NULL;
            }
            index++;
        }
        value = leme_variables_lookup(variables, name, name_length);
        if (value == NULL) {
            leme_config_set_error(error,
                "%s:%d:%d: unknown variable %.*s",
                path, lineno, column, (int)name_length, name);
            free(output);
            return NULL;
        }
        if (!leme_buffer_append(&output, &length, value, strlen(value))) {
            goto out_of_memory;
        }
    }
    return output;

out_of_memory:
    leme_config_set_error(error, "%s:%d:%d: out of memory", path, lineno,
        column);
    free(output);
    return NULL;
}

static bool
leme_config_expand_field(char **field,
    const struct leme_variables *variables, const char *path,
    const struct leme_scfg_source *source, struct leme_scfg_span span,
    char **error)
{
    char *expanded;

    if (strchr(*field, '$') == NULL) {
        return true;
    }
    expanded = leme_config_expand(*field, variables, path, source, span,
        error);
    if (expanded == NULL) {
        return false;
    }
    free(*field);
    *field = expanded;
    return true;
}

static bool
leme_config_collect_variables(struct leme_variables *variables,
    const struct leme_scfg_directive *directive, const char *path,
    const struct leme_scfg_source *source, char **error)
{
    size_t index;

    for (index = 0; index < directive->children.directives_len; index++) {
        const struct leme_scfg_directive *entry =
            &directive->children.directives[index];
        struct leme_variable *entries;
        char *value;
        size_t previous;

        if (entry->children.directives_len != 0) {
            leme_config_set_error(error,
                "%s:%d: a variable cannot contain a block",
                path, entry->lineno);
            return false;
        }
        if (entry->params_len != 1) {
            leme_config_set_error(error,
                "%s:%d: variable %s takes exactly one value",
                path, entry->lineno, entry->name);
            return false;
        }
        for (previous = 0; previous < variables->count; previous++) {
            if (strcmp(variables->entries[previous].name, entry->name) == 0) {
                leme_config_set_error(error,
                    "%s:%d: duplicate variable %s",
                    path, entry->lineno, entry->name);
                return false;
            }
        }
        value = leme_config_expand(entry->params[0], variables, path,
            source, entry->param_spans[0], error);
        if (value == NULL) {
            return false;
        }
        entries = realloc(variables->entries,
            (variables->count + 1) * sizeof(*entries));
        if (entries == NULL) {
            leme_config_set_error(error, "%s:%d: out of memory",
                path, entry->lineno);
            free(value);
            return false;
        }
        variables->entries = entries;
        variables->entries[variables->count].name = strdup(entry->name);
        variables->entries[variables->count].value = value;
        if (variables->entries[variables->count].name == NULL) {
            leme_config_set_error(error, "%s:%d: out of memory",
                path, entry->lineno);
            free(value);
            return false;
        }
        variables->count++;
    }
    return true;
}

static bool
leme_config_expand_block(struct leme_scfg_block *block,
    const struct leme_variables *variables, const char *path,
    const struct leme_scfg_source *source, char **error)
{
    size_t index;

    for (index = 0; index < block->directives_len; index++) {
        struct leme_scfg_directive *directive = &block->directives[index];
        size_t param;

        if (strcmp(directive->name, "vars") == 0) {
            continue;
        }
        if (!leme_config_expand_field(&directive->name, variables, path,
                source, directive->name_span, error)) {
            return false;
        }
        for (param = 0; param < directive->params_len; param++) {
            if (!leme_config_expand_field(&directive->params[param],
                    variables, path, source, directive->param_spans[param],
                    error)) {
                return false;
            }
        }
        if (!leme_config_expand_block(&directive->children, variables, path,
                source, error)) {
            return false;
        }
    }
    return true;
}

static void
leme_variables_finish(struct leme_variables *variables)
{
    size_t index;

    for (index = 0; index < variables->count; index++) {
        free(variables->entries[index].name);
        free(variables->entries[index].value);
    }
    free(variables->entries);
    *variables = (struct leme_variables){0};
}

static bool
leme_config_apply_variables(struct leme_scfg_block *block, const char *path,
    const struct leme_scfg_source *source, char **error)
{
    struct leme_variables variables = {0};
    bool seen = false;
    size_t index;
    bool valid = true;

    for (index = 0; index < block->directives_len; index++) {
        if (strcmp(block->directives[index].name, "vars") != 0) {
            continue;
        }
        if (seen) {
            leme_config_set_error(error, "%s:%d: duplicate block vars",
                path, block->directives[index].lineno);
            leme_variables_finish(&variables);
            return false;
        }
        seen = true;
        if (!leme_config_collect_variables(&variables,
                &block->directives[index], path, source, error)) {
            leme_variables_finish(&variables);
            return false;
        }
    }
    valid = leme_config_expand_block(block, &variables, path, source,
        error);
    leme_variables_finish(&variables);
    return valid;
}

static struct leme_bind_group *
leme_bind_scope_find(struct leme_bind_scope *scope, const char *name)
{
    size_t index;

    for (index = 0; index < scope->count; index++) {
        if (strcmp(scope->groups[index].name, name) == 0) {
            return &scope->groups[index];
        }
    }
    return NULL;
}

static bool
leme_bind_group_add_inherit(struct leme_bind_group *group, const char *name,
    const char *path, int lineno, char **error)
{
    char **names = realloc(group->inherits,
        (group->inherit_count + 1) * sizeof(*names));

    if (names == NULL) {
        leme_config_set_error(error, "%s:%d: out of memory", path, lineno);
        return false;
    }
    group->inherits = names;
    group->inherits[group->inherit_count] = strdup(name);
    if (group->inherits[group->inherit_count] == NULL) {
        leme_config_set_error(error, "%s:%d: out of memory", path, lineno);
        return false;
    }
    group->inherit_count++;
    return true;
}

static bool
leme_config_parse_bind_block(struct leme_config *config,
    struct leme_bind_scope *scope,
    const struct leme_scfg_directive *directive, bool is_mode,
    const char *path, char **error)
{
    struct leme_bind_group *groups;
    struct leme_bind_group *group;
    size_t index;
    bool seen_escape_exits = false;

    if (directive->params_len != 1) {
        leme_config_set_error(error, "%s:%d: %s requires one name",
            path, directive->lineno, directive->name);
        return false;
    }
    if (leme_bind_scope_find(scope, directive->params[0]) != NULL) {
        leme_config_set_error(error,
            "%s:%d: duplicate binding block name %s",
            path, directive->lineno, directive->params[0]);
        return false;
    }
    groups = realloc(scope->groups, (scope->count + 1) * sizeof(*groups));
    if (groups == NULL) {
        leme_config_set_error(error, "%s:%d: out of memory",
            path, directive->lineno);
        return false;
    }
    scope->groups = groups;
    group = &scope->groups[scope->count];
    *group = (struct leme_bind_group){0};
    group->is_mode = is_mode;
    group->escape_exits = true;
    group->lineno = directive->lineno;
    group->name = strdup(directive->params[0]);
    if (group->name == NULL) {
        leme_config_set_error(error, "%s:%d: out of memory",
            path, directive->lineno);
        return false;
    }
    scope->count++;

    for (index = 0; index < directive->children.directives_len; index++) {
        const struct leme_scfg_directive *entry =
            &directive->children.directives[index];
        struct leme_binding *bindings;
        struct leme_binding *binding;
        size_t previous;
        bool duplicate = false;

        if (entry->children.directives_len != 0) {
            if (!leme_config_reject(config, entry->lineno,
                    "binding cannot contain a block")) {
                return false;
            }
            continue;
        }
        if (strcmp(entry->name, "inherit") == 0) {
            if (entry->params_len != 1) {
                if (!leme_config_reject(config, entry->lineno,
                        "inherit requires one bind group name")) {
                    return false;
                }
                continue;
            }
            if (!leme_bind_group_add_inherit(group, entry->params[0],
                    path, entry->lineno, error)) {
                return false;
            }
            continue;
        }
        if (strcmp(entry->name, "escape_exits") == 0) {
            if (!is_mode) {
                if (!leme_config_reject(config, entry->lineno,
                        "escape_exits belongs to a binds block, "
                        "not a bind_group")) {
                    return false;
                }
                continue;
            }
            if (seen_escape_exits || entry->params_len != 1) {
                if (!leme_config_reject(config, entry->lineno,
                        "escape_exits takes one value and may appear once")) {
                    return false;
                }
                continue;
            }
            if (strcmp(entry->params[0], "true") == 0) {
                group->escape_exits = true;
            } else if (strcmp(entry->params[0], "false") == 0) {
                group->escape_exits = false;
            } else {
                if (!leme_config_reject(config, entry->lineno,
                        "escape_exits must be true or false")) {
                    return false;
                }
                continue;
            }
            seen_escape_exits = true;
            continue;
        }
        bindings = realloc(group->bindings,
            (group->binding_count + 1) * sizeof(*bindings));
        if (bindings == NULL) {
            leme_config_set_error(error, "%s:%d: out of memory",
                path, entry->lineno);
            return false;
        }
        group->bindings = bindings;
        binding = &group->bindings[group->binding_count];
        *binding = (struct leme_binding){0};
        binding->lineno = entry->lineno;
        if (!leme_config_parse_key(entry->name,
                &binding->modifiers, &binding->keysym)) {
            if (!leme_config_reject(config, entry->lineno,
                    "invalid key specification %s", entry->name)) {
                return false;
            }
            continue;
        }
        for (previous = 0; previous < group->binding_count; previous++) {
            if (group->bindings[previous].modifiers == binding->modifiers &&
                    group->bindings[previous].keysym == binding->keysym) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            if (!leme_config_reject(config, entry->lineno,
                    "duplicate binding %s", entry->name)) {
                return false;
            }
            continue;
        }
        if (!leme_config_parse_command(&binding->command,
                entry, path, error)) {
            free(*error);
            *error = NULL;
            if (!leme_config_reject(config, entry->lineno,
                    "invalid command for %s", entry->name)) {
                return false;
            }
            continue;
        }
        group->binding_count++;
    }
    return true;
}

static bool
leme_bind_group_append(struct leme_bind_group *group,
    const struct leme_binding *binding, const char *origin,
    const char *path, char **error)
{
    const struct leme_binding **resolved;
    const char **origins;
    size_t index;

    for (index = 0; index < group->resolved_count; index++) {
        if (group->resolved[index]->modifiers != binding->modifiers ||
                group->resolved[index]->keysym != binding->keysym) {
            continue;
        }
        if (group->origin[index] == origin) {
            return true;
        }
        if (origin != group->name) {
            leme_config_set_error(error,
                "%s:%d: groups %s and %s both bind the same key in %s",
                path, group->lineno, group->origin[index], origin,
                group->name);
            return false;
        }
        group->resolved[index] = binding;
        group->origin[index] = origin;
        return true;
    }
    resolved = realloc(group->resolved,
        (group->resolved_count + 1) * sizeof(*resolved));
    if (resolved == NULL) {
        leme_config_set_error(error, "%s:%d: out of memory",
            path, group->lineno);
        return false;
    }
    group->resolved = resolved;
    origins = realloc(group->origin,
        (group->resolved_count + 1) * sizeof(*origins));
    if (origins == NULL) {
        leme_config_set_error(error, "%s:%d: out of memory",
            path, group->lineno);
        return false;
    }
    group->origin = origins;
    group->resolved[group->resolved_count] = binding;
    group->origin[group->resolved_count] = origin;
    group->resolved_count++;
    return true;
}

static bool
leme_bind_group_resolve(struct leme_bind_scope *scope,
    struct leme_bind_group *group, const char *path, char **error)
{
    size_t index;
    size_t entry;

    if (group->state == LEME_BIND_RESOLVED) {
        return true;
    }
    if (group->state == LEME_BIND_RESOLVING) {
        leme_config_set_error(error,
            "%s:%d: bind group %s inherits itself",
            path, group->lineno, group->name);
        return false;
    }
    group->state = LEME_BIND_RESOLVING;
    for (index = 0; index < group->inherit_count; index++) {
        struct leme_bind_group *parent =
            leme_bind_scope_find(scope, group->inherits[index]);

        if (parent == NULL) {
            leme_config_set_error(error,
                "%s:%d: %s inherits unknown bind group %s",
                path, group->lineno, group->name, group->inherits[index]);
            return false;
        }
        if (parent->is_mode) {
            leme_config_set_error(error,
                "%s:%d: %s is a binding mode, not a bind group",
                path, group->lineno, parent->name);
            return false;
        }
        if (parent->state == LEME_BIND_RESOLVING) {
            leme_config_set_error(error, parent == group ?
                "%s:%d: bind group %s inherits itself" :
                "%s:%d: bind groups %s and %s form an inheritance cycle",
                path, group->lineno, group->name, parent->name);
            return false;
        }
        if (!leme_bind_group_resolve(scope, parent, path, error)) {
            return false;
        }
        for (entry = 0; entry < parent->resolved_count; entry++) {
            if (!leme_bind_group_append(group, parent->resolved[entry],
                    parent->origin[entry], path, error)) {
                return false;
            }
        }
    }
    for (index = 0; index < group->binding_count; index++) {
        if (!leme_bind_group_append(group, &group->bindings[index],
                group->name, path, error)) {
            return false;
        }
    }
    group->state = LEME_BIND_RESOLVED;
    return true;
}

static bool
leme_config_materialize_mode(struct leme_config *config,
    const struct leme_bind_group *group, const char *path, char **error)
{
    struct leme_mode *modes;
    struct leme_mode *mode;
    size_t index;
    bool has_exit = false;

    modes = realloc(config->modes, (config->mode_count + 1) * sizeof(*modes));
    if (modes == NULL) {
        leme_config_set_error(error, "%s:%d: out of memory",
            path, group->lineno);
        return false;
    }
    config->modes = modes;
    mode = &config->modes[config->mode_count];
    *mode = (struct leme_mode){0};
    mode->escape_exits = group->escape_exits;
    mode->name = strdup(group->name);
    if (mode->name == NULL) {
        leme_config_set_error(error, "%s:%d: out of memory",
            path, group->lineno);
        return false;
    }
    config->mode_count++;
    if (group->resolved_count == 0) {
        return true;
    }
    mode->bindings = calloc(group->resolved_count, sizeof(*mode->bindings));
    if (mode->bindings == NULL) {
        leme_config_set_error(error, "%s:%d: out of memory",
            path, group->lineno);
        return false;
    }
    for (index = 0; index < group->resolved_count; index++) {
        const struct leme_binding *source = group->resolved[index];

        mode->bindings[index].modifiers = source->modifiers;
        mode->bindings[index].keysym = source->keysym;
        mode->bindings[index].lineno = source->lineno;
        if (!leme_config_copy_command(&mode->bindings[index].command,
                &source->command)) {
            leme_config_set_error(error, "%s:%d: out of memory",
                path, group->lineno);
            return false;
        }
        mode->binding_count++;
        if (source->command.type == LEME_COMMAND_SET_MODE) {
            has_exit = true;
        }
    }
    if (!mode->escape_exits && !has_exit) {
        leme_config_set_error(error,
            "%s:%d: mode %s keeps Escape but binds no mode command",
            path, group->lineno, mode->name);
        return false;
    }
    return true;
}

static bool
leme_config_resolve_binds(struct leme_config *config,
    struct leme_bind_scope *scope, const char *path, char **error)
{
    size_t index;

    for (index = 0; index < scope->count; index++) {
        if (!scope->groups[index].is_mode) {
            continue;
        }
        if (!leme_bind_group_resolve(scope, &scope->groups[index],
                path, error)) {
            return false;
        }
        if (!leme_config_materialize_mode(config, &scope->groups[index],
                path, error)) {
            return false;
        }
    }
    return true;
}

static void
leme_bind_scope_finish(struct leme_bind_scope *scope)
{
    size_t index;

    for (index = 0; index < scope->count; index++) {
        struct leme_bind_group *group = &scope->groups[index];
        size_t entry;

        for (entry = 0; entry < group->binding_count; entry++) {
            leme_config_finish_command(&group->bindings[entry].command);
        }
        for (entry = 0; entry < group->inherit_count; entry++) {
            free(group->inherits[entry]);
        }
        free(group->bindings);
        free(group->inherits);
        free(group->resolved);
        free(group->origin);
        free(group->name);
    }
    free(scope->groups);
    *scope = (struct leme_bind_scope){0};
}

static bool
leme_config_parse_env(struct leme_config *config,
    const struct leme_scfg_directive *directive, const char *path, char **error)
{
    size_t index;

    if (directive->params_len != 0) {
        leme_config_set_error(error, "%s:%d: env takes no arguments",
            path, directive->lineno);
        return false;
    }
    for (index = 0; index < directive->children.directives_len; index++) {
        const struct leme_scfg_directive *entry =
            &directive->children.directives[index];
        struct leme_environment *environment;
        size_t previous;
        bool duplicate = false;

        if (entry->params_len != 1 ||
                entry->children.directives_len != 0 ||
                entry->name[0] == '\0' || strchr(entry->name, '=') != NULL) {
            if (!leme_config_reject(config, entry->lineno,
                    "environment entry requires a valid name and one value")) {
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
            if (!leme_config_reject(config, entry->lineno,
                    "duplicate environment variable %s", entry->name)) {
                return false;
            }
            continue;
        }
        environment = realloc(config->environment,
            (config->environment_count + 1) * sizeof(*environment));
        if (environment == NULL) {
            leme_config_set_error(error, "%s:%d: out of memory",
                path, entry->lineno);
            return false;
        }
        config->environment = environment;
        environment = &config->environment[config->environment_count];
        *environment = (struct leme_environment){
            .name = strdup(entry->name),
            .value = strdup(entry->params[0]),
        };
        if (environment->name == NULL || environment->value == NULL) {
            leme_config_set_error(error, "%s:%d: out of memory",
                path, entry->lineno);
            return false;
        }
        config->environment_count++;
    }
    return true;
}

static bool
leme_config_parse_exec(struct leme_config *config,
    const struct leme_scfg_directive *directive, const char *path, char **error)
{
    size_t index;

    if (directive->params_len != 0) {
        leme_config_set_error(error, "%s:%d: exec takes no arguments",
            path, directive->lineno);
        return false;
    }
    for (index = 0; index < directive->children.directives_len; index++) {
        const struct leme_scfg_directive *entry =
            &directive->children.directives[index];
        struct leme_startup *startup;

        if (entry->children.directives_len != 0) {
            if (!leme_config_reject(config, entry->lineno,
                    "exec entry cannot contain a block")) {
                return false;
            }
            continue;
        }
        startup = realloc(config->startup,
            (config->startup_count + 1) * sizeof(*startup));
        if (startup == NULL) {
            leme_config_set_error(error, "%s:%d: out of memory",
                path, entry->lineno);
            return false;
        }
        config->startup = startup;
        startup = &config->startup[config->startup_count];
        startup->argv = leme_config_copy_argv(entry->name,
            entry->params, entry->params_len);
        if (startup->argv == NULL) {
            leme_config_set_error(error, "%s:%d: out of memory",
                path, entry->lineno);
            return false;
        }
        config->startup_count++;
    }
    return true;
}

bool
leme_config_drop_invalid_binds(struct leme_config *config)
{
    size_t mode_index;

    for (mode_index = 0; mode_index < config->mode_count; mode_index++) {
        struct leme_mode *mode = &config->modes[mode_index];
        size_t index = 0;

        while (index < mode->binding_count) {
            struct leme_binding *binding = &mode->bindings[index];

            if (leme_config_validate_command(config, &binding->command)) {
                index++;
                continue;
            }
            if (!leme_config_reject(config, binding->lineno,
                    "binding in mode %s refers to something undefined",
                    mode->name)) {
                return false;
            }
            leme_config_finish_command(&binding->command);
            memmove(binding, binding + 1,
                (mode->binding_count - index - 1) * sizeof(*binding));
            mode->binding_count--;
        }
    }
    return true;
}

/*
 * Junta todos os erros de sintaxe numa só mensagem. Recuperar serve para
 * relatar tudo de uma vez; a configuração continua recusada.
 */
static void
leme_config_report_syntax(const struct leme_scfg_source *source,
    const char *path, const struct leme_scfg_result *parsed, char **error)
{
    char *joined = NULL;
    size_t length = 0;
    size_t index;

    for (index = 0; index < parsed->error_count; index++) {
        char *rendered = leme_scfg_render_error(source, path,
            &parsed->errors[index]);
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
        leme_config_set_error(error,
            "%s:1: unable to parse the configuration", path);
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

struct leme_config *
leme_config_load(const char *path, char **error)
{
    struct leme_config *config = calloc(1, sizeof(*config));
    struct leme_scfg_source source = {0};
    struct leme_scfg_result parsed = {0};
    struct leme_scfg_block block = {0};
    struct leme_bind_scope scope = {0};
    bool have_tags = false;
    bool have_style = false;
    bool have_animation = false;
    bool have_keyboard = false;
    bool have_output_policy = false;
    bool have_publication = false;
    bool have_config_errors = false;
    bool have_global_pointer = false;
    bool have_env = false;
    bool have_exec = false;
    size_t index;

    if (config == NULL) {
        leme_config_set_error(error, "%s:1: out of memory", path);
        return NULL;
    }
    config->path = strdup(path);
    if (config->path == NULL) {
        leme_config_set_error(error, "%s:1: out of memory", path);
        leme_config_destroy(config);
        return NULL;
    }
    config->initial_tags = 3;
    config->max_tags = 9;
    config->config_errors = (struct leme_config_errors){
        .show = true,
        .position = LEME_BANNER_TOP,
        .timeout = 0,
    };
    config->drop_mode = LEME_DROP_MODE_SIMPLE;
    config->publication.activation = LEME_ACTIVATION_FOLLOW;
    leme_config_set_style_defaults(config);
    leme_config_set_output_defaults(config);
    leme_config_set_pointer_defaults(config);
    if (!leme_scfg_source_load(&source, path)) {
        leme_config_set_error(error, "%s:1: %s", path,
            errno == ENOENT ? "configuration file not found" :
            "unable to read the configuration file");
        leme_config_destroy(config);
        return NULL;
    }
    if (!leme_scfg_parse(&source, &parsed)) {
        leme_config_report_syntax(&source, path, &parsed, error);
        leme_scfg_result_finish(&parsed);
        leme_scfg_source_finish(&source);
        leme_config_destroy(config);
        return NULL;
    }
    /*
     * O bloco é uma vista sobre o resultado: a expansão troca cadeias no
     * sítio, mas nunca realoca o vector, por isso libertar o resultado
     * chega.
     */
    block = parsed.block;
    if (!leme_config_apply_variables(&block, path, &source, error)) {
        leme_scfg_result_finish(&parsed);
        leme_scfg_source_finish(&source);
        leme_config_destroy(config);
        return NULL;
    }
    for (index = 0; index < block.directives_len; index++) {
        const struct leme_scfg_directive *directive = &block.directives[index];
        bool valid;

        if (strcmp(directive->name, "vars") == 0) {
            continue;
        }
        if (strcmp(directive->name, "tags") == 0 && !have_tags) {
            have_tags = true;
            valid = leme_config_parse_tags(config, directive, path, error);
        } else if (strcmp(directive->name, "animation") == 0 &&
                !have_animation) {
            have_animation = true;
            valid = leme_config_parse_animation(
                config, directive, path, error);
        } else if (strcmp(directive->name, "style") == 0 && !have_style) {
            have_style = true;
            valid = leme_config_parse_style(config, directive, path, error);
        } else if (strcmp(directive->name, "keyboard") == 0 &&
                !have_keyboard) {
            have_keyboard = true;
            valid = leme_config_parse_keyboard(
                config, directive, path, error);
        } else if (strcmp(directive->name, "output") == 0) {
            valid = leme_config_parse_output(config, directive,
                path, error, &have_output_policy);
        } else if (strcmp(directive->name, "publication") == 0 &&
                !have_publication) {
            have_publication = true;
            valid = leme_config_parse_publication(
                config, directive, path, error);
        } else if (strcmp(directive->name, "pointer") == 0) {
            valid = leme_config_parse_pointer(config, directive,
                path, error, &have_global_pointer);
        } else if (strcmp(directive->name, "window") == 0) {
            valid = leme_config_parse_window_rule(
                config, directive, path, error);
        } else if (strcmp(directive->name, "scratchpad") == 0) {
            valid = leme_config_parse_scratchpad(
                config, directive, path, error);
        } else if (strcmp(directive->name, "binds") == 0) {
            valid = leme_config_parse_bind_block(config, &scope,
                directive, true, path, error);
        } else if (strcmp(directive->name, "bind_group") == 0) {
            valid = leme_config_parse_bind_block(config, &scope,
                directive, false, path, error);
        } else if (strcmp(directive->name, "config_errors") == 0 &&
                !have_config_errors) {
            have_config_errors = true;
            valid = leme_config_parse_config_errors(
                config, directive, path, error);
        } else if (strcmp(directive->name, "env") == 0 && !have_env) {
            have_env = true;
            valid = leme_config_parse_env(config, directive, path, error);
        } else if (strcmp(directive->name, "exec") == 0 && !have_exec) {
            have_exec = true;
            valid = leme_config_parse_exec(config, directive, path, error);
        } else {
            leme_config_set_error(error,
                "%s:%d: duplicate or unknown block %s",
                path, directive->lineno, directive->name);
            valid = false;
        }
        if (!valid) {
            leme_bind_scope_finish(&scope);
            leme_scfg_result_finish(&parsed);
            leme_scfg_source_finish(&source);
            leme_config_destroy(config);
            return NULL;
        }
    }
    if (!leme_config_resolve_binds(config, &scope, path, error)) {
        leme_bind_scope_finish(&scope);
        leme_scfg_result_finish(&parsed);
        leme_scfg_source_finish(&source);
        leme_config_destroy(config);
        return NULL;
    }
    leme_bind_scope_finish(&scope);
    leme_scfg_result_finish(&parsed);
    leme_scfg_source_finish(&source);
    if (!leme_config_drop_invalid_binds(config)) {
        leme_config_set_error(error, "%s:1: out of memory", path);
        leme_config_destroy(config);
        return NULL;
    }
    if (config->keyboard_layout_count == 0 &&
            !leme_config_set_default_keyboard(config)) {
        leme_config_set_error(error, "%s:1: out of memory", path);
        leme_config_destroy(config);
        return NULL;
    }
    return config;
}
