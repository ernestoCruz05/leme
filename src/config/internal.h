#ifndef LEME_CONFIG_INTERNAL_H
#define LEME_CONFIG_INTERNAL_H

#include "config/config.h"

#include <stdbool.h>
#include <stddef.h>

struct leme_command;
struct leme_scfg_directive;

struct leme_reject_extra {
  const char *label;
  const char *help;
  const struct leme_scfg_directive *secondary;
  const char *secondary_label;
};

const char *leme_config_nearest_key(const char *written,
                                    const char *const *keys, size_t count);
bool leme_config_reject_detailed(struct leme_config *config,
                                 const struct leme_scfg_directive *directive,
                                 int param,
                                 const struct leme_reject_extra *extra,
                                 const char *format, ...);
bool leme_config_reject(struct leme_config *config,
                        const struct leme_scfg_directive *directive, int param,
                        const char *format, ...);
bool leme_config_parse_color(const char *text, float value[4]);
bool leme_config_parse_nonnegative(const char *text, int *value);
bool leme_config_parse_decimal(const char *text, double *value);
bool leme_config_parse_boolean(const char *text, bool *value);
bool leme_config_parse_style_rule(struct leme_config *config,
                                  const struct leme_scfg_directive *directive,
                                  const char *path, char **error);
bool leme_config_desktop_menu_defaults(struct leme_config *config);
void leme_config_desktop_menu_finish(struct leme_config *config);
bool leme_config_parse_desktop_menu(struct leme_config *config,
                                    const struct leme_scfg_directive *directive,
                                    const struct leme_scfg_directive **seen);
bool leme_config_drop_invalid_desktop_menu(struct leme_config *config);
bool leme_config_drop_invalid_binds(struct leme_config *config);
void leme_config_set_error(char **error, const char *format, ...);
void leme_config_finish_command(struct leme_command *command);
bool leme_config_copy_command(struct leme_command *destination,
                              const struct leme_command *source);
char **leme_config_copy_argv(const char *name, char *const *params,
                             size_t count);
void leme_config_free_argv(char **argv);
void leme_config_set_output_defaults(struct leme_config *config);
void leme_config_set_style_defaults(struct leme_config *config);
void leme_config_set_pointer_defaults(struct leme_config *config);
bool leme_config_set_default_keyboard(struct leme_config *config);

#endif
