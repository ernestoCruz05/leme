#include "core/gate.h"

bool (*leme_gate_tags_prepare)(enum leme_tags_prepare_checkpoint,
    const struct leme_view *, const struct leme_tags *);
bool (*leme_gate_capture_accept)(void);
bool (*leme_gate_output_reconcile)(void);
