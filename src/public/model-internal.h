#ifndef LEME_PUBLIC_MODEL_INTERNAL_H
#define LEME_PUBLIC_MODEL_INTERNAL_H

#include "public/model.h"

#define LEME_PUBLIC_ENTITY_COUNT 4u

struct leme_public_index_entry {
  struct leme_public_text id;
  const struct leme_public_value *row;
};
struct leme_public_index {
  struct leme_public_index_entry *entries;
  size_t count;
};
struct leme_public_cached_root {
  size_t references;
  uint64_t generation;
  bool reference_free;
  struct leme_public_builder *builder;
  const struct leme_public_value *value;
};
struct leme_public_snapshot {
  size_t references;
  size_t bytes;
  struct leme_public_snapshot *backing;
  struct leme_public_cached_root *cached[LEME_PUBLIC_ROOT_COUNT];
  struct leme_public_builder *builder;
  uint32_t roots_mask;
  const struct leme_public_value *roots[LEME_PUBLIC_ROOT_COUNT];
  const struct leme_public_value *null_value;
  struct leme_public_index indexes[LEME_PUBLIC_ENTITY_COUNT];
  char instance[33];
  char revision[21];
};
struct leme_public_model {
  struct leme_public_allocator allocator;
  struct leme_public_budget *budget;
  struct leme_public_snapshot *baseline;
  struct leme_public_cached_root *cached[LEME_PUBLIC_ROOT_COUNT];
  struct leme_public_source source;
  leme_public_changed_fn notify;
  void *notify_context;
  size_t snapshot_limit;
  size_t total_limit;
  uint64_t serial;
  uint64_t order;
  uint64_t revision;
  bool available;
  bool dirty;
  bool locked;
  char instance[33];
};

void leme_public_cached_root_ref(struct leme_public_cached_root *root);
void leme_public_cached_root_unref(struct leme_public_cached_root *root);
void leme_public_model_clear_cache(struct leme_public_model *model,
                                   uint32_t roots);
void leme_public_model_sync_source(struct leme_public_model *model,
                                   const struct leme_public_source *source);
enum leme_public_status leme_public_model_cached_root(
    struct leme_public_model *model, const struct leme_public_source *source,
    enum leme_public_root root, const struct leme_public_work *work,
    struct leme_public_capture_diagnostic *diagnostic,
    struct leme_public_cached_root **out);
enum leme_public_status
leme_public_snapshot_project(struct leme_public_model *model, uint32_t roots,
                             struct leme_public_snapshot **out);

enum leme_public_status
leme_public_system_entropy(void *context, unsigned char *out, size_t count);
const char *leme_public_entity_name(enum leme_public_entity kind);
bool leme_public_parse_id(const struct leme_public_model *model,
                          enum leme_public_entity kind,
                          struct leme_public_text text,
                          struct leme_public_id *out, uint16_t *tag_number);

#endif
