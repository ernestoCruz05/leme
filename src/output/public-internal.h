#ifndef LEME_OUTPUT_PUBLIC_INTERNAL_H
#define LEME_OUTPUT_PUBLIC_INTERNAL_H

#include "output/public.h"

struct leme_output;
struct leme_public_output_entry {
  const struct leme_output *output;
  uint64_t serial;
};
enum leme_public_status leme_public_outputs_ordered(
    struct leme_public_builder *b, const struct leme_server *server,
    struct leme_public_output_entry **out, size_t *count);

#endif
