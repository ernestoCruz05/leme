#ifndef LEME_CONFIG_PARSE_INTERNAL_H
#define LEME_CONFIG_PARSE_INTERNAL_H

#include "config/config.h"
#include "config/option.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct leme_scfg_directive;

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

struct leme_config_parse_context {
  const char *path;
  char **error;
  void *state;
};

bool leme_config_parse_options(struct leme_config *config,
                               const struct leme_scfg_directive *directive,
                               const struct leme_option_block *block,
                               void *target, uint32_t *fields, void *state,
                               const char *path, char **error);
bool leme_config_parse_tags(struct leme_config *config,
                            const struct leme_scfg_directive *directive,
                            const char *path, char **error);
bool leme_config_parse_window_rule(struct leme_config *config,
                                   const struct leme_scfg_directive *directive,
                                   const char *path, char **error);
bool leme_config_parse_scratchpad(struct leme_config *config,
                                  const struct leme_scfg_directive *directive,
                                  const char *path, char **error);
bool leme_config_parse_output(struct leme_config *config,
                              const struct leme_scfg_directive *directive,
                              const char *path, char **error,
                              bool *have_policy);
bool leme_config_parse_animation(struct leme_config *config,
                                 const struct leme_scfg_directive *directive,
                                 const char *path, char **error);
bool leme_config_parse_gestures(struct leme_config *config,
                                const struct leme_scfg_directive *directive,
                                const char *path, char **error);
bool leme_config_parse_pointer(struct leme_config *config,
                               const struct leme_scfg_directive *directive,
                               const char *path, char **error,
                               bool *have_global);
bool leme_config_parse_keyboard(struct leme_config *config,
                                const struct leme_scfg_directive *directive,
                                const char *path, char **error);
bool leme_config_parse_bind_block(struct leme_config *config,
                                  struct leme_bind_scope *scope,
                                  const struct leme_scfg_directive *directive,
                                  bool is_mode, const char *path, char **error);
bool leme_config_resolve_binds(struct leme_config *config,
                               struct leme_bind_scope *scope, const char *path,
                               char **error);
void leme_bind_scope_finish(struct leme_bind_scope *scope);
bool leme_config_drop_invalid_binds(struct leme_config *config);

#endif
