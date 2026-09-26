#include "public/value-internal.h"
#include "public/budget.h"

#include <stdalign.h>
#include <stdlib.h>
#include <string.h>

struct leme_public_chunk {
  struct leme_public_chunk *next;
  size_t used;
  size_t capacity;
  alignas(max_align_t) unsigned char data[];
};

static void *default_allocate(void *context, size_t bytes) {
  (void)context;
  return malloc(bytes);
}

static void default_release(void *context, void *allocation) {
  (void)context;
  free(allocation);
}

struct leme_public_allocator leme_public_default_allocator(void) {
  return (struct leme_public_allocator){.allocate = default_allocate,
                                        .release = default_release};
}

enum leme_public_status leme_public_fail(struct leme_public_builder *b,
                                         enum leme_public_status status) {
  if (b == NULL)
    return LEME_PUBLIC_INVALID;
  if (b->status == LEME_PUBLIC_OK)
    b->status = status;
  return b->status;
}

enum leme_public_status leme_public_mutable(struct leme_public_builder *b) {
  if (b == NULL)
    return LEME_PUBLIC_INVALID;
  if (b->sealed)
    return leme_public_fail(b, LEME_PUBLIC_INVALID);
  return b->status;
}

enum leme_public_status
leme_public_builder_create(size_t maximum_bytes,
                           const struct leme_public_allocator *allocator,
                           struct leme_public_builder **out) {
  return leme_public_builder_with_budget(maximum_bytes, NULL, allocator, out);
}

enum leme_public_status
leme_public_builder_create_budget(struct leme_public_budget *budget,
                                  size_t maximum,
                                  struct leme_public_builder **out) {
  return leme_public_builder_with_budget(maximum, budget, NULL, out);
}

enum leme_public_status
leme_public_builder_with_budget(size_t maximum_bytes,
                                struct leme_public_budget *budget,
                                const struct leme_public_allocator *allocator,
                                struct leme_public_builder **out) {
  if (out == NULL)
    return LEME_PUBLIC_INVALID;
  *out = NULL;
  const struct leme_public_allocator selected =
      allocator == NULL ? leme_public_default_allocator() : *allocator;
  if (selected.allocate == NULL || selected.release == NULL)
    return LEME_PUBLIC_INVALID;
  if (maximum_bytes < sizeof(struct leme_public_builder))
    return LEME_PUBLIC_LIMIT;
  if (budget != NULL) {
    const enum leme_public_status status =
        leme_public_budget_reserve(budget, sizeof(struct leme_public_builder));
    if (status != LEME_PUBLIC_OK)
      return status;
  }
  struct leme_public_builder *b =
      selected.allocate(selected.context, sizeof(*b));
  if (b == NULL) {
    leme_public_budget_release(budget, sizeof(*b));
    return LEME_PUBLIC_OOM;
  }
  leme_public_budget_ref(budget);
  *b = (struct leme_public_builder){.maximum = maximum_bytes,
                                    .bytes = sizeof(*b),
                                    .allocator = selected,
                                    .budget = budget};
  *out = b;
  return LEME_PUBLIC_OK;
}

void leme_public_builder_destroy(struct leme_public_builder *b) {
  if (b == NULL)
    return;
  struct leme_public_chunk *chunk = b->chunks;
  while (chunk != NULL) {
    struct leme_public_chunk *next = chunk->next;
    b->allocator.release(b->allocator.context, chunk);
    chunk = next;
  }
  const struct leme_public_allocator allocator = b->allocator;
  struct leme_public_budget *budget = b->budget;
  const size_t bytes = b->bytes;
  allocator.release(allocator.context, b);
  leme_public_budget_release(budget, bytes);
  leme_public_budget_unref(budget);
}

size_t leme_public_builder_bytes(const struct leme_public_builder *b) {
  return b == NULL ? 0 : b->bytes;
}

enum leme_public_status
leme_public_builder_status(const struct leme_public_builder *b) {
  return b == NULL ? LEME_PUBLIC_INVALID : b->status;
}

void *leme_public_allocate(struct leme_public_builder *b, size_t count,
                           size_t size, size_t alignment) {
  if (leme_public_mutable(b) != LEME_PUBLIC_OK)
    return NULL;
  if (size == 0 || count == 0 || alignment == 0 ||
      alignment > alignof(max_align_t) || (alignment & (alignment - 1)) != 0) {
    leme_public_fail(b, LEME_PUBLIC_INVALID);
    return NULL;
  }
  if (count > SIZE_MAX / size) {
    leme_public_fail(b, LEME_PUBLIC_LIMIT);
    return NULL;
  }
  const size_t bytes = count * size;
  struct leme_public_chunk *chunk = b->chunks;
  size_t padding = 0;
  if (chunk != NULL) {
    const size_t remainder = chunk->used % alignment;
    padding = remainder == 0 ? 0 : alignment - remainder;
  }
  if (chunk == NULL || padding > chunk->capacity - chunk->used ||
      bytes > chunk->capacity - chunk->used - padding) {
    const size_t available = b->maximum - b->bytes;
    if (sizeof(*chunk) > available || bytes > available - sizeof(*chunk)) {
      leme_public_fail(b, LEME_PUBLIC_LIMIT);
      return NULL;
    }
    size_t capacity = bytes > 4096 ? bytes : 4096;
    if (capacity > available - sizeof(*chunk))
      capacity = available - sizeof(*chunk);
    const size_t allocation = sizeof(*chunk) + capacity;
    if (b->budget != NULL) {
      const enum leme_public_status status =
          leme_public_budget_reserve(b->budget, allocation);
      if (status != LEME_PUBLIC_OK) {
        leme_public_fail(b, status);
        return NULL;
      }
    }
    chunk = b->allocator.allocate(b->allocator.context, allocation);
    if (chunk == NULL) {
      leme_public_budget_release(b->budget, allocation);
      leme_public_fail(b, LEME_PUBLIC_OOM);
      return NULL;
    }
    *chunk =
        (struct leme_public_chunk){.next = b->chunks, .capacity = capacity};
    b->chunks = chunk;
    b->bytes += allocation;
    padding = 0;
  }
  unsigned char *result = &chunk->data[chunk->used + padding];
  chunk->used += padding + bytes;
  memset(result, 0, bytes);
  return result;
}
