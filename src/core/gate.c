#include "core/gate.h"

#include "shell/sticky.h"

bool (*leme_gate_ownership_prepare)(const struct leme_view *);
bool (*leme_gate_tags_prepare)(enum leme_tags_prepare_checkpoint,
    const struct leme_view *, const struct leme_tags *);
bool (*leme_gate_capture_accept)(void);
bool (*leme_gate_output_reconcile)(void);
bool (*leme_gate_sticky_output_prepare)(
    enum leme_sticky_output_prepare_checkpoint);
bool (*leme_gate_sticky_unmap_prepare)(
    enum leme_sticky_unmap_prepare_checkpoint);
