#include "public/model-internal.h"
#include "public/schema-internal.h"
#include "public/value-internal.h"

#include <inttypes.h>
#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int compare_text(struct leme_public_text lhs,
                        struct leme_public_text rhs) {
  const size_t length = lhs.length < rhs.length ? lhs.length : rhs.length;
  const int result = length == 0 ? 0 : memcmp(lhs.data, rhs.data, length);
  return result != 0               ? result
         : lhs.length < rhs.length ? -1
         : lhs.length > rhs.length ? 1
                                   : 0;
}

static struct leme_public_text record_text(const struct leme_public_value *row,
                                           struct leme_public_text key) {
  struct leme_public_text text = {0};
  if (leme_public_as_text(leme_public_get(row, key), &text) != LEME_PUBLIC_OK)
    return (struct leme_public_text){0};
  return text;
}

enum leme_public_status
leme_public_model_create(const struct leme_public_options *options,
                         struct leme_public_model **out) {
  if (out == NULL)
    return LEME_PUBLIC_INVALID;
  *out = NULL;
  const struct leme_public_options selected =
      options == NULL ? (struct leme_public_options){0} : *options;
  const struct leme_public_allocator allocator =
      selected.allocator.allocate == NULL && selected.allocator.release == NULL
          ? leme_public_default_allocator()
          : selected.allocator;
  struct leme_public_budget *budget = NULL;
  enum leme_public_status status = leme_public_budget_create(
      selected.total_limit == 0 ? LEME_PUBLIC_TOTAL_BYTES
                                : selected.total_limit,
      &allocator, &budget);
  if (status != LEME_PUBLIC_OK)
    return status;
  status = leme_public_budget_reserve(budget, sizeof(struct leme_public_model));
  if (status != LEME_PUBLIC_OK) {
    leme_public_budget_unref(budget);
    return status;
  }
  struct leme_public_model *model =
      allocator.allocate(allocator.context, sizeof(*model));
  if (model == NULL) {
    leme_public_budget_release(budget, sizeof(*model));
    leme_public_budget_unref(budget);
    return LEME_PUBLIC_OOM;
  }
  *model = (struct leme_public_model){
      .allocator = allocator,
      .budget = budget,
      .snapshot_limit = selected.snapshot_limit == 0
                            ? LEME_PUBLIC_SNAPSHOT_BYTES
                            : selected.snapshot_limit,
      .total_limit = selected.total_limit == 0 ? LEME_PUBLIC_TOTAL_BYTES
                                               : selected.total_limit};
  unsigned char entropy[16] = {0};
  status = selected.entropy == NULL
               ? leme_public_system_entropy(NULL, entropy, sizeof(entropy))
               : selected.entropy(selected.entropy_context, entropy,
                                  sizeof(entropy));
  if (status != LEME_PUBLIC_OK) {
    leme_public_model_destroy(model);
    return status;
  }
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < sizeof(entropy); ++i) {
    model->instance[i * 2] = digits[entropy[i] >> 4u];
    model->instance[i * 2 + 1] = digits[entropy[i] & 15u];
  }
  model->available = true;
  model->dirty = true;
  *out = model;
  return LEME_PUBLIC_OK;
}

void leme_public_model_destroy(struct leme_public_model *model) {
  if (model == NULL)
    return;
  leme_public_snapshot_unref(model->baseline);
  leme_public_model_clear_cache(model, LEME_PUBLIC_ALL_ROOTS);
  struct leme_public_budget *budget = model->budget;
  const struct leme_public_allocator allocator = model->allocator;
  allocator.release(allocator.context, model);
  leme_public_budget_release(budget, sizeof(*model));
  leme_public_budget_unref(budget);
}

bool leme_public_model_available(const struct leme_public_model *model) {
  return model != NULL && model->available;
}

void leme_public_model_disable(struct leme_public_model *model) {
  if (model == NULL || !model->available)
    return;
  model->available = false;
  leme_public_snapshot_unref(model->baseline);
  model->baseline = NULL;
  leme_public_model_clear_cache(model, LEME_PUBLIC_ALL_ROOTS);
  if (model->notify != NULL)
    model->notify(model->notify_context, LEME_PUBLIC_DISABLED);
}

struct leme_public_text
leme_public_model_instance(const struct leme_public_model *model) {
  return model == NULL
             ? (struct leme_public_text){0}
             : (struct leme_public_text){.data = model->instance, .length = 32};
}

void leme_public_model_set_observer(struct leme_public_model *model,
                                    leme_public_changed_fn notify,
                                    void *context) {
  if (model == NULL)
    return;
  model->notify = notify;
  model->notify_context = context;
}

void leme_public_model_invalidate(struct leme_public_model *model) {
  if (model == NULL || !model->available)
    return;
  model->dirty = true;
  if (model->notify != NULL)
    model->notify(model->notify_context, LEME_PUBLIC_CHANGED);
}

void leme_public_model_lock_changed(struct leme_public_model *model,
                                    bool locked) {
  if (model == NULL || model->locked == locked)
    return;
  model->locked = locked;
  model->dirty = true;
  leme_public_snapshot_unref(model->baseline);
  model->baseline = NULL;
  leme_public_model_clear_cache(model,
                                LEME_PUBLIC_ALL_ROOTS & ~LEME_PUBLIC_SAFE_ROOTS);
  if (model->notify != NULL)
    model->notify(model->notify_context, locked ? LEME_PUBLIC_LOCKED_CHANGED
                                                : LEME_PUBLIC_UNLOCKED_CHANGED);
}

struct leme_public_budget *
leme_public_model_budget_ref(struct leme_public_model *model) {
  if (model == NULL)
    return NULL;
  leme_public_budget_ref(model->budget);
  return model->budget;
}

enum leme_public_status
leme_public_model_builder_create(struct leme_public_model *model,
                                 size_t maximum_bytes,
                                 struct leme_public_builder **out) {
  if (out == NULL)
    return LEME_PUBLIC_INVALID;
  *out = NULL;
  if (!leme_public_model_available(model))
    return LEME_PUBLIC_UNAVAILABLE;
  return leme_public_builder_with_budget(maximum_bytes, model->budget,
                                         &model->allocator, out);
}

void leme_public_snapshot_ref(struct leme_public_snapshot *snapshot) {
  if (snapshot == NULL)
    return;
  if (snapshot->references == 0 || snapshot->references == SIZE_MAX)
    abort();
  ++snapshot->references;
}

void leme_public_snapshot_unref(struct leme_public_snapshot *snapshot) {
  if (snapshot == NULL)
    return;
  if (snapshot->references == 0)
    abort();
  if (--snapshot->references == 0) {
    leme_public_snapshot_unref(snapshot->backing);
    for (unsigned i = 0; i < LEME_PUBLIC_ROOT_COUNT; ++i)
      leme_public_cached_root_unref(snapshot->cached[i]);
    leme_public_builder_destroy(snapshot->builder);
  }
}

const struct leme_public_value *
leme_public_snapshot_root(const struct leme_public_snapshot *snapshot,
                          enum leme_public_root root) {
  if (snapshot == NULL || (unsigned)root >= (unsigned)LEME_PUBLIC_ROOT_COUNT ||
      (snapshot->roots_mask & LEME_PUBLIC_ROOT_BIT(root)) == 0)
    return NULL;
  return snapshot->roots[root];
}

struct leme_public_text
leme_public_snapshot_instance(const struct leme_public_snapshot *snapshot) {
  return snapshot == NULL ? (struct leme_public_text){0}
                          : (struct leme_public_text){
                                .data = snapshot->instance, .length = 32};
}

struct leme_public_text
leme_public_snapshot_revision(const struct leme_public_snapshot *snapshot) {
  return snapshot == NULL || snapshot->revision[0] == '\0'
             ? (struct leme_public_text){0}
             : (struct leme_public_text){.data = snapshot->revision,
                                         .length = strlen(snapshot->revision)};
}

static int compare_entries(const void *lhs, const void *rhs) {
  const struct leme_public_index_entry *left = lhs;
  const struct leme_public_index_entry *right = rhs;
  return compare_text(left->id, right->id);
}

static enum leme_public_status index_root(struct leme_public_snapshot *snapshot,
                                          const struct leme_public_model *model,
                                          enum leme_public_entity kind,
                                          struct leme_public_value *array) {
  struct leme_public_index *index = &snapshot->indexes[kind];
  index->count = leme_public_length(array);
  if (index->count == 0)
    return LEME_PUBLIC_OK;
  index->entries = leme_public_allocate(
      snapshot->builder, index->count, sizeof(*index->entries),
      alignof(struct leme_public_index_entry));
  if (index->entries == NULL)
    return leme_public_builder_status(snapshot->builder);
  for (size_t i = 0; i < index->count; ++i) {
    if (snapshot->builder->work.step != NULL) {
      enum leme_public_status st =
          snapshot->builder->work.step(snapshot->builder->work.context, 1);
      if (st != LEME_PUBLIC_OK)
        return st;
    }
    const struct leme_public_value *row = leme_public_at(array, i);
    const struct leme_public_text id = record_text(row, LEME_PUBLIC_TEXT("id"));
    struct leme_public_id serial = {0};
    uint16_t slot = 0;
    if (!leme_public_parse_id(model, kind, id, &serial, &slot) ||
        compare_text(record_text(row, LEME_PUBLIC_TEXT("instance")),
                     leme_public_model_instance(model)) != 0)
      return LEME_PUBLIC_INVALID;
    if (kind == LEME_PUBLIC_TAG) {
      int64_t number = 0;
      struct leme_public_id output_id = {0};
      uint16_t output_slot = 0;
      if (leme_public_as_integer(
              leme_public_get(row, LEME_PUBLIC_TEXT("number")), &number) !=
              LEME_PUBLIC_OK ||
          number != slot ||
          !leme_public_parse_id(
              model, LEME_PUBLIC_OUTPUT,
              record_text(leme_public_get(row, LEME_PUBLIC_TEXT("output")),
                          LEME_PUBLIC_TEXT("id")),
              &output_id, &output_slot) ||
          output_id.serial != serial.serial)
        return LEME_PUBLIC_INVALID;
    }
    index->entries[i] = (struct leme_public_index_entry){.id = id, .row = row};
  }
  if (snapshot->builder->work.step != NULL && index->count > 0) {
    enum leme_public_status st =
        snapshot->builder->work.step(snapshot->builder->work.context, index->count);
    if (st != LEME_PUBLIC_OK)
      return st;
  }
  qsort(index->entries, index->count, sizeof(*index->entries), compare_entries);
  for (size_t i = 0; i < index->count; ++i) {
    if (i != 0 &&
        compare_text(index->entries[i - 1].id, index->entries[i].id) == 0)
      return LEME_PUBLIC_INVALID;
    array->data.array.items[i] = index->entries[i].row;
  }
  return LEME_PUBLIC_OK;
}

enum leme_public_status leme_public_snapshot_find(
    const struct leme_public_snapshot *snapshot, enum leme_public_entity kind,
    struct leme_public_text id, const struct leme_public_value **out) {
  if (out == NULL)
    return LEME_PUBLIC_INVALID;
  *out = NULL;
  if (snapshot == NULL || (unsigned)kind >= LEME_PUBLIC_ENTITY_COUNT ||
      (id.data == NULL && id.length != 0))
    return LEME_PUBLIC_INVALID;
  if (id.length > 56)
    return LEME_PUBLIC_NOT_FOUND;
  const struct leme_public_index *index = &snapshot->indexes[kind];
  size_t low = 0, high = index->count;
  while (low < high) {
    const size_t middle = low + (high - low) / 2;
    const int comparison = compare_text(id, index->entries[middle].id);
    if (comparison == 0) {
      *out = index->entries[middle].row;
      return LEME_PUBLIC_OK;
    }
    if (comparison < 0)
      high = middle;
    else
      low = middle + 1;
  }
  return LEME_PUBLIC_NOT_FOUND;
}

static enum leme_public_status
resolve_reference(const struct leme_public_snapshot *snapshot,
                  const struct leme_public_schema *schema,
                  const struct leme_public_value *value,
                  const struct leme_public_value **out) {
  *out = NULL;
  const enum leme_public_status status =
      leme_public_schema_validate(schema, value);
  if (status != LEME_PUBLIC_OK)
    return status;
  if (compare_text(record_text(value, LEME_PUBLIC_TEXT("instance")),
                   leme_public_snapshot_instance(snapshot)) != 0)
    return LEME_PUBLIC_INVALID;
  for (unsigned i = 0; i < LEME_PUBLIC_ENTITY_COUNT; ++i) {
    const enum leme_public_entity kind = (enum leme_public_entity)i;
    if (schema->related == leme_public_entity_schema(kind)) {
      return leme_public_snapshot_find(
          snapshot, kind, record_text(value, LEME_PUBLIC_TEXT("id")), out);
    }
  }
  return LEME_PUBLIC_INVALID;
}

static enum leme_public_status
validate_references(const struct leme_public_snapshot *snapshot,
                    const struct leme_public_schema *schema,
                    const struct leme_public_value *value, size_t depth) {
  if (depth > LEME_PUBLIC_MAX_DEPTH)
    return LEME_PUBLIC_LIMIT;
  if (snapshot->builder->work.step != NULL) {
    enum leme_public_status st =
        snapshot->builder->work.step(snapshot->builder->work.context, 1);
    if (st != LEME_PUBLIC_OK)
      return st;
  }
  if (leme_public_kind(value) == LEME_PUBLIC_NULL ||
      schema->rule == LEME_RULE_ANY)
    return LEME_PUBLIC_OK;
  if (schema->primary != NULL) {
    const struct leme_public_schema *selected =
        leme_public_schema_validate(schema->primary, value) == LEME_PUBLIC_OK
            ? schema->primary
            : schema->alternative;
    return validate_references(snapshot, selected, value, depth + 1);
  }
  if (schema->related != NULL) {
    const struct leme_public_value *target = NULL;
    return resolve_reference(snapshot, schema, value, &target) == LEME_PUBLIC_OK
               ? LEME_PUBLIC_OK
               : LEME_PUBLIC_INVALID;
  }
  if (schema->kind == LEME_PUBLIC_ARRAY) {
    for (size_t i = 0; i < leme_public_length(value); ++i) {
      const enum leme_public_status status = validate_references(
          snapshot, schema->items, leme_public_at(value, i), depth + 1);
      if (status != LEME_PUBLIC_OK)
        return status;
    }
  } else if (schema->kind == LEME_PUBLIC_OBJECT) {
    for (size_t i = 0; i < schema->field_count; ++i) {
      const struct leme_public_field *field = &schema->fields[i];
      const struct leme_public_value *child =
          leme_public_get(value, field->name);
      if (child == NULL)
        continue;
      const enum leme_public_status status =
          validate_references(snapshot, field->type, child, depth + 1);
      if (status != LEME_PUBLIC_OK)
        return status;
    }
  }
  return LEME_PUBLIC_OK;
}

static enum leme_public_status
same_roots_checked(const struct leme_public_snapshot *lhs,
                   const struct leme_public_snapshot *rhs,
                   const struct leme_public_work *work, bool *out_same) {
  if (lhs == NULL || rhs == NULL) {
    *out_same = false;
    return LEME_PUBLIC_OK;
  }
  for (unsigned i = 0; i < (unsigned)LEME_PUBLIC_ROOT_COUNT; ++i) {
    bool equal = false;
    enum leme_public_status st =
        leme_public_equal_checked(lhs->roots[i], rhs->roots[i], work, &equal);
    if (st != LEME_PUBLIC_OK)
      return st;
    if (!equal) {
      *out_same = false;
      return LEME_PUBLIC_OK;
    }
  }
  *out_same = true;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status
capture_work(struct leme_public_model *model,
             const struct leme_public_source *source, uint32_t requested_roots,
             const struct leme_public_work *work,
             struct leme_public_capture_diagnostic *diagnostic,
             struct leme_public_snapshot **out) {
  if (out == NULL)
    return LEME_PUBLIC_INVALID;
  *out = NULL;
  if (source == NULL || source->locked == NULL || source->root == NULL ||
      requested_roots == 0 || (requested_roots & ~LEME_PUBLIC_ALL_ROOTS) != 0)
    return LEME_PUBLIC_INVALID;
  if (!leme_public_model_available(model))
    return LEME_PUBLIC_UNAVAILABLE;
  leme_public_model_sync_source(model, source);
  const bool locked = source->locked(source->context);
  leme_public_model_lock_changed(model, locked);
  const bool sensitive = (requested_roots & ~LEME_PUBLIC_SAFE_ROOTS) != 0;
  if (locked && sensitive)
    return LEME_PUBLIC_LOCKED;
  diagnostic->stage = "work";
  if (work != NULL && work->step != NULL) {
    const enum leme_public_status status = work->step(work->context, 1);
    if (status != LEME_PUBLIC_OK)
      return status;
  }
  if (sensitive && source->cache_snapshots && !model->dirty &&
      model->baseline != NULL) {
    diagnostic->stage = "reuse";
    if (source->locked(source->context)) {
      leme_public_model_lock_changed(model, true);
      return LEME_PUBLIC_LOCKED;
    }
    return leme_public_snapshot_project(model, requested_roots, out);
  }
  const uint32_t capture_roots =
      sensitive ? LEME_PUBLIC_ALL_ROOTS : requested_roots;
  struct leme_public_builder *b = NULL;
  diagnostic->stage = "builder";
  enum leme_public_status status =
      leme_public_model_builder_create(model, model->snapshot_limit, &b);
  if (status != LEME_PUBLIC_OK)
    return status;
  leme_public_builder_set_work(b, work);
  diagnostic->stage = "allocate";
  struct leme_public_snapshot *snapshot = leme_public_allocate(
      b, 1, sizeof(*snapshot), alignof(struct leme_public_snapshot));
  if (snapshot == NULL) {
    status = leme_public_builder_status(b);
    leme_public_builder_destroy(b);
    return status;
  }
  *snapshot = (struct leme_public_snapshot){
      .builder = b, .references = 1, .roots_mask = requested_roots};
  memcpy(snapshot->instance, model->instance, sizeof(snapshot->instance));
  struct leme_public_value *null_value = NULL;
  status = leme_public_null(b, &null_value);
  if (status != LEME_PUBLIC_OK)
    goto cleanup;
  snapshot->null_value = null_value;
  const struct leme_public_value *seal_roots[LEME_PUBLIC_ROOT_COUNT + 1] = {
      null_value};
  size_t root_count = 1;
  for (unsigned i = 0; i < (unsigned)LEME_PUBLIC_ROOT_COUNT; ++i) {
    const enum leme_public_root root = (enum leme_public_root)i;
    if ((capture_roots & LEME_PUBLIC_ROOT_BIT(root)) == 0)
      continue;
    diagnostic->root = root;
    diagnostic->stage = "lock";
    if (sensitive && source->locked(source->context)) {
      leme_public_model_lock_changed(model, true);
      status = LEME_PUBLIC_LOCKED;
      goto cleanup;
    }
    diagnostic->stage = "work";
    if (work != NULL && work->step != NULL) {
      status = work->step(work->context, 1);
      if (status != LEME_PUBLIC_OK)
        goto cleanup;
    }
    struct leme_public_cached_root *cached = NULL;
    status = leme_public_model_cached_root(model, source, root, work,
                                           diagnostic, &cached);
    if (status != LEME_PUBLIC_OK)
      goto cleanup;
    if (cached != NULL) {
      leme_public_cached_root_ref(cached);
      snapshot->cached[i] = cached;
      snapshot->roots[i] = cached->value;
      continue;
    }
    struct leme_public_value *value = NULL;
    diagnostic->stage = "source";
    status = source->root(source->context, model, b, root, &value);
    if (status != LEME_PUBLIC_OK)
      goto cleanup;
    if (value == NULL || value->owner != b) {
      status = LEME_PUBLIC_INVALID;
      goto cleanup;
    }
    diagnostic->stage = "schema";
    status = leme_public_schema_validate(leme_public_root_schema(root), value);
    if (status != LEME_PUBLIC_OK)
      goto cleanup;
    snapshot->roots[i] = value;
    seal_roots[root_count++] = value;
    if (i < LEME_PUBLIC_ENTITY_COUNT) {
      diagnostic->stage = "index";
      status = index_root(snapshot, model, (enum leme_public_entity)i, value);
      if (status != LEME_PUBLIC_OK)
        goto cleanup;
    }
  }
  diagnostic->root = LEME_PUBLIC_ROOT_COUNT;
  diagnostic->stage = "availability";
  if (!leme_public_model_available(model)) {
    status = LEME_PUBLIC_UNAVAILABLE;
    goto cleanup;
  }
  diagnostic->stage = "lock";
  if (source->locked(source->context) != locked) {
    leme_public_model_lock_changed(model, !locked);
    status = LEME_PUBLIC_LOCKED;
    goto cleanup;
  }
  diagnostic->stage = "account";
  snapshot->bytes = leme_public_builder_bytes(b);
  for (unsigned i = 0; i < LEME_PUBLIC_ROOT_COUNT; ++i) {
    if (snapshot->cached[i] == NULL)
      continue;
    const size_t bytes =
        leme_public_builder_bytes(snapshot->cached[i]->builder);
    if (bytes > model->snapshot_limit - snapshot->bytes) {
      status = LEME_PUBLIC_LIMIT;
      goto cleanup;
    }
    snapshot->bytes += bytes;
  }
  diagnostic->stage = "seal";
  status = leme_public_builder_seal(b, seal_roots, root_count);
  if (status != LEME_PUBLIC_OK)
    goto cleanup;
  size_t expanded = snapshot->null_value->mark->work;
  for (unsigned i = 0; i < LEME_PUBLIC_ROOT_COUNT; ++i) {
    if (snapshot->roots[i] == NULL)
      continue;
    const size_t units = snapshot->roots[i]->mark->work;
    if (units > model->snapshot_limit - expanded) {
      status = LEME_PUBLIC_LIMIT;
      goto cleanup;
    }
    expanded += units;
  }
  if (sensitive) {
    diagnostic->stage = "references";
    for (unsigned i = 0; i < (unsigned)LEME_PUBLIC_ROOT_COUNT; ++i) {
      diagnostic->root = (enum leme_public_root)i;
      if (snapshot->cached[i] != NULL && snapshot->cached[i]->reference_free)
        continue;
      status = validate_references(
          snapshot, leme_public_root_schema((enum leme_public_root)i),
          snapshot->roots[i], 1);
      if (status != LEME_PUBLIC_OK)
        goto cleanup;
    }
    bool same = false;
    diagnostic->root = LEME_PUBLIC_ROOT_COUNT;
    diagnostic->stage = "compare";
    status = same_roots_checked(snapshot, model->baseline, work, &same);
    if (status != LEME_PUBLIC_OK)
      goto cleanup;
    const bool changed = !same;
    diagnostic->stage = "revision";
    if (changed && model->revision == UINT64_MAX) {
      leme_public_model_disable(model);
      status = LEME_PUBLIC_LIMIT;
      goto cleanup;
    }
    const uint64_t revision =
        model->revision + (changed ? UINT64_C(1) : UINT64_C(0));
    const int written = snprintf(snapshot->revision, sizeof(snapshot->revision),
                                 "%" PRIu64, revision);
    if (written < 0 || (size_t)written >= sizeof(snapshot->revision)) {
      status = LEME_PUBLIC_INVALID;
      goto cleanup;
    }
    model->revision = revision;
    leme_public_snapshot_unref(model->baseline);
    model->baseline = snapshot;
    leme_public_snapshot_ref(snapshot);
    model->dirty = false;
  }
  leme_public_builder_set_work(b, NULL);
  *out = snapshot;
  diagnostic->root = LEME_PUBLIC_ROOT_COUNT;
  diagnostic->stage = "complete";
  return LEME_PUBLIC_OK;
cleanup:
  leme_public_snapshot_unref(snapshot);
  return status;
}

enum leme_public_status leme_public_model_capture_work_diagnostic(
    struct leme_public_model *model, const struct leme_public_source *source,
    uint32_t requested_roots, const struct leme_public_work *work,
    struct leme_public_capture_diagnostic *diagnostic,
    struct leme_public_snapshot **out) {
  struct leme_public_capture_diagnostic local = {
      .stage = "setup", .root = LEME_PUBLIC_ROOT_COUNT};
  const enum leme_public_status status =
      capture_work(model, source, requested_roots, work, &local, out);
  if (diagnostic != NULL)
    *diagnostic = local;
  return status;
}

enum leme_public_status leme_public_model_capture_work(
    struct leme_public_model *model, const struct leme_public_source *source,
    uint32_t requested_roots, const struct leme_public_work *work,
    struct leme_public_snapshot **out) {
  return leme_public_model_capture_work_diagnostic(
      model, source, requested_roots, work, NULL, out);
}

enum leme_public_status leme_public_model_capture(
    struct leme_public_model *model, const struct leme_public_source *source,
    uint32_t requested_roots, struct leme_public_snapshot **out) {
  return leme_public_model_capture_work(model, source, requested_roots, NULL,
                                        out);
}

enum leme_public_status
leme_public_snapshot_field(const struct leme_public_snapshot *snapshot,
                           const struct leme_public_schema *schema,
                           const struct leme_public_value *row,
                           const struct leme_public_text *path, size_t count,
                           const struct leme_public_value **out) {
  if (out == NULL)
    return LEME_PUBLIC_INVALID;
  *out = NULL;
  const struct leme_public_field *field = NULL;
  enum leme_public_status status =
      leme_public_schema_path(schema, path, count, &field);
  if (status != LEME_PUBLIC_OK)
    return status;
  if (snapshot == NULL || row == NULL || !row->owner->sealed)
    return LEME_PUBLIC_INVALID;
  const struct leme_public_value *current = row;
  for (size_t i = 0; i < count; ++i) {
    if (leme_public_kind(current) == LEME_PUBLIC_NULL) {
      *out = snapshot->null_value;
      return LEME_PUBLIC_OK;
    }
    if (schema->related != NULL) {
      status = resolve_reference(snapshot, schema, current, &current);
      if (status != LEME_PUBLIC_OK)
        return status;
      schema = schema->related;
    }
    if (leme_public_kind(current) != LEME_PUBLIC_OBJECT)
      return LEME_PUBLIC_TYPE_ERROR;
    status = leme_public_schema_path(schema, &path[i], 1, &field);
    if (status != LEME_PUBLIC_OK)
      return status;
    current = leme_public_get(current, path[i]);
    if (current == NULL)
      return LEME_PUBLIC_NOT_FOUND;
    schema = field->type;
    if (leme_public_kind(current) != LEME_PUBLIC_NULL || !field->nullable) {
      status = leme_public_schema_validate(schema, current);
      if (status != LEME_PUBLIC_OK)
        return status;
    }
  }
  *out = leme_public_kind(current) == LEME_PUBLIC_NULL ? snapshot->null_value
                                                       : current;
  return LEME_PUBLIC_OK;
}
