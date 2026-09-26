#include "control/memory.h"

#include <errno.h>
#include <stdalign.h>
#include <stdint.h>
#include <stdlib.h>

struct leme_control_header {
  struct leme_public_budget *account;
  size_t bytes;
  size_t total_reserved;
  max_align_t alignment_pad;
};

size_t leme_control_allocation_bytes(const void *allocation) {
  if (allocation == NULL)
    return 0;
  const struct leme_control_header *header = allocation;
  return (header - 1)->total_reserved;
}

size_t leme_control_allocation_overhead(void) {
  return sizeof(struct leme_control_header);
}

void *leme_control_alloc(struct leme_public_budget *account, size_t bytes) {
  if (account == NULL || bytes == 0) {
    errno = EINVAL;
    return NULL;
  }
  if (bytes > SIZE_MAX - sizeof(struct leme_control_header)) {
    errno = EOVERFLOW;
    return NULL;
  }
  const size_t total_bytes = sizeof(struct leme_control_header) + bytes;
  const enum leme_public_status status =
      leme_public_budget_reserve(account, total_bytes);
  if (status == LEME_PUBLIC_LIMIT) {
    errno = ENOSPC;
    return NULL;
  }
  if (status != LEME_PUBLIC_OK) {
    errno = EINVAL;
    return NULL;
  }
  struct leme_control_header *header = malloc(total_bytes);
  if (header == NULL) {
    leme_public_budget_release(account, total_bytes);
    errno = ENOMEM;
    return NULL;
  }
  leme_public_budget_ref(account);
  *header = (struct leme_control_header){
      .account = account,
      .bytes = bytes,
      .total_reserved = total_bytes,
  };
  return header + 1;
}

void *leme_control_realloc(void *allocation, size_t bytes) {
  if (allocation == NULL || bytes == 0) {
    errno = EINVAL;
    return NULL;
  }
  struct leme_control_header *header =
      (struct leme_control_header *)allocation - 1;
  struct leme_public_budget *account = header->account;
  if (bytes > SIZE_MAX - sizeof(struct leme_control_header)) {
    errno = EOVERFLOW;
    return NULL;
  }
  const size_t new_total = sizeof(struct leme_control_header) + bytes;
  const enum leme_public_status status =
      leme_public_budget_reserve(account, new_total);
  if (status == LEME_PUBLIC_LIMIT) {
    errno = ENOSPC;
    return NULL;
  }
  if (status != LEME_PUBLIC_OK) {
    errno = EINVAL;
    return NULL;
  }
  struct leme_control_header *new_header = realloc(header, new_total);
  if (new_header == NULL) {
    leme_public_budget_release(account, new_total);
    errno = ENOMEM;
    return NULL;
  }
  const size_t old_total = new_header->total_reserved;
  leme_public_budget_release(account, old_total);
  new_header->bytes = bytes;
  new_header->total_reserved = new_total;
  return new_header + 1;
}

void leme_control_free(void *allocation) {
  if (allocation == NULL)
    return;
  struct leme_control_header *header =
      (struct leme_control_header *)allocation - 1;
  struct leme_public_budget *account = header->account;
  const size_t total = header->total_reserved;
  free(header);
  leme_public_budget_release(account, total);
  leme_public_budget_unref(account);
}

enum leme_public_status
leme_control_rehome(void *allocation, struct leme_public_budget *account) {
  if (allocation == NULL || account == NULL)
    return LEME_PUBLIC_INVALID;
  struct leme_control_header *header =
      (struct leme_control_header *)allocation - 1;
  if (header->account == account)
    return LEME_PUBLIC_OK;
  const enum leme_public_status status = leme_public_budget_transfer(
      header->account, account, header->total_reserved);
  if (status != LEME_PUBLIC_OK)
    return status;
  leme_public_budget_ref(account);
  leme_public_budget_unref(header->account);
  header->account = account;
  return LEME_PUBLIC_OK;
}
