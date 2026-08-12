#ifndef LEME_CONFIG_INTERNAL_H
#define LEME_CONFIG_INTERNAL_H

#include "config/config.h"

#include <stdbool.h>
#include <stddef.h>

struct leme_command;

bool leme_config_validate_command(const struct leme_config *config,
    const struct leme_command *command);
bool leme_config_drop_invalid_binds(struct leme_config *config);
void leme_config_set_error(char **error, const char *format, ...);
void leme_config_finish_command(struct leme_command *command);
bool leme_config_copy_command(struct leme_command *destination,
    const struct leme_command *source);
char **leme_config_copy_argv(const char *name,
    char *const *params, size_t count);
void leme_config_free_argv(char **argv);
void leme_config_set_output_defaults(struct leme_config *config);
void leme_config_set_style_defaults(struct leme_config *config);
void leme_config_set_pointer_defaults(struct leme_config *config);
bool leme_config_set_default_keyboard(struct leme_config *config);

#endif
