#ifndef LEME_PUBLIC_BUDGET_H
#define LEME_PUBLIC_BUDGET_H

#include "public/value.h"

struct leme_public_budget;
enum leme_public_status
leme_public_budget_create(size_t maximum_bytes,
                          const struct leme_public_allocator *allocator,
                          struct leme_public_budget **out);
void leme_public_budget_ref(struct leme_public_budget *budget);
void leme_public_budget_unref(struct leme_public_budget *budget);
enum leme_public_status
leme_public_budget_reserve(struct leme_public_budget *budget, size_t bytes);
void leme_public_budget_release(struct leme_public_budget *budget,
                                size_t bytes);
enum leme_public_status
leme_public_budget_child(struct leme_public_budget *parent, size_t maximum,
                         struct leme_public_budget **out);
enum leme_public_status
leme_public_budget_transfer(struct leme_public_budget *from,
                            struct leme_public_budget *to, size_t bytes);
enum leme_public_status
leme_public_builder_create_budget(struct leme_public_budget *budget,
                                  size_t maximum,
                                  struct leme_public_builder **out);

#endif
