#ifndef LEME_SHELL_CONTAINER_POLICY_H
#define LEME_SHELL_CONTAINER_POLICY_H

#include "shell/placement.h"
#include "workspace/layout.h"

#define LEME_CONTAINER_EMPTY_TITLE "Container"
#define LEME_CONTAINER_DROP_EDGE_PX 48
#define LEME_CONTAINER_SPLIT_RATIO 0.5
#define LEME_CONTAINER_GAP 0

enum leme_container_resolution {
  LEME_CONTAINER_KEEP,
  LEME_CONTAINER_UNWRAP,
  LEME_CONTAINER_DESTROY,
};

bool leme_container_drop_at(struct leme_box content, double lx, double ly,
                            struct leme_layout_drop *drop);
enum leme_container_resolution leme_container_resolve_count(size_t count);
size_t leme_container_next_focus(const bool *eligible, size_t count,
                                 size_t removed);

#endif
