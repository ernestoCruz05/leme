#include "overview/overview.h"

#include "config/config.h"
#include "core/server.h"
#include "desktop/owner.h"
#include "desktop/pixels.h"
#include "input/input.h"
#include "output/output.h"
#include "protocols/desktop.h"
#include "render/animation.h"
#include "render/render.h"
#include "render/workspace_transition.h"
#include "shell/view.h"
#include "workspace/tag.h"

#include <linux/input-event-codes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#define LEME_OVERVIEW_COLUMNS 5
#define LEME_OVERVIEW_LABEL 28
#define LEME_OVERVIEW_RING 3
#define LEME_OVERVIEW_DRAG 8.0
#define LEME_OVERVIEW_DIM 0.6f
#define LEME_OVERVIEW_CARD 0.08f

struct leme_overview_slot {
  uint16_t id;
  struct leme_box box;
  struct leme_box shown;
  struct leme_box from;
  struct leme_box to;
  float alpha;
  float alpha_from;
  float alpha_to;
  struct wlr_scene_tree *frame;
  struct wlr_scene_rect *card;
  struct wl_array bases;
};

struct leme_overview {
  struct leme_output *output;
  struct wlr_scene_tree *tree;
  struct wlr_scene_tree *chrome;
  struct wlr_scene_tree *ring;
  struct wlr_scene_tree *motion;
  struct wlr_scene_rect *dim;
  struct wl_event_source *settled;
  struct leme_overview_slot slots[LEME_TAGS_RING_MAX];
  size_t count;
  size_t columns;
  size_t selected;
  size_t pressed;
  size_t lifted;
  size_t over;
  double press_x;
  double press_y;
  float dim_alpha;
  float dim_from;
  float dim_to;
  uint16_t picked;
  bool dragging;
  bool building;
  bool closing;
};

static const struct leme_desk_owner leme_overview_owner;

static void leme_overview_layout(struct leme_overview *overview) {
  const struct leme_box usable = leme_output_usable_box(overview->output);
  const struct leme_box full = leme_output_full_box(overview->output);
  const int count = (int)overview->count;
  const int rows = (count + LEME_OVERVIEW_COLUMNS - 1) / LEME_OVERVIEW_COLUMNS;
  const int columns = (count + rows - 1) / rows;
  const int margin = usable.width / 40 < 8 ? 8 : usable.width / 40;
  int width = (usable.width - margin * (columns + 1)) / columns;
  const int room =
      (usable.height - margin * (rows + 1)) / rows - LEME_OVERVIEW_LABEL;

  if (width > usable.width * 2 / 5) {
    width = usable.width * 2 / 5;
  }
  int height = (int)((int64_t)width * full.height / full.width);
  const int lane = (usable.height - LEME_OVERVIEW_LABEL - 16) * 20 / 67;
  const int limit = rows == 1 && lane < room ? lane : room;
  if (height > limit) {
    height = limit;
    width = (int)((int64_t)height * full.width / full.height);
  }
  if (width < 1) {
    width = 1;
  }
  if (height < 1) {
    height = 1;
  }
  const int grid = rows * (height + LEME_OVERVIEW_LABEL) + (rows - 1) * margin;

  overview->columns = (size_t)columns;
  for (int index = 0; index < count; index++) {
    const int row = index / columns;
    const int in_row = row == rows - 1 ? count - columns * (rows - 1) : columns;
    const int row_width = in_row * width + (in_row - 1) * margin;

    overview->slots[index].box = (struct leme_box){
        .x = usable.x + (usable.width - row_width) / 2 +
             (index % columns) * (width + margin),
        .y = usable.y + (usable.height - grid) / 2 +
             row * (height + LEME_OVERVIEW_LABEL + margin),
        .width = width,
        .height = height,
    };
  }
}

static void leme_overview_select(struct leme_overview *overview, size_t index) {
  const struct leme_config *config = overview->output->server->config;
  const struct leme_box box = overview->slots[index].box;
  const int gap = 2 * LEME_OVERVIEW_RING;
  const struct leme_box edges[] = {
      {box.x - gap, box.y - gap, box.width + 2 * gap, LEME_OVERVIEW_RING},
      {box.x - gap, box.y + box.height + gap - LEME_OVERVIEW_RING,
       box.width + 2 * gap, LEME_OVERVIEW_RING},
      {box.x - gap, box.y - gap, LEME_OVERVIEW_RING, box.height + 2 * gap},
      {box.x + box.width + gap - LEME_OVERVIEW_RING, box.y - gap,
       LEME_OVERVIEW_RING, box.height + 2 * gap},
  };

  overview->selected = index;
  if (overview->ring != NULL) {
    wlr_scene_node_destroy(&overview->ring->node);
  }
  overview->ring = wlr_scene_tree_create(overview->chrome);
  if (overview->ring == NULL) {
    return;
  }
  for (size_t edge = 0; edge < LEME_ARRAY_LENGTH(edges); edge++) {
    struct wlr_scene_rect *rect =
        wlr_scene_rect_create(overview->ring, edges[edge].width,
                              edges[edge].height, config->border_active);

    if (rect != NULL) {
      wlr_scene_node_set_position(&rect->node, edges[edge].x, edges[edge].y);
    }
  }
}

static void leme_overview_place(const struct leme_overview *overview,
                                struct leme_overview_slot *slot) {
  const struct leme_box full = leme_output_full_box(overview->output);
  const float shade = LEME_OVERVIEW_CARD * slot->alpha;
  const float card[4] = {shade, shade, shade, shade};

  wlr_scene_node_set_position(&slot->frame->node, slot->shown.x, slot->shown.y);
  wlr_scene_rect_set_size(slot->card, slot->shown.width, slot->shown.height);
  wlr_scene_rect_set_color(slot->card, card);
  leme_render_view_scale_nodes(
      &slot->bases, (double)slot->shown.width / (double)full.width,
      (double)slot->shown.height / (double)full.height);
  leme_render_view_fade_nodes(&slot->bases, slot->alpha);
}

static int leme_overview_mix(int from, int to, double progress) {
  return from + (int)lround((double)(to - from) * progress);
}

static void leme_overview_step(struct leme_overview *overview,
                               double progress) {
  const float fade = progress < 0.0   ? 0.0f
                     : progress > 1.0 ? 1.0f
                                      : (float)progress;

  for (size_t index = 0; index < overview->count; index++) {
    struct leme_overview_slot *slot = &overview->slots[index];

    slot->shown = (struct leme_box){
        leme_overview_mix(slot->from.x, slot->to.x, progress),
        leme_overview_mix(slot->from.y, slot->to.y, progress),
        leme_overview_mix(slot->from.width, slot->to.width, progress),
        leme_overview_mix(slot->from.height, slot->to.height, progress),
    };
    if (slot->shown.width < 1) {
      slot->shown.width = 1;
    }
    if (slot->shown.height < 1) {
      slot->shown.height = 1;
    }
    slot->alpha = slot->alpha_from + (slot->alpha_to - slot->alpha_from) * fade;
    leme_overview_place(overview, slot);
  }
  overview->dim_alpha =
      overview->dim_from + (overview->dim_to - overview->dim_from) * fade;
  wlr_scene_rect_set_color(
      overview->dim,
      (float[4]){0.0f, 0.0f, 0.0f, LEME_OVERVIEW_DIM * overview->dim_alpha});
}

static void leme_overview_stop(struct leme_overview *overview) {
  if (overview->motion == NULL) {
    return;
  }
  leme_animation_manager_cancel_data(&overview->output->server->animations,
                                     overview);
  wlr_scene_node_destroy(&overview->motion->node);
  overview->motion = NULL;
}

static void leme_overview_clear(struct leme_overview *overview) {
  leme_overview_stop(overview);
  for (size_t index = 0; index < overview->count; index++) {
    wl_array_release(&overview->slots[index].bases);
    wl_array_init(&overview->slots[index].bases);
  }
  overview->count = 0;
  if (overview->tree != NULL) {
    wlr_scene_node_destroy(&overview->tree->node);
  }
  overview->tree = NULL;
  overview->chrome = NULL;
  overview->ring = NULL;
  overview->dim = NULL;
}

static bool leme_overview_build(struct leme_overview *overview) {
  static const float text[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  struct leme_output *output = overview->output;
  struct leme_tags *tags = &output->tags;
  const struct leme_box full = leme_output_full_box(output);
  const uint16_t chosen = overview->count == 0
                              ? tags->focused_id
                              : overview->slots[overview->selected].id;
  uint16_t ids[LEME_TAGS_RING_MAX];

  leme_overview_clear(overview);
  overview->tree = wlr_scene_tree_create(output->server->scene_overview);
  if (overview->tree == NULL) {
    return false;
  }
  overview->dim =
      wlr_scene_rect_create(overview->tree, full.width, full.height,
                            (float[4]){0.0f, 0.0f, 0.0f, LEME_OVERVIEW_DIM});
  if (overview->dim == NULL) {
    return false;
  }
  wlr_scene_node_set_position(&overview->dim->node, full.x, full.y);
  overview->dim_alpha = 1.0f;
  overview->dim_to = 1.0f;
  overview->selected = 0;
  overview->lifted = SIZE_MAX;
  overview->dragging = false;
  const size_t count = leme_tags_navigable(tags, ids, LEME_ARRAY_LENGTH(ids));
  if (count == 0) {
    return false;
  }
  for (size_t index = 0; index < count; index++) {
    overview->slots[index].id = ids[index];
    overview->slots[index].frame = NULL;
    if (ids[index] == chosen) {
      overview->selected = index;
    }
  }
  overview->count = count;
  leme_overview_layout(overview);
  for (size_t index = 0; index < count; index++) {
    static const float card[4] = {LEME_OVERVIEW_CARD, LEME_OVERVIEW_CARD,
                                  LEME_OVERVIEW_CARD, LEME_OVERVIEW_CARD};
    struct leme_overview_slot *slot = &overview->slots[index];
    const struct leme_tag *tag = tags->table[slot->id];

    slot->frame = wlr_scene_tree_create(overview->tree);
    if (slot->frame == NULL ||
        !leme_desk_pixels_rect(
            slot->frame,
            (struct leme_box){0, 0, slot->box.width, slot->box.height}, card, 8,
            false)) {
      return false;
    }
    struct wlr_scene_node *first =
        wl_container_of(slot->frame->children.next, first, link);
    slot->card = wlr_scene_rect_from_node(first);
    struct wlr_scene_tree *copy = wlr_scene_tree_create(slot->frame);
    if (copy == NULL) {
      return false;
    }
    if (!leme_render_workspace_collect_tag(
            output, slot->id, copy, tag == NULL ? NULL : tag->focused_view,
            false)) {
      wlr_scene_node_destroy(&copy->node);
    } else if (!leme_render_view_collect_content(&slot->bases, copy, 0, 0)) {
      wl_array_release(&slot->bases);
      wl_array_init(&slot->bases);
      wlr_scene_node_destroy(&copy->node);
    }
    slot->shown = slot->box;
    slot->to = slot->box;
    slot->alpha = 1.0f;
    slot->alpha_to = 1.0f;
    leme_overview_place(overview, slot);
  }
  overview->chrome = wlr_scene_tree_create(overview->tree);
  if (overview->chrome == NULL) {
    return false;
  }
  for (size_t index = 0; index < count; index++) {
    const struct leme_overview_slot *slot = &overview->slots[index];
    char label[8];

    (void)snprintf(label, sizeof(label), "%u", slot->id);
    (void)leme_desk_pixels_place_line(
        overview->chrome, label, "Sans Bold 11", text,
        leme_output_scale(output),
        (struct leme_box){slot->box.x, slot->box.y + slot->box.height,
                          slot->box.width, LEME_OVERVIEW_LABEL},
        true);
  }
  leme_overview_select(overview, overview->selected);
  return true;
}

void leme_overview_close(struct leme_server *server) {
  struct leme_overview *overview = server->overview;

  if (overview == NULL) {
    return;
  }
  server->overview = NULL;
  leme_desk_owner_release(server, &leme_overview_owner);
  if (overview->settled != NULL) {
    wl_event_source_remove(overview->settled);
  }
  leme_overview_clear(overview);
  leme_render_workspace_refresh_views(overview->output);
  free(overview);
  leme_input_pointer_rebase(server);
}

static void leme_overview_handle_settled(void *data) {
  struct leme_server *server = data;
  struct leme_tags *tags = &server->overview->output->tags;
  const uint16_t id = server->overview->picked;

  server->overview->settled = NULL;
  leme_overview_close(server);
  if (id == tags->focused_id) {
    return;
  }
  (void)leme_tags_focus_id(tags, id);
  leme_render_workspace_transition_finish(tags->output);
  leme_view_refresh_tag_focus(server);
}

static void
leme_overview_motion_apply(void *data,
                           const struct leme_animation_frame *frame) {
  leme_overview_step(data, frame->scalar);
}

static void leme_overview_motion_done(void *data) {
  struct leme_overview *overview = data;
  struct leme_server *server = overview->output->server;

  overview->motion = NULL;
  leme_overview_step(overview, 1.0);
  if (!overview->closing) {
    wlr_scene_node_set_enabled(&overview->chrome->node, true);
  } else if (overview->settled == NULL) {
    overview->settled =
        wl_event_loop_add_idle(wl_display_get_event_loop(server->display),
                               leme_overview_handle_settled, server);
  }
}

static void leme_overview_animate(struct leme_overview *overview) {
  struct leme_server *server = overview->output->server;
  const struct leme_animation_settings *settings =
      &server->config->animation[LEME_ANIMATION_OVERVIEW];
  struct leme_animation_spec spec = leme_animation_move_spec(
      settings, (struct leme_box){0}, (struct leme_box){0}, 0.0, 1.0);

  leme_overview_stop(overview);
  for (size_t index = 0; index < overview->count; index++) {
    overview->slots[index].from = overview->slots[index].shown;
    overview->slots[index].alpha_from = overview->slots[index].alpha;
  }
  overview->dim_from = overview->dim_alpha;
  if (settings->configured) {
    overview->motion = wlr_scene_tree_create(overview->tree);
  }
  if (overview->motion == NULL) {
    leme_overview_motion_done(overview);
    return;
  }
  spec.to_scalar = 1.0;
  leme_animation_run(&server->animations, overview->motion, &spec,
                     &(struct leme_animation_subject){
                         .data = overview,
                         .owner = overview->output,
                         .apply = leme_overview_motion_apply,
                         .done = leme_overview_motion_done,
                     });
  wlr_output_schedule_frame(overview->output->wlr_output);
}

static void leme_overview_arrange(struct leme_overview *overview) {
  const struct wlr_cursor *cursor = overview->output->server->cursor;
  const size_t lifted = overview->lifted;
  const size_t over = overview->over;

  for (size_t index = 0; index < overview->count; index++) {
    size_t place = index;

    if (lifted < overview->count) {
      if (index == lifted) {
        place = over;
      } else if (lifted < index && index <= over) {
        place = index - 1;
      } else if (over <= index && index < lifted) {
        place = index + 1;
      }
    }
    overview->slots[index].to = overview->slots[place].box;
    overview->slots[index].alpha_to = 1.0f;
  }
  overview->dim_to = 1.0f;
  if (lifted >= overview->count) {
    leme_overview_select(overview, overview->selected);
    leme_overview_animate(overview);
    return;
  }
  struct leme_overview_slot *slot = &overview->slots[lifted];

  const int rise = overview->count <= overview->columns
                       ? slot->to.height * 23 / 20
                       : slot->to.height / 6;

  slot->to.x -= slot->to.width / 40;
  slot->to.y -= slot->to.height / 40 + rise;
  slot->to.width += slot->to.width / 20;
  slot->to.height += slot->to.height / 20;
  leme_overview_select(overview, over);
  if (overview->dragging) {
    slot->to.x = slot->box.x - slot->box.width / 40 +
                 (int)(cursor->x - overview->press_x);
    slot->to.y = slot->box.y - slot->box.height / 40 +
                 (int)(cursor->y - overview->press_y);
    slot->shown = slot->to;
    leme_overview_place(overview, slot);
  }
  wlr_scene_node_raise_to_top(&slot->frame->node);
  leme_overview_animate(overview);
}

static void leme_overview_dismiss(struct leme_overview *overview, uint16_t id) {
  const struct leme_box full = leme_output_full_box(overview->output);
  size_t chosen = overview->count;

  for (size_t index = 0; index < overview->count; index++) {
    if (overview->slots[index].id == id) {
      chosen = index;
    }
  }
  if (chosen == overview->count) {
    leme_overview_close(overview->output->server);
    return;
  }
  overview->closing = true;
  overview->picked = id;
  overview->lifted = SIZE_MAX;
  overview->dragging = false;
  wlr_scene_node_set_enabled(&overview->chrome->node, false);
  for (size_t index = 0; index < overview->count; index++) {
    overview->slots[index].to =
        index == chosen ? full : overview->slots[index].box;
    overview->slots[index].alpha_to = index == chosen ? 1.0f : 0.0f;
  }
  overview->dim_to = 0.0f;
  wlr_scene_node_raise_to_top(&overview->slots[chosen].frame->node);
  leme_overview_animate(overview);
}

static void leme_overview_drop(struct leme_overview *overview) {
  const size_t lifted = overview->lifted;
  const size_t over = overview->over;

  overview->lifted = SIZE_MAX;
  overview->dragging = false;
  overview->selected = over;
  if (lifted == over ||
      !leme_tags_move(&overview->output->tags, overview->slots[lifted].id,
                      overview->slots[over].id)) {
    leme_overview_arrange(overview);
    return;
  }
  overview->building = true;
  const bool built = leme_overview_build(overview);
  overview->building = false;
  if (!built) {
    leme_overview_close(overview->output->server);
  }
}

static size_t leme_overview_at(const struct leme_server *server) {
  const struct leme_overview *overview = server->overview;

  for (size_t index = 0; index < overview->count; index++) {
    const struct leme_box box = overview->slots[index].box;

    if (server->cursor->x >= box.x && server->cursor->x < box.x + box.width &&
        server->cursor->y >= box.y && server->cursor->y < box.y + box.height) {
      return index;
    }
  }
  return overview->count;
}

static void leme_overview_motion(struct leme_server *server) {
  struct leme_overview *overview = server->overview;
  const size_t index = leme_overview_at(server);

  wlr_seat_pointer_notify_clear_focus(server->seat);
  if (overview->closing) {
    return;
  }
  if (overview->lifted < overview->count) {
    struct leme_overview_slot *slot = &overview->slots[overview->lifted];

    if (!overview->dragging) {
      return;
    }
    if (index < overview->count && index != overview->over) {
      overview->over = index;
      leme_overview_arrange(overview);
      return;
    }
    slot->shown.x = slot->box.x - slot->box.width / 40 +
                    (int)(server->cursor->x - overview->press_x);
    slot->shown.y = slot->box.y - slot->box.height / 40 +
                    (int)(server->cursor->y - overview->press_y);
    slot->from = slot->shown;
    slot->to = slot->shown;
    leme_overview_place(overview, slot);
    return;
  }
  if (overview->pressed < overview->count &&
      (fabs(server->cursor->x - overview->press_x) >= LEME_OVERVIEW_DRAG ||
       fabs(server->cursor->y - overview->press_y) >= LEME_OVERVIEW_DRAG)) {
    overview->lifted = overview->pressed;
    overview->over = index < overview->count ? index : overview->pressed;
    overview->dragging = true;
    leme_overview_arrange(overview);
    return;
  }
  if (index < overview->count && index != overview->selected) {
    leme_overview_select(overview, index);
  }
}

static bool leme_overview_button(struct leme_server *server,
                                 const struct wlr_pointer_button_event *event) {
  struct leme_overview *overview = server->overview;
  const size_t index = leme_overview_at(server);

  if (event->button != BTN_LEFT || overview->closing) {
    return true;
  }
  if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
    if (overview->lifted >= overview->count) {
      overview->pressed = index;
      overview->press_x = server->cursor->x;
      overview->press_y = server->cursor->y;
    }
    return true;
  }
  const size_t pressed = overview->pressed;

  overview->pressed = SIZE_MAX;
  if (overview->dragging) {
    leme_overview_drop(overview);
    return true;
  }
  if (pressed != index || overview->lifted < overview->count) {
    return true;
  }
  leme_overview_dismiss(overview, index < overview->count
                                      ? overview->slots[index].id
                                      : overview->output->tags.focused_id);
  return true;
}

static bool leme_overview_key(struct leme_server *server, xkb_keysym_t keysym,
                              uint32_t modifiers, const char *utf8,
                              size_t length) {
  struct leme_overview *overview = server->overview;
  const bool lifted = overview->lifted < overview->count;
  size_t target = lifted ? overview->over : overview->selected;

  (void)modifiers;
  (void)utf8;
  (void)length;
  if (overview->closing) {
    if (keysym == XKB_KEY_Escape) {
      leme_overview_close(server);
    }
    return true;
  }
  switch (keysym) {
  case XKB_KEY_Left:
    target = target > 0 ? target - 1 : target;
    break;
  case XKB_KEY_Right:
    target = target + 1 < overview->count ? target + 1 : target;
    break;
  case XKB_KEY_Up:
    target = target >= overview->columns ? target - overview->columns : target;
    break;
  case XKB_KEY_Down:
    if (target / overview->columns <
        (overview->count - 1) / overview->columns) {
      target = target + overview->columns < overview->count
                   ? target + overview->columns
                   : overview->count - 1;
    }
    break;
  case XKB_KEY_space:
    if (lifted) {
      leme_overview_drop(overview);
    } else {
      overview->lifted = target;
      overview->over = target;
      overview->pressed = SIZE_MAX;
      leme_overview_arrange(overview);
    }
    return true;
  case XKB_KEY_Return:
  case XKB_KEY_KP_Enter:
    if (lifted) {
      leme_overview_drop(overview);
    } else {
      leme_overview_dismiss(overview, overview->slots[target].id);
    }
    return true;
  case XKB_KEY_Escape:
    if (lifted) {
      overview->lifted = SIZE_MAX;
      overview->dragging = false;
      overview->pressed = SIZE_MAX;
      leme_overview_arrange(overview);
    } else {
      leme_overview_dismiss(overview, overview->output->tags.focused_id);
    }
    return true;
  default:
    return false;
  }
  if (lifted) {
    overview->over = target;
    leme_overview_arrange(overview);
  } else if (target != overview->selected) {
    leme_overview_select(overview, target);
  }
  return true;
}

static const struct leme_desk_owner leme_overview_owner = {
    .modal = true,
    .motion = leme_overview_motion,
    .button = leme_overview_button,
    .key = leme_overview_key,
    .cancel = leme_overview_close,
};

void leme_overview_toggle(struct leme_server *server) {
  struct leme_output *output = leme_output_focused(server);
  struct leme_overview *overview = server->overview;

  if (overview != NULL) {
    if (!overview->closing) {
      leme_overview_dismiss(overview, overview->output->tags.focused_id);
    }
    return;
  }
  if (output == NULL || leme_output_full_box(output).width <= 0 ||
      leme_output_full_box(output).height <= 0) {
    return;
  }
  leme_render_workspace_transition_finish(output);
  leme_input_workspace_gesture_cancel(server);
  leme_desk_owner_cancel(server);
  overview = calloc(1, sizeof(*overview));
  if (overview == NULL) {
    return;
  }
  overview->output = output;
  overview->pressed = SIZE_MAX;
  if (!leme_overview_build(overview)) {
    leme_overview_clear(overview);
    free(overview);
    return;
  }
  server->overview = overview;
  leme_desk_owner_claim(server, &leme_overview_owner, 0);
  leme_render_workspace_refresh_views(output);
  wlr_seat_pointer_notify_clear_focus(server->seat);
  leme_desktop_cursor_default(server);
  for (size_t index = 0; index < overview->count; index++) {
    struct leme_overview_slot *slot = &overview->slots[index];

    if (index == overview->selected) {
      slot->shown = leme_output_full_box(output);
    } else {
      slot->alpha = 0.0f;
    }
    leme_overview_place(overview, slot);
  }
  overview->dim_alpha = 0.0f;
  wlr_scene_node_set_enabled(&overview->chrome->node, false);
  wlr_scene_node_raise_to_top(&overview->slots[overview->selected].frame->node);
  leme_overview_animate(overview);
}

bool leme_overview_hides(const struct leme_output *output) {
  return output != NULL && output->server != NULL &&
         output->server->overview != NULL &&
         output->server->overview->output == output;
}

void leme_overview_refresh(struct leme_output *output) {
  struct leme_overview *overview;

  if (!leme_overview_hides(output) || output->server->overview->building ||
      output->server->overview->closing) {
    return;
  }
  overview = output->server->overview;
  overview->building = true;
  const bool built = leme_overview_build(overview);
  overview->building = false;
  if (!built) {
    leme_overview_close(output->server);
  }
}
