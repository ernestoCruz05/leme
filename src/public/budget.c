#include "public/budget.h"
#include "public/value-internal.h"

#include <stdlib.h>

struct leme_public_budget {
  size_t maximum;
  size_t used;
  size_t references;
  struct leme_public_allocator allocator;
  struct leme_public_budget *parent;
  struct leme_public_budget *root;
  size_t depth;
};

enum leme_public_status
leme_public_budget_create(size_t maximum_bytes,
                          const struct leme_public_allocator *allocator,
                          struct leme_public_budget **out) {
  if (out == NULL)
    return LEME_PUBLIC_INVALID;
  *out = NULL;
  if (maximum_bytes < sizeof(struct leme_public_budget))
    return LEME_PUBLIC_LIMIT;
  const struct leme_public_allocator selected =
      allocator == NULL ? leme_public_default_allocator() : *allocator;
  if (selected.allocate == NULL || selected.release == NULL)
    return LEME_PUBLIC_INVALID;
  struct leme_public_budget *budget =
      selected.allocate(selected.context, sizeof(*budget));
  if (budget == NULL)
    return LEME_PUBLIC_OOM;
  *budget = (struct leme_public_budget){.maximum = maximum_bytes,
                                        .used = sizeof(*budget),
                                        .references = 1,
                                        .allocator = selected,
                                        .parent = NULL,
                                        .root = budget,
                                        .depth = 0};
  *out = budget;
  return LEME_PUBLIC_OK;
}

void leme_public_budget_ref(struct leme_public_budget *budget) {
  if (budget == NULL)
    return;
  if (budget->references == 0 || budget->references == SIZE_MAX)
    abort();
  ++budget->references;
}

void leme_public_budget_unref(struct leme_public_budget *budget) {
  if (budget == NULL)
    return;
  if (budget->references == 0)
    abort();
  if (--budget->references != 0)
    return;
  const size_t min_used = budget->parent == NULL ? sizeof(*budget) : 0;
  if (budget->used != min_used)
    abort();
  const struct leme_public_allocator allocator = budget->allocator;
  struct leme_public_budget *parent = budget->parent;
  allocator.release(allocator.context, budget);
  if (parent != NULL) {
    leme_public_budget_release(parent, sizeof(struct leme_public_budget));
    leme_public_budget_unref(parent);
  }
}

enum leme_public_status
leme_public_budget_reserve(struct leme_public_budget *budget, size_t bytes) {
  if (budget == NULL)
    return LEME_PUBLIC_INVALID;
  for (struct leme_public_budget *cur = budget; cur != NULL; cur = cur->parent) {
    if (bytes > cur->maximum - cur->used)
      return LEME_PUBLIC_LIMIT;
  }
  for (struct leme_public_budget *cur = budget; cur != NULL; cur = cur->parent)
    cur->used += bytes;
  return LEME_PUBLIC_OK;
}

void leme_public_budget_release(struct leme_public_budget *budget,
                                size_t bytes) {
  if (budget == NULL)
    return;
  for (struct leme_public_budget *cur = budget; cur != NULL; cur = cur->parent) {
    const size_t min_used = cur->parent == NULL ? sizeof(*cur) : 0;
    if (bytes > cur->used - min_used)
      abort();
  }
  for (struct leme_public_budget *cur = budget; cur != NULL; cur = cur->parent)
    cur->used -= bytes;
}

enum leme_public_status
leme_public_budget_child(struct leme_public_budget *parent, size_t maximum,
                         struct leme_public_budget **out) {
  if (parent == NULL || out == NULL)
    return LEME_PUBLIC_INVALID;
  *out = NULL;
  if (parent->depth >= 3)
    return LEME_PUBLIC_LIMIT;
  const enum leme_public_status status =
      leme_public_budget_reserve(parent, sizeof(struct leme_public_budget));
  if (status != LEME_PUBLIC_OK)
    return status;
  struct leme_public_budget *child =
      parent->allocator.allocate(parent->allocator.context, sizeof(*child));
  if (child == NULL) {
    leme_public_budget_release(parent, sizeof(*child));
    return LEME_PUBLIC_OOM;
  }
  leme_public_budget_ref(parent);
  *child = (struct leme_public_budget){
      .maximum = maximum,
      .used = 0,
      .references = 1,
      .allocator = parent->allocator,
      .parent = parent,
      .root = parent->root,
      .depth = parent->depth + 1,
  };
  *out = child;
  return LEME_PUBLIC_OK;
}

enum leme_public_status
leme_public_budget_transfer(struct leme_public_budget *from,
                            struct leme_public_budget *to, size_t bytes) {
  if (from == NULL || to == NULL)
    return LEME_PUBLIC_INVALID;
  if (from->root != to->root)
    return LEME_PUBLIC_INVALID;
  const size_t from_min = from->parent == NULL ? sizeof(*from) : 0;
  if (bytes > from->used - from_min)
    return LEME_PUBLIC_INVALID;
  struct leme_public_budget *left = from;
  struct leme_public_budget *right = to;
  while (left != NULL && right != NULL && left->depth > right->depth)
    left = left->parent;
  while (left != NULL && right != NULL && right->depth > left->depth)
    right = right->parent;
  while (left != NULL && right != NULL && left != right) {
    left = left->parent;
    right = right->parent;
  }
  if (left == NULL || right == NULL)
    return LEME_PUBLIC_INVALID;
  struct leme_public_budget *common = left;
  for (struct leme_public_budget *cur = to; cur != common; cur = cur->parent) {
    if (bytes > cur->maximum - cur->used)
      return LEME_PUBLIC_LIMIT;
  }
  for (struct leme_public_budget *cur = from; cur != common; cur = cur->parent)
    cur->used -= bytes;
  for (struct leme_public_budget *cur = to; cur != common; cur = cur->parent)
    cur->used += bytes;
  return LEME_PUBLIC_OK;
}
