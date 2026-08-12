#include "shell/rules.h"

#include "config/config.h"

#include <fnmatch.h>
#include <stddef.h>
#include <string.h>

static bool
leme_window_rule_matches(const struct leme_window_rule *rule,
    const char *identity, const char *title)
{
    if (strcmp(rule->identity, "*") != 0 &&
            (identity == NULL || strcmp(rule->identity, identity) != 0)) {
        return false;
    }
    if (rule->title != NULL &&
            (title == NULL || fnmatch(rule->title, title, 0) != 0)) {
        return false;
    }
    return true;
}

struct leme_view_rules
leme_view_rules_match(const struct leme_config *config, const char *identity,
    const char *title)
{
    struct leme_view_rules resolved = {0};
    size_t index;

    if (config == NULL) {
        return resolved;
    }
    for (index = 0; index < config->window_rule_count; index++) {
        const struct leme_window_rule *rule = &config->window_rules[index];

        if (!leme_window_rule_matches(rule, identity, title)) {
            continue;
        }
        if ((rule->fields & LEME_WINDOW_RULE_TAG) != 0) {
            resolved.tag_id = rule->tag_id;
        }
        if ((rule->fields & LEME_WINDOW_RULE_FLOATING) != 0) {
            resolved.floating = rule->floating;
        }
        if ((rule->fields & LEME_WINDOW_RULE_FULLSCREEN) != 0) {
            resolved.fullscreen = rule->fullscreen;
        }
        if ((rule->fields & LEME_WINDOW_RULE_OUTPUT) != 0) {
            resolved.output = rule->output;
        }
        if ((rule->fields & LEME_WINDOW_RULE_OPACITY) != 0) {
            resolved.opacity = rule->opacity;
        }
        resolved.fields |= rule->fields;
    }
    return resolved;
}
