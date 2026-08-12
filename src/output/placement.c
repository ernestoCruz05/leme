#include "output/placement.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

static bool
leme_placement_names_equal(const char *left, const char *right)
{
    return left != NULL && right != NULL && strcmp(left, right) == 0;
}

static int
leme_placement_clamp(int64_t value)
{
    if (value > INT_MAX) {
        return INT_MAX;
    }
    if (value < INT_MIN) {
        return INT_MIN;
    }
    return (int)value;
}

static int
leme_placement_add(int value, int delta)
{
    return leme_placement_clamp((int64_t)value + (int64_t)delta);
}

static int
leme_placement_subtract(int value, int delta)
{
    return leme_placement_clamp((int64_t)value - (int64_t)delta);
}

static void
leme_placement_apply_relation(const struct leme_placement_request *request,
    const struct leme_placement_request *reference,
    const struct leme_placement *anchor, struct leme_placement *placement)
{
    switch (request->relation) {
    case LEME_OUTPUT_RELATION_LEFT_OF:
        placement->x = leme_placement_subtract(anchor->x, request->width);
        placement->y = anchor->y;
        break;
    case LEME_OUTPUT_RELATION_RIGHT_OF:
        placement->x = leme_placement_add(anchor->x, reference->width);
        placement->y = anchor->y;
        break;
    case LEME_OUTPUT_RELATION_TOP_OF:
        placement->x = anchor->x;
        placement->y = leme_placement_subtract(anchor->y, request->height);
        break;
    case LEME_OUTPUT_RELATION_BOTTOM_OF:
        placement->x = anchor->x;
        placement->y = leme_placement_add(anchor->y, reference->height);
        break;
    case LEME_OUTPUT_RELATION_NONE:
        return;
    }
    placement->placed = true;
}

static bool
leme_placement_resolve_relative(
    const struct leme_placement_request *requests, size_t count,
    struct leme_placement *placements, size_t index)
{
    size_t reference;

    for (reference = 0; reference < count; reference++) {
        if (reference == index || !placements[reference].placed ||
                !leme_placement_names_equal(requests[index].relative_to,
                    requests[reference].name)) {
            continue;
        }
        leme_placement_apply_relation(&requests[index], &requests[reference],
            &placements[reference], &placements[index]);
        return placements[index].placed;
    }
    return false;
}

static int
leme_placement_right_edge(const struct leme_placement_request *requests,
    size_t count, const struct leme_placement *placements)
{
    int edge = 0;
    size_t index;

    for (index = 0; index < count; index++) {
        int candidate;

        if (!placements[index].placed) {
            continue;
        }
        candidate = leme_placement_add(
            placements[index].x, requests[index].width);
        if (candidate > edge) {
            edge = candidate;
        }
    }
    return edge;
}

void
leme_placement_resolve(const struct leme_placement_request *requests,
    size_t count, struct leme_placement *placements)
{
    size_t index;
    bool progress;

    for (index = 0; index < count; index++) {
        placements[index] = (struct leme_placement){0};
        if (requests[index].has_position) {
            placements[index].x = requests[index].x;
            placements[index].y = requests[index].y;
            placements[index].placed = true;
        }
    }

    do {
        progress = false;
        for (index = 0; index < count; index++) {
            if (placements[index].placed ||
                    requests[index].relation == LEME_OUTPUT_RELATION_NONE) {
                continue;
            }
            progress |= leme_placement_resolve_relative(
                requests, count, placements, index);
        }
    } while (progress);

    for (index = 0; index < count; index++) {
        if (placements[index].placed) {
            continue;
        }
        placements[index].x =
            leme_placement_right_edge(requests, count, placements);
        placements[index].y = 0;
        placements[index].placed = true;
    }
}
