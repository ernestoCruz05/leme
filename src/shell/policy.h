#ifndef LEME_SHELL_POLICY_H
#define LEME_SHELL_POLICY_H

#include "core/leme.h"

#include <stdbool.h>

struct leme_server;
struct leme_tags;
struct leme_view;
struct leme_view_rules;

bool leme_view_policy_fixed_size(int min_width, int min_height,
    int max_width, int max_height);

struct leme_view_destination {
    struct leme_tags *tags;
    uint16_t tag_id;
};

struct leme_view_destination leme_view_policy_destination(
    const struct leme_server *server, const struct leme_view *parent);
void leme_view_policy_apply_rules(struct leme_server *server,
    const struct leme_view_rules *rules,
    struct leme_view_destination *destination, bool *floating,
    bool parent_destination);
struct leme_box leme_view_policy_clamp_box(
    struct leme_box box, struct leme_box area);
struct leme_box leme_view_policy_reanchor_box(struct leme_box box,
    struct leme_box from, struct leme_box to);
struct leme_box leme_view_policy_center_box(struct leme_box box,
    struct leme_box anchor, struct leme_box area);
struct leme_box leme_view_policy_requested_box(struct leme_box requested,
    struct leme_box area, bool unmanaged);
bool leme_view_policy_hover_focus(const struct leme_view *view);

#endif
