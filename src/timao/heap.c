#include "timao/heap.h"
#include "timao/diagnostic.h"

#include <stdalign.h>
#include <stdint.h>
#include <stdlib.h>

struct memory_header {
  struct leme_public_budget *account;
  size_t bytes;
  max_align_t alignment;
};

struct timao_heap_object {
  struct timao_heap_object *next;
  struct timao_heap_object *gray_next;
  struct timao_heap *owner;
  size_t bytes;
  size_t edges;
  timao_heap_edge_fn edge;
  timao_heap_destroy_fn destroy;
  bool marked;
  max_align_t alignment;
};

void *timao_memory_alloc(struct leme_public_budget *account, size_t bytes,
                         struct timao_diagnostic *error) {
  if (account == NULL || bytes == 0) {
    timao_error(error, "invalid_argument", "invalid allocation request");
    return NULL;
  }
  if (bytes > SIZE_MAX - sizeof(struct memory_header)) {
    timao_error(error, "resource_limit", "allocation size overflow");
    return NULL;
  }
  const size_t total = sizeof(struct memory_header) + bytes;
  if (leme_public_budget_reserve(account, total) != LEME_PUBLIC_OK) {
    timao_error(error, "resource_limit", "interpreter memory limit exceeded");
    return NULL;
  }
  struct memory_header *header = calloc(1, total);
  if (header == NULL) {
    leme_public_budget_release(account, total);
    timao_error(error, "out_of_memory", "allocation failed");
    return NULL;
  }
  leme_public_budget_ref(account);
  header->account = account;
  header->bytes = total;
  return header + 1;
}

void timao_memory_free(void *allocation) {
  if (allocation == NULL)
    return;
  struct memory_header *header = (struct memory_header *)allocation - 1;
  struct leme_public_budget *account = header->account;
  const size_t bytes = header->bytes;
  free(header);
  leme_public_budget_release(account, bytes);
  leme_public_budget_unref(account);
}

enum timao_status timao_heap_alloc(struct timao_heap *heap, size_t size,
                                   size_t edges, timao_heap_edge_fn edge,
                                   timao_heap_destroy_fn destroy,
                                   struct timao_heap_object **out,
                                   struct timao_diagnostic *error) {
  if (out == NULL)
    return timao_error(error, "invalid_argument", "missing allocation output");
  *out = NULL;
  if (heap == NULL || (edges != 0 && edge == NULL))
    return timao_error(error, "invalid_argument", "invalid heap allocation");
  if (size > SIZE_MAX - sizeof(struct timao_heap_object) -
                 sizeof(struct memory_header))
    return timao_error(error, "resource_limit",
                       "heap allocation size overflow");
  const size_t payload = sizeof(struct timao_heap_object) + size;
  const size_t total = payload + sizeof(struct memory_header);
  if (total > heap->maximum - heap->bytes)
    return timao_error(error, "resource_limit", "heap memory limit exceeded");
  struct timao_heap_object *object =
      timao_memory_alloc(heap->account, payload, error);
  if (object == NULL)
    return TIMAO_ERROR;
  object->owner = heap;
  object->bytes = total;
  object->edges = edges;
  object->edge = edge;
  object->destroy = destroy;
  object->next = heap->objects;
  heap->objects = object;
  heap->bytes += total;
  *out = object;
  return TIMAO_OK;
}

void *timao_heap_data(struct timao_heap_object *object) {
  return object == NULL ? NULL : object + 1;
}

bool timao_heap_owns(const struct timao_heap *heap,
                     const struct timao_heap_object *object) {
  return object != NULL && object->owner == heap;
}

void timao_heap_root_add(struct timao_heap *heap, struct timao_heap_root *root,
                         struct timao_heap_object *object) {
  *root = (struct timao_heap_root){.next = heap->roots, .object = object};
  heap->roots = root;
}

void timao_heap_root_remove(struct timao_heap *heap,
                            struct timao_heap_root *root) {
  struct timao_heap_root **slot = &heap->roots;
  while (*slot != NULL && *slot != root)
    slot = &(*slot)->next;
  if (*slot != NULL) {
    *slot = root->next;
    *root = (struct timao_heap_root){0};
  }
}

static void mark(struct timao_heap_object *object,
                 struct timao_heap_object **gray) {
  if (object == NULL || object->marked)
    return;
  object->marked = true;
  object->gray_next = *gray;
  *gray = object;
}

static void release_object(struct timao_heap *heap,
                           struct timao_heap_object *object) {
  if (object->destroy != NULL)
    object->destroy(timao_heap_data(object));
  heap->bytes -= object->bytes;
  timao_memory_free(object);
}

enum timao_status timao_heap_collect(struct timao_heap *heap,
                                     struct timao_meter *meter,
                                     struct timao_diagnostic *error) {
  if (heap == NULL)
    return timao_error(error, "invalid_argument", "missing heap");
  for (struct timao_heap_object *object = heap->objects; object != NULL;
       object = object->next) {
    if (timao_charge(meter, 1, error) != TIMAO_OK)
      return TIMAO_ERROR;
    object->marked = false;
    object->gray_next = NULL;
  }
  struct timao_heap_object *gray = NULL;
  for (const struct timao_heap_root *root = heap->roots; root != NULL;
       root = root->next) {
    if (timao_charge(meter, 1, error) != TIMAO_OK)
      return TIMAO_ERROR;
    mark(root->object, &gray);
  }
  while (gray != NULL) {
    if (timao_charge(meter, 1, error) != TIMAO_OK)
      return TIMAO_ERROR;
    struct timao_heap_object *object = gray;
    gray = object->gray_next;
    for (size_t i = 0; i < object->edges; ++i) {
      if (timao_charge(meter, 1, error) != TIMAO_OK)
        return TIMAO_ERROR;
      mark(object->edge(timao_heap_data(object), i), &gray);
    }
  }
  struct timao_heap_object **slot = &heap->objects;
  while (*slot != NULL) {
    if (timao_charge(meter, 1, error) != TIMAO_OK)
      return TIMAO_ERROR;
    struct timao_heap_object *object = *slot;
    if (object->marked) {
      slot = &object->next;
    } else {
      *slot = object->next;
      release_object(heap, object);
    }
  }
  return TIMAO_OK;
}

void timao_heap_finish(struct timao_heap *heap) {
  while (heap->objects != NULL) {
    struct timao_heap_object *object = heap->objects;
    heap->objects = object->next;
    release_object(heap, object);
  }
  heap->roots = NULL;
}
