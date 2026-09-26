#ifndef TIMAO_HEAP_H
#define TIMAO_HEAP_H

#include "timao/limits.h"
#include "public/budget.h"

struct timao_heap_object;
struct timao_heap_root {
  struct timao_heap_root *next;
  struct timao_heap_object *object;
};

struct timao_heap {
  struct leme_public_budget *account;
  struct timao_heap_object *objects;
  struct timao_heap_root *roots;
  size_t bytes;
  size_t maximum;
};

typedef struct timao_heap_object *(*timao_heap_edge_fn)(const void *value,
                                                        size_t index);
typedef void (*timao_heap_destroy_fn)(void *value);

void *timao_memory_alloc(struct leme_public_budget *account, size_t bytes,
                         struct timao_diagnostic *error);
void timao_memory_free(void *allocation);
enum timao_status timao_heap_alloc(struct timao_heap *heap, size_t size,
                                   size_t edges, timao_heap_edge_fn edge,
                                   timao_heap_destroy_fn destroy,
                                   struct timao_heap_object **out,
                                   struct timao_diagnostic *error);
void *timao_heap_data(struct timao_heap_object *object);
bool timao_heap_owns(const struct timao_heap *heap,
                     const struct timao_heap_object *object);
void timao_heap_root_add(struct timao_heap *heap, struct timao_heap_root *root,
                         struct timao_heap_object *object);
void timao_heap_root_remove(struct timao_heap *heap,
                            struct timao_heap_root *root);
enum timao_status timao_heap_collect(struct timao_heap *heap,
                                     struct timao_meter *meter,
                                     struct timao_diagnostic *error);
void timao_heap_finish(struct timao_heap *heap);

#endif
