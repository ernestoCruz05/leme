#include "config/public-internal.h"
#include "public/value-internal.h"

#include <limits.h>

static const char *severity_name(enum leme_diagnostic_severity severity) {
  switch (severity) {
  case LEME_DIAGNOSTIC_ERROR:
    return "error";
  case LEME_DIAGNOSTIC_WARNING:
    return "warning";
  }
  return NULL;
}

static const char *trail_name(enum leme_trail_kind kind) {
  switch (kind) {
  case LEME_TRAIL_FOR:
    return "for";
  case LEME_TRAIL_IF:
    return "if";
  }
  return NULL;
}

static enum leme_public_status strings_value(struct leme_public_builder *b,
                                             char *const *strings, size_t count,
                                             struct leme_public_value **out) {
  if (count != 0 && strings == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  struct leme_public_value *array = NULL;
  if (leme_public_array(b, count, &array) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  for (size_t i = 0; i < count; ++i) {
    struct leme_public_value *value = NULL;
    if (strings[i] == NULL)
      return leme_public_fail(b, LEME_PUBLIC_INVALID);
    if (leme_config_public_text(b, strings[i], &value) != LEME_PUBLIC_OK ||
        leme_public_array_set(b, array, i, value) != LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
  }
  *out = array;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status
location_value(struct leme_public_builder *b, const struct leme_config *config,
               const struct leme_diagnostic_span *span, bool secondary,
               struct leme_public_value **out) {
  const struct leme_source_table *table = &config->sources;
  if (table->count > LEME_SOURCE_TABLE_MAX)
    return leme_public_fail(b, LEME_PUBLIC_LIMIT);
  const struct leme_scfg_source *source =
      table->entries == NULL ? NULL
                             : leme_source_table_get(table, span->source);
  const char *path =
      table->paths == NULL ? NULL : leme_source_table_path(table, span->source);
  bool available = source != NULL && source->data != NULL &&
                   source->line_offsets != NULL && source->line_count != 0 &&
                   source->line_count <= INT_MAX && source->length <= INT_MAX &&
                   (span->span.offset != 0 || span->span.length != 0) &&
                   span->span.offset <= source->length &&
                   span->span.length <= source->length - span->span.offset;
  int line = 0, column = 0;
  if (available) {
    leme_scfg_source_locate(source, span->span, &line, &column);
    available = line > 0 && column > 0;
  }
  if (!available && !secondary)
    return leme_public_null(b, out);
  struct leme_public_value *record = NULL;
  if (leme_public_object(b, 5, &record) != LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("path"), path) !=
          LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("label"), span->label) !=
          LEME_PUBLIC_OK ||
      (available
           ? leme_public_put_int(b, record, LEME_PUBLIC_TEXT("line"), line)
           : leme_public_put_null(b, record, LEME_PUBLIC_TEXT("line"))) !=
          LEME_PUBLIC_OK ||
      (available
           ? leme_public_put_int(b, record, LEME_PUBLIC_TEXT("column"), column)
           : leme_public_put_null(b, record, LEME_PUBLIC_TEXT("column"))) !=
          LEME_PUBLIC_OK ||
      (available
           ? leme_config_public_count(b, record, LEME_PUBLIC_TEXT("length"),
                                      span->span.length)
           : leme_public_put_null(b, record, LEME_PUBLIC_TEXT("length"))) !=
          LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  *out = record;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status trail_value(struct leme_public_builder *b,
                                           const struct leme_trail_table *table,
                                           uint16_t start,
                                           struct leme_public_value **out) {
  if (table->count > UINT16_MAX)
    return leme_public_fail(b, LEME_PUBLIC_LIMIT);
  uint16_t chain[LEME_CONFIG_EXPAND_MAX_DEPTH] = {0};
  size_t count = 0;
  uint16_t current = start;
  while (current != 0) {
    for (size_t i = 0; i < count; ++i)
      if (chain[i] == current)
        return leme_public_fail(b, LEME_PUBLIC_INVALID);
    if (count == LEME_CONFIG_EXPAND_MAX_DEPTH)
      return leme_public_fail(b, LEME_PUBLIC_LIMIT);
    if (table->nodes == NULL)
      return leme_public_fail(b, LEME_PUBLIC_INVALID);
    const struct leme_trail_node *node = leme_trail_table_get(table, current);
    if (node == NULL || trail_name(node->kind) == NULL)
      return leme_public_fail(b, LEME_PUBLIC_INVALID);
    chain[count++] = current;
    current = node->parent;
  }
  struct leme_public_value *array = NULL;
  if (leme_public_array(b, count, &array) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  for (size_t i = 0; i < count; ++i) {
    const struct leme_trail_node *node =
        leme_trail_table_get(table, chain[count - i - 1]);
    if (node == NULL)
      return leme_public_fail(b, LEME_PUBLIC_INVALID);
    struct leme_public_value *record = NULL;
    if (leme_public_object(b, 3, &record) != LEME_PUBLIC_OK ||
        leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("kind"),
                             trail_name(node->kind)) != LEME_PUBLIC_OK ||
        leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("variable"),
                             node->variable) != LEME_PUBLIC_OK ||
        leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("value"),
                             node->value) != LEME_PUBLIC_OK ||
        leme_public_array_set(b, array, i, record) != LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
  }
  *out = array;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status diagnostic_value(
    struct leme_public_builder *b, const struct leme_config *config,
    const struct leme_diagnostic *entry, struct leme_public_value **out) {
  const char *severity = severity_name(entry->severity);
  if (severity == NULL || entry->message == NULL ||
      (entry->secondary_count != 0 && entry->secondary == NULL) ||
      (entry->iterations_count != 0 && entry->iterations == NULL))
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  if (entry->iterations_count > LEME_CONFIG_EXPAND_MAX_TOTAL_ITERATIONS)
    return leme_public_fail(b, LEME_PUBLIC_LIMIT);
  struct leme_public_value *record = NULL, *primary = NULL, *secondary = NULL,
                           *notes = NULL, *helps = NULL;
  struct leme_public_value *expansion = NULL, *iterations = NULL;
  if (leme_public_object(b, 8, &record) != LEME_PUBLIC_OK ||
      location_value(b, config, &entry->primary, false, &primary) !=
          LEME_PUBLIC_OK ||
      leme_public_array(b, entry->secondary_count, &secondary) !=
          LEME_PUBLIC_OK ||
      leme_public_array(b, entry->iterations_count, &iterations) !=
          LEME_PUBLIC_OK ||
      strings_value(b, entry->notes, entry->notes_count, &notes) !=
          LEME_PUBLIC_OK ||
      strings_value(b, entry->helps, entry->helps_count, &helps) !=
          LEME_PUBLIC_OK ||
      trail_value(b, &config->trails, entry->trail, &expansion) !=
          LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  for (size_t i = 0; i < entry->secondary_count; ++i) {
    struct leme_public_value *location = NULL;
    if (location_value(b, config, &entry->secondary[i], true, &location) !=
            LEME_PUBLIC_OK ||
        leme_public_array_set(b, secondary, i, location) != LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
  }
  for (size_t i = 0; i < entry->iterations_count; ++i) {
    struct leme_public_value *trail = NULL;
    if (trail_value(b, &config->trails, entry->iterations[i], &trail) !=
            LEME_PUBLIC_OK ||
        leme_public_array_set(b, iterations, i, trail) != LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
  }
  if (leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("severity"), severity) !=
          LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("message"),
                           entry->message) != LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("primary"), primary) !=
          LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("secondary"),
                             secondary) != LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("notes"), notes) !=
          LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("helps"), helps) !=
          LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("expansion"),
                             expansion) != LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("iterations"),
                             iterations) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  *out = record;
  return LEME_PUBLIC_OK;
}

enum leme_public_status
leme_config_public_diagnostics(struct leme_public_builder *b,
                               const struct leme_config *config,
                               struct leme_public_value **out) {
  if (out == NULL)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  *out = NULL;
  if (config == NULL ||
      (config->diagnostics.count != 0 && config->diagnostics.entries == NULL))
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  if (config->diagnostics.count > LEME_DIAGNOSTICS_MAX)
    return leme_public_fail(b, LEME_PUBLIC_LIMIT);
  struct leme_public_value *record = NULL, *entries = NULL;
  if (leme_public_object(b, 2, &record) != LEME_PUBLIC_OK ||
      leme_public_array(b, config->diagnostics.count, &entries) !=
          LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  for (size_t i = 0; i < config->diagnostics.count; ++i) {
    struct leme_public_value *entry = NULL;
    if (diagnostic_value(b, config, &config->diagnostics.entries[i], &entry) !=
            LEME_PUBLIC_OK ||
        leme_public_array_set(b, entries, i, entry) != LEME_PUBLIC_OK)
      return leme_public_builder_status(b);
  }
  if (leme_public_object_set(b, record, LEME_PUBLIC_TEXT("entries"), entries) !=
          LEME_PUBLIC_OK ||
      leme_public_put_bool(b, record, LEME_PUBLIC_TEXT("truncated"),
                           config->diagnostics.truncated) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  *out = record;
  return LEME_PUBLIC_OK;
}
