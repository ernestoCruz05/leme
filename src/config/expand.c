#include "config/expand.h"
#include "config/internal.h"
#include "config/scfg.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LEME_CONFIG_EXPAND_MAX_DEPTH 16U
#define LEME_CONFIG_EXPAND_MAX_ITERATIONS 1024U
#define LEME_CONFIG_EXPAND_MAX_TOTAL_ITERATIONS 65536U
#define LEME_CONFIG_EXPAND_MAX_DIRECTIVES 8192U

struct leme_expand_binding {
  char *name;
  char *value;
};

struct leme_expand_env_reference {
  size_t field_offset;
  size_t decoded_offset;
  size_t name_length;
};

struct leme_expand_list {
  char *name;
  char **items;
  size_t item_count;
};

struct leme_expander {
  const struct leme_scfg_source *source;
  const char *path;
  char **error;
  struct leme_diagnostics *diagnostics;
  struct leme_expand_binding *bindings;
  size_t binding_count;
  size_t scalar_count;
  size_t output_count;
  size_t total_iterations;
  size_t expanded_bytes;
  struct leme_expand_env_reference *env_refs;
  size_t env_ref_count;
  struct leme_expand_list *lists;
  size_t list_count;
  uint16_t active_trail;
  struct leme_trail_table *trails;
  const struct leme_env_file *env_file;
  const struct leme_scfg_block *pending_vars;
  size_t pending_index;
};

static bool leme_trail_push(struct leme_trail_table *table, uint16_t parent,
                            enum leme_trail_kind kind, const char *variable,
                            const char *value, uint16_t *index) {
  struct leme_trail_node *grown;
  char *variable_copy;
  char *value_copy;

  if (table == NULL) {
    if (index != NULL) {
      *index = 0;
    }
    return true;
  }
  if (table->count == 0) {
    table->capacity = 16;
    grown = calloc(table->capacity, sizeof(*grown));
    if (grown == NULL) {
      return false;
    }
    table->nodes = grown;
    table->count = 1;
  }
  if (table->count >= UINT16_MAX) {
    if (index != NULL) {
      *index = (uint16_t)(table->count - 1);
    }
    return true;
  }
  if (table->count >= table->capacity) {
    size_t new_capacity = table->capacity * 2;
    if (new_capacity > UINT16_MAX) {
      new_capacity = UINT16_MAX;
    }
    grown = realloc(table->nodes, new_capacity * sizeof(*grown));
    if (grown == NULL) {
      return false;
    }
    table->nodes = grown;
    table->capacity = new_capacity;
  }
  variable_copy = strdup(variable == NULL ? "" : variable);
  if (variable_copy == NULL) {
    return false;
  }
  value_copy = strdup(value == NULL ? "" : value);
  if (value_copy == NULL) {
    free(variable_copy);
    return false;
  }
  table->nodes[table->count] = (struct leme_trail_node){
      .parent = parent,
      .kind = kind,
      .variable = variable_copy,
      .value = value_copy,
  };
  *index = (uint16_t)table->count;
  table->count++;
  return true;
}

const struct leme_trail_node *
leme_trail_table_get(const struct leme_trail_table *table, uint16_t index) {
  if (index == 0 || (size_t)index >= table->count) {
    return NULL;
  }
  return &table->nodes[index];
}

void leme_trail_table_finish(struct leme_trail_table *table) {
  size_t index;

  if (table == NULL) {
    return;
  }
  for (index = 0; index < table->count; index++) {
    free(table->nodes[index].variable);
    free(table->nodes[index].value);
  }
  free(table->nodes);
  table->nodes = NULL;
  table->count = 0;
  table->capacity = 0;
}

static bool leme_expand_identifier_char(char value, bool first) {
  if (value == '_' || (value >= 'A' && value <= 'Z') ||
      (value >= 'a' && value <= 'z')) {
    return true;
  }
  return !first && value >= '0' && value <= '9';
}

static bool leme_expand_is_valid_identifier(const char *name) {
  size_t index;

  if (name == NULL || name[0] == '\0') {
    return false;
  }
  if (!leme_expand_identifier_char(name[0], true)) {
    return false;
  }
  for (index = 1; name[index] != '\0'; index++) {
    if (!leme_expand_identifier_char(name[index], false)) {
      return false;
    }
  }
  return true;
}

static const char *leme_expand_lookup(const struct leme_expander *expander,
                                      const char *name, size_t length) {
  size_t index = expander->binding_count;

  while (index > 0) {
    index--;
    if (strlen(expander->bindings[index].name) == length &&
        strncmp(expander->bindings[index].name, name, length) == 0) {
      return expander->bindings[index].value;
    }
  }
  return NULL;
}

static bool leme_expand_declared_later(const struct leme_expander *expander,
                                       const char *name, size_t length) {
  const struct leme_scfg_block *block = expander->pending_vars;

  if (block == NULL) {
    return false;
  }
  for (size_t index = expander->pending_index; index < block->directives_len;
       index++) {
    const char *declared = block->directives[index].name;

    if (strlen(declared) == length && memcmp(declared, name, length) == 0) {
      return true;
    }
  }
  return false;
}

static bool leme_buffer_append(char **buffer, size_t *length, const char *text,
                               size_t count) {
  if (count > SIZE_MAX - *length - 1) {
    return false;
  }
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

static char *leme_expand_field(struct leme_expander *expander,
                               const char *input, struct leme_scfg_span span) {
  char *output = calloc(1, 1);
  size_t length = 0;
  size_t index = 0;
  int lineno = 0;
  int column = 0;

  leme_scfg_source_locate(expander->source, span, &lineno, &column);
  if (output == NULL) {
    leme_config_set_error(expander->error, "%s:%d:%d: out of memory",
                          expander->path, lineno, column);
    return NULL;
  }
  while (input[index] != '\0') {
    const char *name;
    const char *value;
    size_t name_length = 0;
    bool explicit_form;
    size_t dollar_index;

    if (length > LEME_CONFIG_EXPAND_MAX_FIELD_BYTES) {
      goto too_large;
    }
    if (input[index] != '$') {
      const size_t run = strcspn(&input[index], "$");

      if (!leme_buffer_append(&output, &length, &input[index], run)) {
        goto out_of_memory;
      }
      index += run;
      continue;
    }
    dollar_index = index;
    index++;
    if (input[index] == '$') {
      if (!leme_buffer_append(&output, &length, "$", 1)) {
        goto out_of_memory;
      }
      index++;
      continue;
    }
    if (strncmp(&input[index], "(env.", 5) == 0) {
      char stack_name[128];
      char *heap_name = NULL;
      char *target_name = stack_name;
      const char *env_value;

      index += 5;
      name = &input[index];
      while (leme_expand_identifier_char(input[index + name_length],
                                         name_length == 0)) {
        name_length++;
      }
      if (name_length == 0) {
        leme_config_set_error(
            expander->error,
            "%s:%d:%d: expected an environment variable name after $(env.",
            expander->path, lineno, column);
        free(output);
        return NULL;
      }
      index += name_length;
      if (input[index] != ')') {
        if (strchr(&input[index], ')') != NULL) {
          leme_config_set_error(
              expander->error,
              "%s:%d:%d: expected an environment variable name after $(env.",
              expander->path, lineno, column);
        } else {
          leme_config_set_error(expander->error,
                                "%s:%d:%d: unterminated $( in %s",
                                expander->path, lineno, column, input);
        }
        free(output);
        return NULL;
      }
      index++;

      if (name_length >= sizeof(stack_name)) {
        heap_name = malloc(name_length + 1);
        if (heap_name == NULL) {
          goto out_of_memory;
        }
        target_name = heap_name;
      }
      memcpy(target_name, name, name_length);
      target_name[name_length] = '\0';
      env_value = getenv(target_name);
      if (env_value == NULL || env_value[0] == '\0') {
        env_value = leme_env_file_lookup(expander->env_file, target_name);
      }
      free(heap_name);

      if (env_value != NULL && env_value[0] != '\0') {
        if (!leme_buffer_append(&output, &length, env_value,
                                strlen(env_value))) {
          goto out_of_memory;
        }
      } else {
        bool already_reported = false;
        size_t ref_idx;

        for (ref_idx = 0; ref_idx < expander->env_ref_count; ref_idx++) {
          if (expander->env_refs[ref_idx].field_offset == span.offset &&
              expander->env_refs[ref_idx].decoded_offset == dollar_index &&
              expander->env_refs[ref_idx].name_length == name_length) {
            already_reported = true;
            break;
          }
        }
        if (!already_reported) {
          struct leme_expand_env_reference *grown;

          if (expander->env_ref_count >
              SIZE_MAX / sizeof(struct leme_expand_env_reference) - 1) {
            goto out_of_memory;
          }
          grown = realloc(expander->env_refs,
                          (expander->env_ref_count + 1) * sizeof(*grown));
          if (grown == NULL) {
            goto out_of_memory;
          }
          expander->env_refs = grown;
          expander->env_refs[expander->env_ref_count] =
              (struct leme_expand_env_reference){
                  .field_offset = span.offset,
                  .decoded_offset = dollar_index,
                  .name_length = name_length,
              };
          expander->env_ref_count++;
          struct leme_diagnostic_detail detail = {
              .severity = LEME_DIAGNOSTIC_WARNING,
              .line = lineno,
              .primary =
                  {
                      .source = 0,
                      .span = span,
                  },
              .trail = expander->active_trail,
          };
          if (!leme_diagnostics_add_detailed(
                  expander->diagnostics, &detail,
                  "environment variable %.*s is unset or empty",
                  (int)name_length, name)) {
            goto out_of_memory;
          }
        }
        if (!leme_buffer_append(&output, &length, "none", 4)) {
          goto out_of_memory;
        }
      }
      continue;
    }
    explicit_form = input[index] == '(';
    if (explicit_form) {
      index++;
    }
    name = &input[index];
    while (leme_expand_identifier_char(input[index + name_length],
                                       name_length == 0)) {
      name_length++;
    }
    if (name_length == 0) {
      leme_config_set_error(expander->error,
                            "%s:%d:%d: expected a variable name after $",
                            expander->path, lineno, column);
      free(output);
      return NULL;
    }
    index += name_length;
    if (explicit_form) {
      if (input[index] != ')') {
        leme_config_set_error(expander->error,
                              "%s:%d:%d: unterminated $( in %s", expander->path,
                              lineno, column, input);
        free(output);
        return NULL;
      }
      index++;
    }
    value = leme_expand_lookup(expander, name, name_length);
    if (value == NULL &&
        leme_expand_declared_later(expander, name, name_length)) {
      leme_config_set_error(expander->error,
                            "%s:%d:%d: variable %.*s is used before it is "
                            "declared; variables can only use names declared "
                            "above them",
                            expander->path, lineno, column, (int)name_length,
                            name);
      free(output);
      return NULL;
    }
    if (value == NULL) {
      leme_config_set_error(expander->error, "%s:%d:%d: unknown variable %.*s",
                            expander->path, lineno, column, (int)name_length,
                            name);
      free(output);
      return NULL;
    }
    if (!leme_buffer_append(&output, &length, value, strlen(value))) {
      goto out_of_memory;
    }
  }
  if (length > LEME_CONFIG_EXPAND_MAX_FIELD_BYTES) {
    goto too_large;
  }
  if (length > LEME_CONFIG_EXPAND_MAX_TOTAL_BYTES - expander->expanded_bytes) {
    leme_config_set_error(
        expander->error, "%s:%d:%d: expanded configuration exceeds %u bytes",
        expander->path, lineno, column, LEME_CONFIG_EXPAND_MAX_TOTAL_BYTES);
    free(output);
    return NULL;
  }
  expander->expanded_bytes += length;
  return output;

too_large:
  leme_config_set_error(
      expander->error, "%s:%d:%d: expanded value exceeds %u bytes",
      expander->path, lineno, column, LEME_CONFIG_EXPAND_MAX_FIELD_BYTES);
  free(output);
  return NULL;

out_of_memory:
  leme_config_set_error(expander->error, "%s:%d:%d: out of memory",
                        expander->path, lineno, column);
  free(output);
  return NULL;
}

static bool leme_expand_parse_range(const char *text, uint64_t *low,
                                    uint64_t *high) {
  const char *dotdot = strstr(text, "..");
  const char *right;
  size_t left_len;
  size_t right_len;
  size_t i;
  uint64_t l = 0;
  uint64_t h = 0;

  if (dotdot == NULL) {
    return false;
  }
  if (strstr(dotdot + 2, "..") != NULL) {
    return false;
  }
  left_len = (size_t)(dotdot - text);
  if (left_len == 0) {
    return false;
  }
  for (i = 0; i < left_len; i++) {
    uint64_t digit;

    if (text[i] < '0' || text[i] > '9') {
      return false;
    }
    digit = (uint64_t)(text[i] - '0');
    if (l > (UINT64_MAX - digit) / 10) {
      return false;
    }
    l = l * 10 + digit;
  }

  right = dotdot + 2;
  right_len = strlen(right);
  if (right_len == 0) {
    return false;
  }
  for (i = 0; i < right_len; i++) {
    uint64_t digit;

    if (right[i] < '0' || right[i] > '9') {
      return false;
    }
    digit = (uint64_t)(right[i] - '0');
    if (h > (UINT64_MAX - digit) / 10) {
      return false;
    }
    h = h * 10 + digit;
  }

  if (l > h) {
    return false;
  }

  *low = l;
  *high = h;
  return true;
}

static bool leme_expand_push_local(struct leme_expander *expander,
                                   const char *name, const char *value,
                                   int lineno) {
  struct leme_expand_binding *grown;

  if (expander->binding_count > SIZE_MAX / sizeof(*grown) - 1) {
    leme_config_set_error(expander->error, "%s:%d: out of memory",
                          expander->path, lineno);
    return false;
  }
  grown = realloc(expander->bindings,
                  (expander->binding_count + 1) * sizeof(*grown));
  if (grown == NULL) {
    leme_config_set_error(expander->error, "%s:%d: out of memory",
                          expander->path, lineno);
    return false;
  }
  expander->bindings = grown;
  expander->bindings[expander->binding_count].name = strdup(name);
  expander->bindings[expander->binding_count].value = strdup(value);
  if (expander->bindings[expander->binding_count].name == NULL ||
      expander->bindings[expander->binding_count].value == NULL) {
    free(expander->bindings[expander->binding_count].name);
    free(expander->bindings[expander->binding_count].value);
    leme_config_set_error(expander->error, "%s:%d: out of memory",
                          expander->path, lineno);
    return false;
  }
  expander->binding_count++;
  return true;
}

static void leme_expand_pop_local(struct leme_expander *expander) {
  if (expander->binding_count > 0) {
    expander->binding_count--;
    free(expander->bindings[expander->binding_count].name);
    free(expander->bindings[expander->binding_count].value);
    expander->bindings[expander->binding_count].name = NULL;
    expander->bindings[expander->binding_count].value = NULL;
  }
}

static bool leme_expand_block(struct leme_expander *expander,
                              const struct leme_scfg_block *input,
                              struct leme_scfg_block *output, bool is_top_level,
                              size_t generation_depth);

static const struct leme_expand_list *
leme_expand_find_list(const struct leme_expander *expander, const char *name);

static bool leme_expand_for(struct leme_expander *expander,
                            const struct leme_scfg_directive *directive,
                            struct leme_scfg_block *output,
                            size_t generation_depth) {
  char *expanded_iter = NULL;
  uint64_t low = 0;
  uint64_t high = 0;
  size_t count;
  size_t iteration;

  if (!directive->has_block) {
    leme_config_set_error(expander->error, "%s:%d: for requires braces",
                          expander->path, directive->lineno);
    return false;
  }
  if (directive->params_len != 3 || strcmp(directive->params[1], "in") != 0) {
    leme_config_set_error(expander->error, "%s:%d: malformed for loop header",
                          expander->path, directive->lineno);
    return false;
  }
  if (!leme_expand_is_valid_identifier(directive->params[0])) {
    leme_config_set_error(
        expander->error, "%s:%d: invalid loop variable name %s", expander->path,
        directive->lineno, directive->params[0]);
    return false;
  }

  if (generation_depth >= LEME_CONFIG_EXPAND_MAX_DEPTH) {
    leme_config_set_error(
        expander->error,
        "%s:%d: expanded configuration exceeds 16 active generation levels",
        expander->path, directive->lineno);
    return false;
  }

  expanded_iter = leme_expand_field(expander, directive->params[2],
                                    directive->param_spans[2]);
  if (expanded_iter == NULL) {
    return false;
  }

  if (strstr(expanded_iter, "..") != NULL) {
    if (!leme_expand_parse_range(expanded_iter, &low, &high)) {
      leme_config_set_error(expander->error, "%s:%d: invalid range %s",
                            expander->path, directive->lineno, expanded_iter);
      free(expanded_iter);
      return false;
    }
    if (high - low >= LEME_CONFIG_EXPAND_MAX_ITERATIONS) {
      leme_config_set_error(expander->error,
                            "%s:%d: loop exceeds 1024 iterations",
                            expander->path, directive->lineno);
      free(expanded_iter);
      return false;
    }
    count = (size_t)(high - low) + 1;
    for (iteration = 0; iteration < count; iteration++) {
      uint64_t current = low + (uint64_t)iteration;
      char value[32];
      int written;

      if (expander->total_iterations >=
          LEME_CONFIG_EXPAND_MAX_TOTAL_ITERATIONS) {
        leme_config_set_error(expander->error,
                              "%s:%d: expanded configuration exceeds total "
                              "iteration limit of 65536",
                              expander->path, directive->lineno);
        free(expanded_iter);
        return false;
      }
      expander->total_iterations++;

      written = snprintf(value, sizeof(value), "%" PRIu64, current);
      if (written < 0 || (size_t)written >= sizeof(value)) {
        leme_config_set_error(expander->error, "%s:%d: out of memory",
                              expander->path, directive->lineno);
        free(expanded_iter);
        return false;
      }
      uint16_t saved_trail = expander->active_trail;
      uint16_t node = 0;

      if (!leme_trail_push(expander->trails, saved_trail, LEME_TRAIL_FOR,
                           directive->params[0], value, &node)) {
        leme_config_set_error(expander->error, "%s:%d: out of memory",
                              expander->path, directive->lineno);
        free(expanded_iter);
        return false;
      }
      expander->active_trail = node;
      if (!leme_expand_push_local(expander, directive->params[0], value,
                                  directive->lineno)) {
        expander->active_trail = saved_trail;
        free(expanded_iter);
        return false;
      }
      if (!leme_expand_block(expander, &directive->children, output, false,
                             generation_depth + 1)) {
        leme_expand_pop_local(expander);
        expander->active_trail = saved_trail;
        free(expanded_iter);
        return false;
      }
      leme_expand_pop_local(expander);
      expander->active_trail = saved_trail;
    }
  } else {
    const struct leme_expand_list *list =
        leme_expand_find_list(expander, expanded_iter);

    if (list == NULL) {
      leme_config_set_error(expander->error, "%s:%d: unknown list %s",
                            expander->path, directive->lineno, expanded_iter);
      free(expanded_iter);
      return false;
    }
    if (list->item_count > LEME_CONFIG_EXPAND_MAX_ITERATIONS) {
      leme_config_set_error(expander->error,
                            "%s:%d: loop exceeds 1024 iterations",
                            expander->path, directive->lineno);
      free(expanded_iter);
      return false;
    }
    for (iteration = 0; iteration < list->item_count; iteration++) {
      if (expander->total_iterations >=
          LEME_CONFIG_EXPAND_MAX_TOTAL_ITERATIONS) {
        leme_config_set_error(expander->error,
                              "%s:%d: expanded configuration exceeds total "
                              "iteration limit of 65536",
                              expander->path, directive->lineno);
        free(expanded_iter);
        return false;
      }
      expander->total_iterations++;

      uint16_t saved_trail = expander->active_trail;
      uint16_t node = 0;

      if (!leme_trail_push(expander->trails, saved_trail, LEME_TRAIL_FOR,
                           directive->params[0], list->items[iteration],
                           &node)) {
        leme_config_set_error(expander->error, "%s:%d: out of memory",
                              expander->path, directive->lineno);
        free(expanded_iter);
        return false;
      }
      expander->active_trail = node;
      if (!leme_expand_push_local(expander, directive->params[0],
                                  list->items[iteration], directive->lineno)) {
        expander->active_trail = saved_trail;
        free(expanded_iter);
        return false;
      }
      if (!leme_expand_block(expander, &directive->children, output, false,
                             generation_depth + 1)) {
        leme_expand_pop_local(expander);
        expander->active_trail = saved_trail;
        free(expanded_iter);
        return false;
      }
      leme_expand_pop_local(expander);
      expander->active_trail = saved_trail;
    }
  }

  free(expanded_iter);
  return true;
}

static bool leme_expand_if(struct leme_expander *expander,
                           const struct leme_scfg_block *parent, size_t *index,
                           struct leme_scfg_block *output,
                           size_t generation_depth) {
  const struct leme_scfg_directive *directive = &parent->directives[*index];
  const struct leme_scfg_directive *else_directive = NULL;
  char *left = NULL;
  char *right = NULL;
  bool condition_result;
  bool equal;

  if (!directive->has_block) {
    leme_config_set_error(expander->error, "%s:%d: if requires braces",
                          expander->path, directive->lineno);
    return false;
  }
  if (directive->params_len != 3) {
    leme_config_set_error(expander->error, "%s:%d: malformed if condition",
                          expander->path, directive->lineno);
    return false;
  }
  if (strcmp(directive->params[1], "==") != 0 &&
      strcmp(directive->params[1], "!=") != 0) {
    leme_config_set_error(expander->error, "%s:%d: invalid operator %s",
                          expander->path, directive->lineno,
                          directive->params[1]);
    return false;
  }

  if (*index + 1 < parent->directives_len &&
      strcmp(parent->directives[*index + 1].name, "else") == 0) {
    else_directive = &parent->directives[*index + 1];
    (*index)++;
    if (!else_directive->has_block) {
      leme_config_set_error(expander->error, "%s:%d: else requires braces",
                            expander->path, else_directive->lineno);
      return false;
    }
    if (else_directive->params_len != 0) {
      leme_config_set_error(expander->error, "%s:%d: else takes no parameters",
                            expander->path, else_directive->lineno);
      return false;
    }
  }

  if (generation_depth >= LEME_CONFIG_EXPAND_MAX_DEPTH) {
    leme_config_set_error(
        expander->error,
        "%s:%d: expanded configuration exceeds 16 active generation levels",
        expander->path, directive->lineno);
    return false;
  }

  left = leme_expand_field(expander, directive->params[0],
                           directive->param_spans[0]);
  if (left == NULL) {
    return false;
  }
  right = leme_expand_field(expander, directive->params[2],
                            directive->param_spans[2]);
  if (right == NULL) {
    free(left);
    return false;
  }

  equal = strcmp(left, right) == 0;
  condition_result = strcmp(directive->params[1], "==") == 0 ? equal : !equal;
  free(left);
  free(right);

  if (condition_result) {
    uint16_t saved_trail = expander->active_trail;
    uint16_t node = 0;

    if (!leme_trail_push(expander->trails, saved_trail, LEME_TRAIL_IF,
                         directive->params[0], "true", &node)) {
      leme_config_set_error(expander->error, "%s:%d: out of memory",
                            expander->path, directive->lineno);
      return false;
    }
    expander->active_trail = node;
    if (!leme_expand_block(expander, &directive->children, output, false,
                           generation_depth + 1)) {
      expander->active_trail = saved_trail;
      return false;
    }
    expander->active_trail = saved_trail;
  } else if (else_directive != NULL) {
    uint16_t saved_trail = expander->active_trail;
    uint16_t node = 0;

    if (!leme_trail_push(expander->trails, saved_trail, LEME_TRAIL_IF,
                         directive->params[0], "false", &node)) {
      leme_config_set_error(expander->error, "%s:%d: out of memory",
                            expander->path, else_directive->lineno);
      return false;
    }
    expander->active_trail = node;
    if (!leme_expand_block(expander, &else_directive->children, output, false,
                           generation_depth + 1)) {
      expander->active_trail = saved_trail;
      return false;
    }
    expander->active_trail = saved_trail;
  }

  return true;
}

static bool leme_expand_clone_directive(struct leme_expander *expander,
                                        const struct leme_scfg_directive *input,
                                        struct leme_scfg_block *output,
                                        size_t generation_depth) {
  struct leme_scfg_directive *grown;
  struct leme_scfg_directive *directive;
  size_t param;

  if (expander->output_count >= LEME_CONFIG_EXPAND_MAX_DIRECTIVES) {
    leme_config_set_error(
        expander->error,
        "%s:%d: expanded configuration exceeds 8192 directives", expander->path,
        input->lineno);
    return false;
  }
  expander->output_count++;

  if (output->directives_len >
      SIZE_MAX / sizeof(struct leme_scfg_directive) - 1) {
    leme_config_set_error(expander->error, "%s:%d: out of memory",
                          expander->path, input->lineno);
    return false;
  }
  grown = realloc(output->directives,
                  (output->directives_len + 1) * sizeof(*grown));
  if (grown == NULL) {
    leme_config_set_error(expander->error, "%s:%d: out of memory",
                          expander->path, input->lineno);
    return false;
  }
  output->directives = grown;
  directive = &output->directives[output->directives_len];
  memset(directive, 0, sizeof(*directive));
  output->directives_len++;

  directive->lineno = input->lineno;
  directive->trail = expander->active_trail;
  directive->span = input->span;
  directive->name_span = input->name_span;
  directive->has_block = input->has_block;

  directive->name = leme_expand_field(expander, input->name, input->name_span);
  if (directive->name == NULL) {
    return false;
  }

  if (input->params_len > 0) {
    if (input->params_len > SIZE_MAX / sizeof(char *) ||
        input->params_len > SIZE_MAX / sizeof(struct leme_scfg_span)) {
      leme_config_set_error(expander->error, "%s:%d: out of memory",
                            expander->path, input->lineno);
      return false;
    }
    directive->params = calloc(input->params_len, sizeof(char *));
    directive->param_spans =
        calloc(input->params_len, sizeof(struct leme_scfg_span));
    if (directive->params == NULL || directive->param_spans == NULL) {
      leme_config_set_error(expander->error, "%s:%d: out of memory",
                            expander->path, input->lineno);
      return false;
    }
    for (param = 0; param < input->params_len; param++) {
      directive->param_spans[param] = input->param_spans[param];
      directive->params[param] = leme_expand_field(
          expander, input->params[param], input->param_spans[param]);
      if (directive->params[param] == NULL) {
        return false;
      }
      directive->params_len = param + 1;
    }
  }

  if (!leme_expand_block(expander, &input->children, &directive->children,
                         false, generation_depth)) {
    return false;
  }
  return true;
}

static bool leme_expand_block(struct leme_expander *expander,
                              const struct leme_scfg_block *input,
                              struct leme_scfg_block *output, bool is_top_level,
                              size_t generation_depth) {
  size_t index;

  for (index = 0; index < input->directives_len; index++) {
    const struct leme_scfg_directive *directive = &input->directives[index];

    if (strcmp(directive->name, "vars") == 0) {
      if (is_top_level) {
        continue;
      }
      leme_config_set_error(expander->error,
                            "%s:%d: vars is only allowed at the top level",
                            expander->path, directive->lineno);
      return false;
    }
    if (strcmp(directive->name, "lists") == 0) {
      if (is_top_level) {
        continue;
      }
      leme_config_set_error(expander->error,
                            "%s:%d: lists is only allowed at the top level",
                            expander->path, directive->lineno);
      return false;
    }
    if (strcmp(directive->name, "for") == 0) {
      if (!leme_expand_for(expander, directive, output, generation_depth)) {
        return false;
      }
      continue;
    }
    if (strcmp(directive->name, "if") == 0) {
      if (!leme_expand_if(expander, input, &index, output, generation_depth)) {
        return false;
      }
      continue;
    }
    if (strcmp(directive->name, "else") == 0) {
      leme_config_set_error(expander->error, "%s:%d: else without previous if",
                            expander->path, directive->lineno);
      return false;
    }
    if (!leme_expand_clone_directive(expander, directive, output,
                                     generation_depth)) {
      return false;
    }
  }
  return true;
}

static bool leme_expand_collect_vars(struct leme_expander *expander,
                                     const struct leme_scfg_block *top_level) {
  bool seen = false;
  size_t top_index;

  for (top_index = 0; top_index < top_level->directives_len; top_index++) {
    const struct leme_scfg_directive *block = &top_level->directives[top_index];
    size_t entry_index;

    if (strcmp(block->name, "vars") != 0) {
      continue;
    }
    if (seen) {
      leme_config_set_error(expander->error, "%s:%d: duplicate block vars",
                            expander->path, block->lineno);
      return false;
    }
    seen = true;
    if (!block->has_block) {
      leme_config_set_error(expander->error,
                            "%s:%d: block vars requires braces", expander->path,
                            block->lineno);
      return false;
    }
    if (block->params_len != 0) {
      leme_config_set_error(expander->error,
                            "%s:%d: block vars takes no parameters",
                            expander->path, block->lineno);
      return false;
    }
    for (entry_index = 0; entry_index < block->children.directives_len;
         entry_index++) {
      const struct leme_scfg_directive *entry =
          &block->children.directives[entry_index];
      struct leme_expand_binding *grown;
      char *value;
      size_t previous;

      if (entry->has_block || entry->children.directives_len != 0) {
        leme_config_set_error(expander->error,
                              "%s:%d: a variable cannot contain a block",
                              expander->path, entry->lineno);
        return false;
      }
      if (entry->params_len != 1) {
        leme_config_set_error(expander->error,
                              "%s:%d: variable %s takes exactly one value",
                              expander->path, entry->lineno, entry->name);
        return false;
      }
      if (!leme_expand_is_valid_identifier(entry->name)) {
        leme_config_set_error(expander->error,
                              "%s:%d: invalid variable name %s", expander->path,
                              entry->lineno, entry->name);
        return false;
      }
      for (previous = 0; previous < expander->scalar_count; previous++) {
        if (strcmp(expander->bindings[previous].name, entry->name) == 0) {
          leme_config_set_error(expander->error, "%s:%d: duplicate variable %s",
                                expander->path, entry->lineno, entry->name);
          return false;
        }
      }
      expander->pending_vars = &block->children;
      expander->pending_index = entry_index;
      value =
          leme_expand_field(expander, entry->params[0], entry->param_spans[0]);
      expander->pending_vars = NULL;
      if (value == NULL) {
        return false;
      }
      if (expander->binding_count > SIZE_MAX / sizeof(*grown) - 1) {
        leme_config_set_error(expander->error, "%s:%d: out of memory",
                              expander->path, entry->lineno);
        free(value);
        return false;
      }
      grown = realloc(expander->bindings,
                      (expander->binding_count + 1) * sizeof(*grown));
      if (grown == NULL) {
        leme_config_set_error(expander->error, "%s:%d: out of memory",
                              expander->path, entry->lineno);
        free(value);
        return false;
      }
      expander->bindings = grown;
      expander->bindings[expander->binding_count].name = strdup(entry->name);
      expander->bindings[expander->binding_count].value = value;
      if (expander->bindings[expander->binding_count].name == NULL) {
        leme_config_set_error(expander->error, "%s:%d: out of memory",
                              expander->path, entry->lineno);
        free(value);
        return false;
      }
      expander->scalar_count++;
      expander->binding_count++;
    }
  }
  return true;
}

static bool leme_expand_collect_lists(struct leme_expander *expander,
                                      const struct leme_scfg_block *top_level) {
  bool seen = false;
  size_t top_index;

  for (top_index = 0; top_index < top_level->directives_len; top_index++) {
    const struct leme_scfg_directive *block = &top_level->directives[top_index];
    size_t entry_index;

    if (strcmp(block->name, "lists") != 0) {
      continue;
    }
    if (seen) {
      leme_config_set_error(expander->error, "%s:%d: duplicate block lists",
                            expander->path, block->lineno);
      return false;
    }
    seen = true;
    if (!block->has_block) {
      leme_config_set_error(expander->error,
                            "%s:%d: block lists requires braces",
                            expander->path, block->lineno);
      return false;
    }
    if (block->params_len != 0) {
      leme_config_set_error(expander->error,
                            "%s:%d: block lists takes no parameters",
                            expander->path, block->lineno);
      return false;
    }
    for (entry_index = 0; entry_index < block->children.directives_len;
         entry_index++) {
      const struct leme_scfg_directive *entry =
          &block->children.directives[entry_index];
      struct leme_expand_list *grown;
      struct leme_expand_list *current;
      size_t previous;
      size_t item_idx;

      if (entry->has_block || entry->children.directives_len != 0) {
        leme_config_set_error(expander->error,
                              "%s:%d: a list cannot contain a block",
                              expander->path, entry->lineno);
        return false;
      }
      if (entry->params_len == 0) {
        leme_config_set_error(expander->error,
                              "%s:%d: list %s requires at least one item",
                              expander->path, entry->lineno, entry->name);
        return false;
      }
      if (!leme_expand_is_valid_identifier(entry->name)) {
        leme_config_set_error(expander->error, "%s:%d: invalid list name %s",
                              expander->path, entry->lineno, entry->name);
        return false;
      }
      for (previous = 0; previous < expander->list_count; previous++) {
        if (strcmp(expander->lists[previous].name, entry->name) == 0) {
          leme_config_set_error(expander->error, "%s:%d: duplicate list %s",
                                expander->path, entry->lineno, entry->name);
          return false;
        }
      }
      if (expander->list_count > SIZE_MAX / sizeof(*grown) - 1) {
        leme_config_set_error(expander->error, "%s:%d: out of memory",
                              expander->path, entry->lineno);
        return false;
      }
      grown =
          realloc(expander->lists, (expander->list_count + 1) * sizeof(*grown));
      if (grown == NULL) {
        leme_config_set_error(expander->error, "%s:%d: out of memory",
                              expander->path, entry->lineno);
        return false;
      }
      expander->lists = grown;
      current = &expander->lists[expander->list_count];
      memset(current, 0, sizeof(*current));
      current->name = strdup(entry->name);
      if (current->name == NULL) {
        leme_config_set_error(expander->error, "%s:%d: out of memory",
                              expander->path, entry->lineno);
        return false;
      }
      if (entry->params_len > SIZE_MAX / sizeof(char *)) {
        free(current->name);
        leme_config_set_error(expander->error, "%s:%d: out of memory",
                              expander->path, entry->lineno);
        return false;
      }
      current->items = calloc(entry->params_len, sizeof(char *));
      if (current->items == NULL) {
        free(current->name);
        leme_config_set_error(expander->error, "%s:%d: out of memory",
                              expander->path, entry->lineno);
        return false;
      }
      for (item_idx = 0; item_idx < entry->params_len; item_idx++) {
        current->items[item_idx] = leme_expand_field(
            expander, entry->params[item_idx], entry->param_spans[item_idx]);
        if (current->items[item_idx] == NULL) {
          for (size_t k = 0; k < item_idx; k++) {
            free(current->items[k]);
          }
          free(current->items);
          free(current->name);
          return false;
        }
        current->item_count++;
      }
      expander->list_count++;
    }
  }
  return true;
}

static const struct leme_expand_list *
leme_expand_find_list(const struct leme_expander *expander, const char *name) {
  size_t index;

  for (index = 0; index < expander->list_count; index++) {
    if (strcmp(expander->lists[index].name, name) == 0) {
      return &expander->lists[index];
    }
  }
  return NULL;
}

static void leme_expand_lists_finish(struct leme_expander *expander) {
  size_t l, i;

  for (l = 0; l < expander->list_count; l++) {
    free(expander->lists[l].name);
    for (i = 0; i < expander->lists[l].item_count; i++) {
      free(expander->lists[l].items[i]);
    }
    free(expander->lists[l].items);
  }
  free(expander->lists);
  expander->lists = NULL;
  expander->list_count = 0;
}

static void leme_expander_finish(struct leme_expander *expander) {
  size_t index;

  for (index = 0; index < expander->binding_count; index++) {
    free(expander->bindings[index].name);
    free(expander->bindings[index].value);
  }
  free(expander->bindings);
  expander->bindings = NULL;
  expander->binding_count = 0;
  expander->scalar_count = 0;
  free(expander->env_refs);
  expander->env_refs = NULL;
  expander->env_ref_count = 0;
  leme_expand_lists_finish(expander);
}

bool leme_config_expand_templates(
    const struct leme_scfg_block *input, struct leme_scfg_block *output,
    struct leme_diagnostics *diagnostics, const struct leme_scfg_source *source,
    const char *path, struct leme_trail_table *trails,
    const struct leme_env_file *env_file, char **error) {
  struct leme_trail_table local_trails = {0};
  struct leme_expander expander = {
      .source = source,
      .path = path,
      .error = error,
      .diagnostics = diagnostics,
      .trails = trails != NULL ? trails : &local_trails,
      .env_file = env_file,
  };
  bool valid = true;

  *output = (struct leme_scfg_block){0};

  if (!leme_expand_collect_vars(&expander, input)) {
    leme_expander_finish(&expander);
    leme_scfg_block_finish(output);
    if (trails != NULL) {
      leme_trail_table_finish(trails);
    } else {
      leme_trail_table_finish(&local_trails);
    }
    return false;
  }

  if (!leme_expand_collect_lists(&expander, input)) {
    leme_expander_finish(&expander);
    leme_scfg_block_finish(output);
    if (trails != NULL) {
      leme_trail_table_finish(trails);
    } else {
      leme_trail_table_finish(&local_trails);
    }
    return false;
  }

  valid = leme_expand_block(&expander, input, output, true, 0);

  leme_expander_finish(&expander);
  if (!valid) {
    leme_scfg_block_finish(output);
    if (trails != NULL) {
      leme_trail_table_finish(trails);
    } else {
      leme_trail_table_finish(&local_trails);
    }
    return false;
  }
  if (trails == NULL) {
    leme_trail_table_finish(&local_trails);
  }
  return true;
}
