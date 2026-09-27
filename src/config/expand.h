#ifndef LEME_CONFIG_EXPAND_H
#define LEME_CONFIG_EXPAND_H

#include "config/diagnostics.h"
#include "config/envfile.h"
#include "config/scfg.h"

#include <stdint.h>

#define LEME_CONFIG_EXPAND_MAX_DEPTH 16U
#define LEME_CONFIG_EXPAND_MAX_ITERATIONS 1024U
#define LEME_CONFIG_EXPAND_MAX_TOTAL_ITERATIONS 65536U
#define LEME_CONFIG_EXPAND_MAX_DIRECTIVES 8192U
#define LEME_CONFIG_EXPAND_MAX_FIELD_BYTES 65536U
#define LEME_CONFIG_EXPAND_MAX_TOTAL_BYTES 4194304U

enum leme_trail_kind {
  LEME_TRAIL_FOR,
  LEME_TRAIL_IF,
};

struct leme_trail_node {
  uint16_t parent;
  enum leme_trail_kind kind;
  char *variable;
  char *value;
};

struct leme_trail_table {
  struct leme_trail_node *nodes;
  size_t count;
  size_t capacity;
};

const struct leme_trail_node *
leme_trail_table_get(const struct leme_trail_table *table, uint16_t index);
void leme_trail_table_finish(struct leme_trail_table *table);

bool leme_config_expand_templates(
    const struct leme_scfg_block *input, struct leme_scfg_block *output,
    struct leme_diagnostics *diagnostics, const struct leme_scfg_source *source,
    const char *path, struct leme_trail_table *trails,
    const struct leme_env_file *env_file, char **error);

#endif
