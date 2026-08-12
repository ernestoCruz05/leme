#ifndef LEME_PLACEMENT_H
#define LEME_PLACEMENT_H

#include <stdbool.h>
#include <stddef.h>

enum leme_output_relation {
    LEME_OUTPUT_RELATION_NONE,
    LEME_OUTPUT_RELATION_LEFT_OF,
    LEME_OUTPUT_RELATION_RIGHT_OF,
    LEME_OUTPUT_RELATION_TOP_OF,
    LEME_OUTPUT_RELATION_BOTTOM_OF,
};

struct leme_placement_request {
    const char *name;
    int width;
    int height;
    int x;
    int y;
    const char *relative_to;
    enum leme_output_relation relation;
    bool has_position;
};

struct leme_placement {
    int x;
    int y;
    bool placed;
};

void leme_placement_resolve(const struct leme_placement_request *requests,
    size_t count, struct leme_placement *placements);

#endif
