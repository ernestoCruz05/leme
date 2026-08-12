#ifndef LEME_SHELL_RULES_H
#define LEME_SHELL_RULES_H

#include <stdbool.h>
#include <stdint.h>

struct leme_config;

struct leme_view_rules {
    uint32_t fields;
    uint16_t tag_id;
    bool floating;
    bool fullscreen;
    const char *output;
    double opacity;
};

struct leme_view_rules leme_view_rules_match(const struct leme_config *config,
    const char *identity, const char *title);

#endif
