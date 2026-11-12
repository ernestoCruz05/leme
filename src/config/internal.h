#ifndef LEME_CONFIG_INTERNAL_H
#define LEME_CONFIG_INTERNAL_H

#include "config/config.h"

#include <stdbool.h>
#include <stddef.h>

struct leme_binding;
struct leme_command;
struct leme_scfg_directive;
struct leme_option_block;

struct leme_reject_extra {
  const char *label;
  const char *help;
  const struct leme_scfg_directive *secondary;
  const char *secondary_label;
};

extern const char *const leme_choice_activation[3];
extern const char *const leme_choice_banner[2];
extern const char *const leme_choice_accel_profile[2];
extern const char *const leme_choice_drop_mode[2];
extern const char *const leme_choice_fullscreen_covers[3];
extern const char *const leme_choice_gesture_mode[3];
extern const char *const leme_choice_layout[3];
extern const char *const leme_choice_secondary_gpu[2];
extern const char *const leme_choice_workspace_style[2];
extern const char *const leme_pointer_click_names[3];
extern const char *const leme_pointer_scroll_names[4];
extern const char *const leme_pointer_button_names[8];
extern const char *const leme_pointer_tap_map_names[2];
extern const char *const leme_pointer_drag_lock_names[3];

extern const struct leme_option_block leme_style_block;
extern const struct leme_option_block leme_cursor_block;
extern const struct leme_option_block leme_workspace_switch_block;
extern const struct leme_option_block leme_edges_block;
extern const struct leme_option_block leme_pointer_block;
extern const struct leme_option_block leme_output_policy_block;
extern const struct leme_option_block leme_publication_block;
extern const struct leme_option_block leme_config_errors_block;
extern const struct leme_option_block leme_desktop_block;

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
bool leme_config_bindings_overlap(const struct leme_binding *first,
                                  const struct leme_binding *second);
void leme_config_set_error(char **error, const char *format, ...);
void leme_config_finish_command(struct leme_command *command);
bool leme_config_check_command(const struct leme_command *command,
                               char **error);
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
