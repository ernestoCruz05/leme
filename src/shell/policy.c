#include "shell/policy.h"

#include "config/config.h"
#include "core/server.h"
#include "output/output.h"
#include "shell/rules.h"
#include "shell/view.h"
#include "workspace/tag.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>

static int
leme_view_policy_saturate(int64_t value)
{
    if (value > INT_MAX) {
        return INT_MAX;
    }
    if (value < INT_MIN) {
        return INT_MIN;
    }
    return (int)value;
}

bool
leme_view_policy_fixed_size(int min_width, int min_height,
    int max_width, int max_height)
{
    return min_width > 0 && min_height > 0 &&
        max_width == min_width && max_height == min_height;
}

struct leme_view_destination
leme_view_policy_destination(
    const struct leme_server *server, const struct leme_view *parent)
{
    if (parent != NULL && parent->mapped && !parent->unmanaged &&
            leme_ownership_tag(parent) != NULL && leme_ownership_tag(parent)->owner != NULL) {
        return (struct leme_view_destination){
            .tags = leme_ownership_tag(parent)->owner,
            .tag_id = leme_ownership_tag(parent)->id,
        };
    }
    if (server == NULL || server->focused_output == NULL) {
        return (struct leme_view_destination){0};
    }
    return (struct leme_view_destination){
        .tags = &server->focused_output->tags,
        .tag_id = server->focused_output->tags.focused_id,
    };
}

void
leme_view_policy_apply_rules(struct leme_server *server,
    const struct leme_view_rules *rules,
    struct leme_view_destination *destination, bool *floating,
    bool parent_destination)
{
    struct leme_output *output;

    if (server == NULL || rules == NULL || destination == NULL) {
        return;
    }
    if (!parent_destination &&
            (rules->fields & LEME_WINDOW_RULE_OUTPUT) != 0) {
        output = leme_output_by_name(server, rules->output);
        if (output != NULL) {
            destination->tags = leme_output_tags(output);
            destination->tag_id = destination->tags->focused_id;
        }
    }
    if (!parent_destination &&
            (rules->fields & LEME_WINDOW_RULE_TAG) != 0 &&
            destination->tags != NULL &&
            rules->tag_id >= 1 &&
            rules->tag_id <= destination->tags->max_tags) {
        destination->tag_id = rules->tag_id;
    }
    if ((rules->fields & LEME_WINDOW_RULE_FLOATING) != 0 &&
            floating != NULL) {
        *floating = rules->floating;
    }
}

struct leme_box
leme_view_policy_clamp_box(struct leme_box box, struct leme_box area)
{
    int64_t right;
    int64_t bottom;

    if (area.width < 1 || area.height < 1) {
        return box;
    }
    if (box.width < 1) {
        box.width = 1;
    } else if (box.width > area.width) {
        box.width = area.width;
    }
    if (box.height < 1) {
        box.height = 1;
    } else if (box.height > area.height) {
        box.height = area.height;
    }
    right = (int64_t)area.x + area.width;
    bottom = (int64_t)area.y + area.height;
    if (box.x < area.x) {
        box.x = area.x;
    } else if ((int64_t)box.x + box.width > right) {
        box.x = leme_view_policy_saturate(right - box.width);
    }
    if (box.y < area.y) {
        box.y = area.y;
    } else if ((int64_t)box.y + box.height > bottom) {
        box.y = leme_view_policy_saturate(bottom - box.height);
    }
    return box;
}

struct leme_box
leme_view_policy_reanchor_box(struct leme_box box,
    struct leme_box from, struct leme_box to)
{
    double x_fraction = 0.0;
    double y_fraction = 0.0;

    if (from.width > 0) {
        x_fraction = (double)((int64_t)box.x - from.x) /
            (double)from.width;
    }
    if (from.height > 0) {
        y_fraction = (double)((int64_t)box.y - from.y) /
            (double)from.height;
    }
    if (!isfinite(x_fraction)) {
        x_fraction = 0.0;
    }
    if (!isfinite(y_fraction)) {
        y_fraction = 0.0;
    }
    box.x = leme_view_policy_saturate((int64_t)to.x +
        (int64_t)llround(x_fraction * (double)to.width));
    box.y = leme_view_policy_saturate((int64_t)to.y +
        (int64_t)llround(y_fraction * (double)to.height));
    return leme_view_policy_clamp_box(box, to);
}

struct leme_box
leme_view_policy_center_box(struct leme_box box,
    struct leme_box anchor, struct leme_box area)
{
    if (area.width < 1 || area.height < 1) {
        return box;
    }
    if (anchor.width < 1 || anchor.height < 1) {
        anchor = area;
    }
    if (box.width < 1) {
        box.width = 1;
    } else if (box.width > area.width) {
        box.width = area.width;
    }
    if (box.height < 1) {
        box.height = 1;
    } else if (box.height > area.height) {
        box.height = area.height;
    }
    box.x = leme_view_policy_saturate((int64_t)anchor.x +
        ((int64_t)anchor.width - box.width) / 2);
    box.y = leme_view_policy_saturate((int64_t)anchor.y +
        ((int64_t)anchor.height - box.height) / 2);
    return leme_view_policy_clamp_box(box, area);
}

struct leme_box
leme_view_policy_requested_box(struct leme_box requested,
    struct leme_box area, bool unmanaged)
{
    return unmanaged ? requested :
        leme_view_policy_clamp_box(requested, area);
}

bool
leme_view_policy_hover_focus(const struct leme_view *view)
{
    return view != NULL && !view->unmanaged;
}
