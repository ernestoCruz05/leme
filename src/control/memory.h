#ifndef LEME_CONTROL_MEMORY_H
#define LEME_CONTROL_MEMORY_H

#include "public/budget.h"
#include <stddef.h>

void *leme_control_alloc(struct leme_public_budget *account, size_t bytes);
void *leme_control_realloc(void *allocation, size_t bytes);
void leme_control_free(void *allocation);
size_t leme_control_allocation_bytes(const void *allocation);
size_t leme_control_allocation_overhead(void);
enum leme_public_status leme_control_rehome(void *allocation,
                                            struct leme_public_budget *account);

#endif
