#ifndef LEME_PUBLIC_IDENTITY_H
#define LEME_PUBLIC_IDENTITY_H

#include <stdbool.h>
#include <stdint.h>

struct leme_public_id {
  uint64_t serial;
};
struct leme_public_view_meta {
  struct leme_public_id id;
  bool ever_mapped;
  bool urgent;
  uint64_t last_focused;
  uint64_t urgent_since;
};
enum leme_public_entity {
  LEME_PUBLIC_VIEW,
  LEME_PUBLIC_TAG,
  LEME_PUBLIC_OUTPUT,
  LEME_PUBLIC_INPUT
};

#endif
