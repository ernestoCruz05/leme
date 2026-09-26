#include "public/model-internal.h"
#include "public/schema-internal.h"
#include "public/value-internal.h"

#include <stdalign.h>
#include <stdlib.h>
#include <string.h>

void leme_public_cached_root_ref(struct leme_public_cached_root *root) {
  if (root == NULL)
    return;
  if (root->references == 0 || root->references == SIZE_MAX)
    abort();
  ++root->references;
}

void leme_public_cached_root_unref(struct leme_public_cached_root *root) {
  if (root == NULL)
    return;
  if (root->references == 0)
    abort();
  if (--root->references == 0)
    leme_public_builder_destroy(root->builder);
}

void leme_public_model_clear_cache(struct leme_public_model *model,
                                   uint32_t roots) {
  for (unsigned i = 0; i < LEME_PUBLIC_ROOT_COUNT; ++i) {
    if ((roots & LEME_PUBLIC_ROOT_BIT(i)) == 0)
      continue;
    leme_public_cached_root_unref(model->cached[i]);
    model->cached[i] = NULL;
  }
}

void leme_public_model_sync_source(struct leme_public_model *model,
                                   const struct leme_public_source *source) {
  if (model->source.context != source->context ||
      model->source.root != source->root ||
      model->source.locked != source->locked ||
      model->source.root_generation != source->root_generation ||
      model->source.cache_snapshots != source->cache_snapshots) {
    leme_public_model_clear_cache(model, LEME_PUBLIC_ALL_ROOTS);
    model->dirty = true;
    model->source = *source;
  }
  for (unsigned i = 0; i < LEME_PUBLIC_ROOT_COUNT; ++i) {
    struct leme_public_cached_root *cached = model->cached[i];
    if (cached == NULL)
      continue;
    const uint64_t generation =
        source->root_generation == NULL
            ? 0
            : source->root_generation(source->context,
                                      (enum leme_public_root)i);
    if (generation == 0 || generation != cached->generation) {
      leme_public_cached_root_unref(cached);
      model->cached[i] = NULL;
      model->dirty = true;
    }
  }
}

static bool has_references(const struct leme_public_schema *schema,
                           size_t depth) {
  if (schema == NULL)
    return false;
  if (depth > LEME_PUBLIC_MAX_DEPTH || schema->related != NULL)
    return true;
  if (schema->rule == LEME_RULE_ANY)
    return false;
  if (has_references(schema->primary, depth + 1) ||
      has_references(schema->alternative, depth + 1) ||
      has_references(schema->items, depth + 1))
    return true;
  for (size_t i = 0; i < schema->field_count; ++i)
    if (has_references(schema->fields[i].type, depth + 1))
      return true;
  return false;
}

enum leme_public_status leme_public_model_cached_root(
    struct leme_public_model *model, const struct leme_public_source *source,
    enum leme_public_root root, const struct leme_public_work *work,
    struct leme_public_capture_diagnostic *diagnostic,
    struct leme_public_cached_root **out) {
  *out = NULL;
  if (source->root_generation == NULL ||
      (root != LEME_PUBLIC_CONFIG && root != LEME_PUBLIC_RUNTIME))
    return LEME_PUBLIC_OK;
  const uint64_t generation = source->root_generation(source->context, root);
  if (generation == 0)
    return LEME_PUBLIC_OK;
  if (model->cached[root] != NULL) {
    *out = model->cached[root];
    return LEME_PUBLIC_OK;
  }
  const bool reference_free = !has_references(leme_public_root_schema(root), 1);
  struct leme_public_builder *b = NULL;
  diagnostic->stage = "builder";
  enum leme_public_status status =
      leme_public_model_builder_create(model, model->snapshot_limit, &b);
  if (status != LEME_PUBLIC_OK)
    return status;
  leme_public_builder_set_work(b, work);
  struct leme_public_cached_root *cached = leme_public_allocate(
      b, 1, sizeof(*cached), alignof(struct leme_public_cached_root));
  if (cached == NULL) {
    status = leme_public_builder_status(b);
    goto fail;
  }
  struct leme_public_value *value = NULL;
  diagnostic->stage = "source";
  status = source->root(source->context, model, b, root, &value);
  if (status != LEME_PUBLIC_OK)
    goto fail;
  if (value == NULL || value->owner != b) {
    status = LEME_PUBLIC_INVALID;
    goto fail;
  }
  diagnostic->stage = "schema";
  status = leme_public_schema_validate(leme_public_root_schema(root), value);
  if (status != LEME_PUBLIC_OK)
    goto fail;
  diagnostic->stage = "seal";
  const struct leme_public_value *roots[] = {value};
  status = leme_public_builder_seal(b, roots, 1);
  if (status != LEME_PUBLIC_OK)
    goto fail;
  diagnostic->stage = "lock";
  if (root == LEME_PUBLIC_CONFIG && source->locked(source->context)) {
    leme_public_model_lock_changed(model, true);
    status = LEME_PUBLIC_LOCKED;
    goto fail;
  }
  diagnostic->stage = "generation";
  if (!leme_public_model_available(model) ||
      source->root_generation(source->context, root) != generation) {
    status = LEME_PUBLIC_UNAVAILABLE;
    goto fail;
  }
  leme_public_builder_set_work(b, NULL);
  *cached = (struct leme_public_cached_root){.references = 1,
                                             .generation = generation,
                                             .reference_free = reference_free,
                                             .builder = b,
                                             .value = value};
  model->cached[root] = cached;
  *out = cached;
  return LEME_PUBLIC_OK;
fail:
  leme_public_builder_destroy(b);
  return status;
}

enum leme_public_status
leme_public_snapshot_project(struct leme_public_model *model, uint32_t roots,
                             struct leme_public_snapshot **out) {
  struct leme_public_snapshot *baseline = model->baseline;
  if (baseline->roots_mask == roots) {
    leme_public_snapshot_ref(baseline);
    *out = baseline;
    return LEME_PUBLIC_OK;
  }
  struct leme_public_builder *b = NULL;
  enum leme_public_status status =
      leme_public_model_builder_create(model, model->snapshot_limit, &b);
  if (status != LEME_PUBLIC_OK)
    return status;
  struct leme_public_snapshot *snapshot = leme_public_allocate(
      b, 1, sizeof(*snapshot), alignof(struct leme_public_snapshot));
  if (snapshot == NULL) {
    status = leme_public_builder_status(b);
    goto fail;
  }
  const size_t bytes = leme_public_builder_bytes(b);
  if (baseline->bytes > model->snapshot_limit - bytes) {
    status = LEME_PUBLIC_LIMIT;
    goto fail;
  }
  status = leme_public_builder_seal(b, NULL, 0);
  if (status != LEME_PUBLIC_OK)
    goto fail;
  *snapshot = (struct leme_public_snapshot){.references = 1,
                                            .bytes = bytes + baseline->bytes,
                                            .builder = b,
                                            .roots_mask = roots,
                                            .backing = baseline,
                                            .null_value = baseline->null_value};
  memcpy(snapshot->roots, baseline->roots, sizeof(snapshot->roots));
  memcpy(snapshot->indexes, baseline->indexes, sizeof(snapshot->indexes));
  memcpy(snapshot->instance, baseline->instance, sizeof(snapshot->instance));
  memcpy(snapshot->revision, baseline->revision, sizeof(snapshot->revision));
  leme_public_snapshot_ref(baseline);
  *out = snapshot;
  return LEME_PUBLIC_OK;
fail:
  leme_public_builder_destroy(b);
  return status;
}
