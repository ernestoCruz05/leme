#ifndef LEME_PUBLIC_MODEL_H
#define LEME_PUBLIC_MODEL_H

#include "public/budget.h"
#include "public/schema.h"

#define LEME_PUBLIC_API_VERSION 1u
#define LEME_PUBLIC_SNAPSHOT_BYTES ((size_t)33554432)
#define LEME_PUBLIC_TOTAL_BYTES ((size_t)67108864)

struct leme_public_model;
struct leme_public_snapshot;
struct leme_public_capture_diagnostic {
  const char *stage;
  enum leme_public_root root;
};
enum leme_public_change {
  LEME_PUBLIC_CHANGED,
  LEME_PUBLIC_LOCKED_CHANGED,
  LEME_PUBLIC_UNLOCKED_CHANGED,
  LEME_PUBLIC_DISABLED
};
struct leme_public_options {
  size_t snapshot_limit;
  size_t total_limit;
  struct leme_public_allocator allocator;
  void *entropy_context;
  enum leme_public_status (*entropy)(void *context, unsigned char *out,
                                     size_t count);
};
struct leme_public_source {
  void *context;
  bool (*locked)(void *context);
  enum leme_public_status (*root)(void *context,
                                  const struct leme_public_model *model,
                                  struct leme_public_builder *b,
                                  enum leme_public_root root,
                                  struct leme_public_value **out);
  bool cache_snapshots;
  uint64_t (*root_generation)(void *context, enum leme_public_root root);
};

enum leme_public_status
leme_public_model_create(const struct leme_public_options *options,
                         struct leme_public_model **out);
void leme_public_model_destroy(struct leme_public_model *model);
struct leme_public_budget *
leme_public_model_budget_ref(struct leme_public_model *model);
enum leme_public_status
leme_public_model_builder_create(struct leme_public_model *model,
                                 size_t maximum_bytes,
                                 struct leme_public_builder **out);
void leme_public_model_disable(struct leme_public_model *model);
bool leme_public_model_available(const struct leme_public_model *model);
struct leme_public_text
leme_public_model_instance(const struct leme_public_model *model);
enum leme_public_status
leme_public_model_issue_id(struct leme_public_model *model,
                           struct leme_public_id *out);
enum leme_public_status
leme_public_model_next_order(struct leme_public_model *model, uint64_t *out);
enum leme_public_status
leme_public_id_value(struct leme_public_builder *b,
                     const struct leme_public_model *model,
                     enum leme_public_entity kind, struct leme_public_id id,
                     uint16_t tag_number, struct leme_public_value **out);
enum leme_public_status
leme_public_ref_value(struct leme_public_builder *b,
                      const struct leme_public_model *model,
                      enum leme_public_entity kind, struct leme_public_id id,
                      uint16_t tag_number, struct leme_public_value **out);
typedef void (*leme_public_changed_fn)(void *, enum leme_public_change);
void leme_public_model_set_observer(struct leme_public_model *model,
                                    leme_public_changed_fn notify,
                                    void *context);
void leme_public_model_invalidate(struct leme_public_model *model);
void leme_public_model_lock_changed(struct leme_public_model *model,
                                    bool locked);
enum leme_public_status leme_public_model_capture(
    struct leme_public_model *model, const struct leme_public_source *source,
    uint32_t requested_roots, struct leme_public_snapshot **out);
enum leme_public_status leme_public_model_capture_work(
    struct leme_public_model *model, const struct leme_public_source *source,
    uint32_t roots, const struct leme_public_work *work,
    struct leme_public_snapshot **out);
enum leme_public_status leme_public_model_capture_work_diagnostic(
    struct leme_public_model *model, const struct leme_public_source *source,
    uint32_t roots, const struct leme_public_work *work,
    struct leme_public_capture_diagnostic *diagnostic,
    struct leme_public_snapshot **out);
void leme_public_snapshot_ref(struct leme_public_snapshot *snapshot);
void leme_public_snapshot_unref(struct leme_public_snapshot *snapshot);
const struct leme_public_value *
leme_public_snapshot_root(const struct leme_public_snapshot *snapshot,
                          enum leme_public_root root);
struct leme_public_text
leme_public_snapshot_instance(const struct leme_public_snapshot *snapshot);
struct leme_public_text
leme_public_snapshot_revision(const struct leme_public_snapshot *snapshot);
enum leme_public_status leme_public_snapshot_find(
    const struct leme_public_snapshot *snapshot, enum leme_public_entity kind,
    struct leme_public_text id, const struct leme_public_value **out);
enum leme_public_status
leme_public_snapshot_field(const struct leme_public_snapshot *snapshot,
                           const struct leme_public_schema *schema,
                           const struct leme_public_value *row,
                           const struct leme_public_text *path, size_t count,
                           const struct leme_public_value **out);

#endif
