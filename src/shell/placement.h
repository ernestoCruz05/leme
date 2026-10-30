#ifndef LEME_SHELL_PLACEMENT_H
#define LEME_SHELL_PLACEMENT_H

#include "core/leme.h"
#include "workspace/layout.h"

enum leme_view_placement {
  LEME_VIEW_PLACEMENT_NORMAL,
  LEME_VIEW_PLACEMENT_MAXIMIZED,
  LEME_VIEW_PLACEMENT_LEFT_HALF,
  LEME_VIEW_PLACEMENT_RIGHT_HALF,
  LEME_VIEW_PLACEMENT_TOP_LEFT,
  LEME_VIEW_PLACEMENT_TOP_RIGHT,
  LEME_VIEW_PLACEMENT_BOTTOM_LEFT,
  LEME_VIEW_PLACEMENT_BOTTOM_RIGHT,
};

struct leme_tag;
struct leme_output;

struct leme_placement_anchor {
  struct leme_box original;
  struct leme_box restored;
  struct leme_box frame;
  double start_x;
  double start_y;
  double cursor_x;
  double cursor_y;
};

struct leme_box
leme_placement_anchored_box(const struct leme_placement_anchor *anchor);
struct leme_box leme_placement_translate_box(struct leme_box box,
                                             struct leme_box from,
                                             struct leme_box to);
struct leme_box leme_placement_area(const struct leme_tag *tag,
                                    const struct leme_output *output);

struct leme_placement_state {
  enum leme_view_placement kind;
  bool restore_valid;
  struct leme_box restore_box;
};

bool leme_placement_set(struct leme_placement_state *state,
                        enum leme_view_placement kind,
                        struct leme_box normal_content);
void leme_placement_forget(struct leme_placement_state *state);
struct leme_box leme_placement_box(struct leme_box area,
                                   enum leme_view_placement placement);
enum leme_view_placement leme_placement_snap(enum leme_view_placement current,
                                             enum leme_direction direction);
enum leme_view_placement
leme_placement_at(struct leme_box full, double x, double y,
                  bool (*has_neighbour)(double x, double y, void *data),
                  void *data);

#endif
