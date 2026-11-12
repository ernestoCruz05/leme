#ifndef LEME_SHELL_CONTAINER_POLICY_H
#define LEME_SHELL_CONTAINER_POLICY_H

#include "shell/placement.h"
#include "workspace/layout.h"

#define LEME_CONTAINER_EMPTY_TITLE "Container"
#define LEME_CONTAINER_DROP_EDGE_PX 48
#define LEME_CONTAINER_SPLIT_RATIO 0.5
#define LEME_CONTAINER_GAP 0

bool leme_container_drop_at(struct leme_box content, double lx, double ly,
                            struct leme_layout_drop *drop);

#endif
