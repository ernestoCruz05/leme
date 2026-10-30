#include "desktop/strip.h"
#include "desktop/owner.h"
#include "desktop/target.h"
#include "shell/container.h"

#include "config/config.h"
#include "config/style.h"
#include "core/server.h"
#include "desktop/app-icon.h"
#include "desktop/desktop.h"
#include "desktop/grid.h"
#include "desktop/icons.h"
#include "desktop/pixels.h"
#include "input/input.h"
#include "output/output.h"
#include "render/render.h"
#include "shell/view.h"

#include <limits.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>

#define LEME_DESK_STRIP_CELL 64

struct leme_desk_strip_item {
  struct leme_desk_target target;
  uint64_t minimized_order;
  char *identity;
  char *path;
  bool resolved;
};

struct leme_desk_strip_output {
  struct wlr_scene_tree *tree;
  struct leme_desk_strip_item *items;
  size_t count;
  size_t page;
  size_t capacity;
  struct leme_box area;
  float scale;
  uint16_t tag;
  bool dirty;
  bool refresh;
};

enum leme_desk_strip_action {
  LEME_DESK_STRIP_NONE,
  LEME_DESK_STRIP_RESTORE,
  LEME_DESK_STRIP_PREVIOUS,
  LEME_DESK_STRIP_NEXT,
};

struct leme_desk_strip {
  struct leme_desk_icons *icons;
  struct leme_output *pressed_output;
  struct leme_desk_target pressed_target;
  enum leme_desk_strip_action action;
  double press_x;
  double press_y;
};

static const struct leme_desk_owner leme_desk_strip_owner;

size_t leme_desk_strip_capacity(struct leme_box area, size_t count) {
  if (count == 0 || area.width < LEME_DESK_STRIP_CELL ||
      area.height < LEME_DESK_STRIP_CELL ||
      (int64_t)area.x + area.width > INT_MAX ||
      (int64_t)area.y + area.height > INT_MAX) {
    return 0;
  }
  const size_t cells = (size_t)(area.width / LEME_DESK_STRIP_CELL);
  if (count <= cells) {
    return cells;
  }
  return cells > 2 ? cells - 2 : 0;
}

struct leme_box leme_desk_strip_area_for_count(struct leme_box area,
                                               size_t count) {
  if (count == 0) {
    return area;
  }
  if (leme_desk_strip_capacity(area, count) == 0) {
    return (struct leme_box){0};
  }
  area.height -= LEME_DESK_STRIP_CELL;
  return area;
}

void leme_desk_strip_cancel_click(struct leme_server *server) {
  if (server == NULL) {
    return;
  }
  leme_desk_owner_release(server, &leme_desk_strip_owner);
  if (server->desk_strip != NULL) {
    server->desk_strip->pressed_output = NULL;
    server->desk_strip->pressed_target = (struct leme_desk_target){0};
    server->desk_strip->action = LEME_DESK_STRIP_NONE;
  }
}

static void leme_desk_strip_cancel_output(struct leme_output *output) {
  if (output->server != NULL && output->server->desk_strip != NULL &&
      output->server->desk_strip->pressed_output == output) {
    leme_desk_strip_cancel_click(output->server);
  }
}

static void leme_desk_strip_items_destroy(struct leme_desk_strip_item *items,
                                          size_t count) {
  if (items == NULL) {
    return;
  }
  for (size_t i = 0; i < count; i++) {
    free(items[i].identity);
    free(items[i].path);
  }
  free(items);
}

static void leme_desk_strip_clear(struct leme_desk_strip_output *state) {
  if (state->tree != NULL) {
    wlr_scene_node_destroy(&state->tree->node);
    state->tree = NULL;
  }
  leme_desk_strip_items_destroy(state->items, state->count);
  state->items = NULL;
  state->count = 0;
}

static void leme_desk_strip_hide(struct leme_output *output) {
  leme_desk_strip_cancel_output(output);
  if (output->desk_strip != NULL && output->desk_strip->tree != NULL) {
    wlr_scene_node_set_enabled(&output->desk_strip->tree->node, false);
  }
}

static bool leme_desk_strip_geometry(const struct leme_output *output) {
  const struct leme_desk_strip_output *state = output->desk_strip;
  return leme_box_equal(state->area, output->usable_box) &&
         state->scale == output->wlr_output->scale &&
         state->tag == output->tags.focused_id;
}

static bool leme_desk_strip_ready(const struct leme_output *output) {
  return output != NULL && leme_desk_output_active(output) &&
         output->server != NULL && output->server->config != NULL &&
         output->server->config->desktop_minimized_strip &&
         output->desk_strip != NULL && output->desk_strip->tree != NULL &&
         output->desk_strip->tree->node.enabled && !output->desk_strip->dirty &&
         leme_desk_strip_geometry(output);
}

struct leme_box leme_desk_strip_content_box(const struct leme_output *output) {
  struct leme_box area =
      output == NULL ? (struct leme_box){0} : output->usable_box;
  if (leme_desk_strip_ready(output)) {
    area = leme_desk_strip_area_for_count(area, output->desk_strip->count);
  }
  return area;
}

bool leme_desk_strip_icon_box(const struct leme_output *output,
                              struct leme_desk_target target,
                              struct leme_box *out) {
  if (out == NULL || !leme_desk_strip_ready(output)) {
    return false;
  }
  const struct leme_desk_strip_output *state = output->desk_strip;
  if (state->capacity == 0) {
    return false;
  }
  for (size_t i = 0; i < state->count; i++) {
    if (!leme_desk_target_equal(state->items[i].target, target)) {
      continue;
    }
    const size_t page = i / state->capacity;
    const bool paging = state->count > state->capacity;
    const size_t cell =
        page < state->page ? 0
        : page > state->page
            ? (size_t)(state->area.width / LEME_DESK_STRIP_CELL - 1)
            : i % state->capacity + (paging ? 1U : 0U);
    const int x = (int)cell * LEME_DESK_STRIP_CELL;
    *out = (struct leme_box){state->area.x + x,
                             state->area.y + state->area.height -
                                 LEME_DESK_STRIP_CELL,
                             LEME_DESK_STRIP_CELL, LEME_DESK_STRIP_CELL};
    return true;
  }
  return false;
}

static const char *leme_desk_strip_identity(const struct leme_view *view) {
  const char *identity = leme_view_identity(view);
  return identity == NULL || strnlen(identity, 256) >= 256 ? "" : identity;
}

static struct leme_desk_strip_item *
leme_desk_strip_find(const struct leme_desk_strip_output *state,
                     struct leme_desk_target target) {
  if (state != NULL) {
    for (size_t i = 0; i < state->count; i++) {
      if (leme_desk_target_equal(state->items[i].target, target)) {
        return &state->items[i];
      }
    }
  }
  return NULL;
}

static int leme_desk_strip_compare(const void *a, const void *b) {
  const struct leme_desk_strip_item *left = a;
  const struct leme_desk_strip_item *right = b;
  if (left->minimized_order != right->minimized_order) {
    return (left->minimized_order > right->minimized_order) -
           (left->minimized_order < right->minimized_order);
  }
  return (left->target.id.serial > right->target.id.serial) -
         (left->target.id.serial < right->target.id.serial);
}

static bool leme_desk_strip_rect(struct wlr_scene_tree *tree,
                                 struct leme_box box, const float color[4]) {
  struct wlr_scene_rect *rect =
      wlr_scene_rect_create(tree, box.width, box.height, color);
  if (rect == NULL) {
    return false;
  }
  wlr_scene_node_set_position(&rect->node, box.x, box.y);
  return true;
}

static bool leme_desk_strip_fallback(struct wlr_scene_tree *tree, int x) {
  const float white[4] = {0.8f, 0.8f, 0.8f, 1.0f};
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  return leme_desk_strip_rect(tree, (struct leme_box){x + 12, 14, 40, 36},
                              white) &&
         leme_desk_strip_rect(tree, (struct leme_box){x + 15, 23, 34, 24},
                              black);
}

static bool leme_desk_strip_arrow(struct wlr_scene_tree *tree, int x, bool next,
                                  bool enabled,
                                  const struct leme_config *config) {
  struct leme_desk_look look = {0};
  leme_desk_style_resolve(
      config, LEME_DESK_ELEMENT_STRIP,
      enabled ? 0 : LEME_DESK_STATE_BIT(LEME_DESK_STATE_DISABLED), &look);
  const float *color = look.color;
  for (int i = 0; i < 7; i++) {
    const int dx = next ? i * 2 : -i * 2;
    if (!leme_desk_strip_rect(
            tree, (struct leme_box){x + 28 + dx, 20 + i * 2, 3, 3}, color) ||
        !leme_desk_strip_rect(
            tree, (struct leme_box){x + 28 + dx, 44 - i * 2, 3, 3}, color)) {
      return false;
    }
  }
  return true;
}

static bool leme_desk_strip_view_unit(const struct leme_view *view,
                                      struct leme_desk_target *target) {
  return leme_view_minimized_unit(view, target) &&
         target->kind == LEME_DESK_TARGET_VIEW &&
         target->id.serial == view->public_meta.id.serial;
}

static bool
leme_desk_strip_prepare_item(const struct leme_desk_strip_output *state,
                             struct leme_desk_strip_item *item,
                             struct leme_desk_target target, uint64_t order,
                             const struct leme_view *icon_view, bool reuse) {
  if (target.id.serial == 0) {
    return false;
  }
  item->target = target;
  item->minimized_order = order;
  item->identity = strdup(leme_desk_strip_identity(icon_view));
  if (item->identity == NULL) {
    return false;
  }
  const struct leme_desk_strip_item *old = leme_desk_strip_find(state, target);
  if (reuse && old != NULL && strcmp(old->identity, item->identity) == 0) {
    item->resolved = old->resolved;
    if (old->path != NULL) {
      item->path = strdup(old->path);
      if (item->path == NULL) {
        return false;
      }
    }
  }
  return true;
}

static bool
leme_desk_strip_item_same(const struct leme_desk_strip_output *state,
                          struct leme_desk_target target, uint64_t order,
                          const struct leme_view *icon_view) {
  const struct leme_desk_strip_item *item = leme_desk_strip_find(state, target);
  return item != NULL && item->minimized_order == order &&
         strcmp(item->identity, leme_desk_strip_identity(icon_view)) == 0;
}

static bool leme_desk_strip_build(struct leme_output *output,
                                  struct leme_tag *tag, size_t count) {
  const size_t capacity = leme_desk_strip_capacity(output->usable_box, count);
  int pixels = 0;
  if (capacity == 0 || !leme_desk_grid_pixels(output->wlr_output->scale,
                                              LEME_DESK_STRIP_ICON, &pixels)) {
    return false;
  }
  struct leme_desk_strip_output *state = output->desk_strip;
  struct leme_desk_strip_item *items = NULL;
  struct wlr_scene_tree *tree = NULL;
  bool ok = false;
  if (count > SIZE_MAX / sizeof(*items)) {
    return false;
  }
  items = calloc(count, sizeof(*items));
  if (items == NULL) {
    return false;
  }
  const bool reuse =
      !state->refresh && state->scale == output->wlr_output->scale;
  size_t index = 0;
  struct leme_view *view = NULL;
  wl_list_for_each(view, &tag->views, tag_link) {
    struct leme_desk_target target = {0};
    if (!leme_desk_strip_view_unit(view, &target)) {
      continue;
    }
    if (index >= count ||
        !leme_desk_strip_prepare_item(state, &items[index++], target,
                                      view->minimized_order, view, reuse)) {
      goto cleanup;
    }
  }
  if (tag->containers.next != NULL) {
    const struct leme_container *container = NULL;
    wl_list_for_each(container, &tag->containers, link) {
      if (!container->minimized) {
        continue;
      }
      const struct leme_desk_target target = {LEME_DESK_TARGET_CONTAINER,
                                              container->id};
      if (index >= count ||
          !leme_desk_strip_prepare_item(state, &items[index++], target,
                                        container->minimized_order,
                                        container->focused, reuse)) {
        goto cleanup;
      }
    }
  }
  if (index != count) {
    goto cleanup;
  }
  qsort(items, count, sizeof(*items), leme_desk_strip_compare);
  const size_t last_page = (count - 1) / capacity;
  const size_t page = state->tag != tag->id     ? 0
                      : state->page > last_page ? last_page
                                                : state->page;
  const size_t start = page * capacity;
  const size_t visible = count - start < capacity ? count - start : capacity;
  const bool paging = count > capacity;
  tree = wlr_scene_tree_create(output->server->scene_minimized);
  if (tree == NULL) {
    goto cleanup;
  }
  wlr_scene_node_set_enabled(&tree->node, false);
  const struct leme_box area = output->usable_box;
  wlr_scene_node_set_position(&tree->node, area.x,
                              area.y + (area.height - LEME_DESK_STRIP_CELL));
  tree->node.data = output;
  const struct leme_config *config = output->server->config;
  struct leme_desk_look look = {0};
  leme_desk_style_resolve(config, LEME_DESK_ELEMENT_STRIP, 0, &look);
  if (!leme_desk_strip_rect(
          tree, (struct leme_box){0, 0, area.width, LEME_DESK_STRIP_CELL},
          look.background)) {
    goto cleanup;
  }
  if (paging &&
      (!leme_desk_strip_arrow(tree, 0, false, page > 0, config) ||
       !leme_desk_strip_arrow(
           tree, (area.width / LEME_DESK_STRIP_CELL - 1) * LEME_DESK_STRIP_CELL,
           true, page < last_page, config))) {
    goto cleanup;
  }
  int x = paging ? LEME_DESK_STRIP_CELL : 0;
  for (size_t i = 0; i < visible; i++, x += LEME_DESK_STRIP_CELL) {
    struct leme_desk_strip_item *item = &items[start + i];
    if (!item->resolved) {
      struct leme_desk_look icon = {0};
      leme_desk_style_resolve(config, LEME_DESK_ELEMENT_ICON, 0, &icon);
      (void)leme_desk_app_icon_resolve(
          item->identity, icon.theme,
          (int)ceil((double)output->wlr_output->scale), &item->path);
      item->resolved = true;
    }
    struct wlr_buffer *buffer =
        item->path == NULL
            ? NULL
            : leme_desk_icons_get(output->server->desk_strip->icons, item->path,
                                  pixels);
    if (buffer != NULL) {
      if (!leme_desk_pixels_place_buffer(
              tree, buffer,
              (struct leme_box){x + 8, 8, LEME_DESK_STRIP_ICON,
                                LEME_DESK_STRIP_ICON})) {
        goto cleanup;
      }
    } else if (!leme_desk_strip_fallback(tree, x)) {
      goto cleanup;
    }
  }
  leme_desk_strip_clear(state);
  state->tree = tree;
  state->items = items;
  state->count = count;
  state->page = page;
  state->capacity = capacity;
  state->area = area;
  state->scale = output->wlr_output->scale;
  state->tag = tag->id;
  state->dirty = false;
  state->refresh = false;
  wlr_scene_node_set_enabled(&tree->node, true);
  leme_desk_icons_prune(output->server->desk_strip->icons);
  ok = true;
cleanup:
  if (!ok) {
    if (tree != NULL) {
      wlr_scene_node_destroy(&tree->node);
    }
    leme_desk_strip_items_destroy(items, count);
  }
  return ok;
}

bool leme_desk_strip_sync_output(struct leme_output *output) {
  if (output == NULL) {
    return true;
  }
  if (!leme_desk_output_active(output) || output->server == NULL ||
      output->server->config == NULL ||
      !output->server->config->desktop_minimized_strip) {
    leme_desk_strip_hide(output);
    return true;
  }
  struct leme_tag *tag = output->tags.table[output->tags.focused_id];
  size_t count = 0;
  bool covered = false;
  bool same = output->desk_strip != NULL && !output->desk_strip->dirty &&
              leme_desk_strip_geometry(output);
  struct leme_view *view = NULL;
  if (tag != NULL) {
    wl_list_for_each(view, &tag->views, tag_link) {
      covered = covered || (view->mapped && view->fullscreen);
      struct leme_desk_target target = {0};
      if (leme_desk_strip_view_unit(view, &target)) {
        count++;
        same = same && leme_desk_strip_item_same(output->desk_strip, target,
                                                 view->minimized_order, view);
      }
    }
    if (tag->containers.next != NULL) {
      const struct leme_container *container = NULL;
      wl_list_for_each(container, &tag->containers, link) {
        if (!container->minimized) {
          continue;
        }
        count++;
        const struct leme_desk_target target = {LEME_DESK_TARGET_CONTAINER,
                                                container->id};
        same = same && leme_desk_strip_item_same(output->desk_strip, target,
                                                 container->minimized_order,
                                                 container->focused);
      }
    }
  }
  if (count == 0) {
    leme_desk_strip_hide(output);
    if (output->desk_strip != NULL) {
      leme_desk_strip_clear(output->desk_strip);
    }
    return true;
  }
  if (output->server->desk_strip == NULL ||
      output->server->scene_minimized == NULL) {
    leme_desk_strip_hide(output);
    return false;
  }
  if (covered) {
    leme_desk_strip_hide(output);
    return true;
  }
  if (same && output->desk_strip->count == count &&
      output->desk_strip->tree != NULL) {
    wlr_scene_node_set_enabled(&output->desk_strip->tree->node, true);
    return true;
  }
  leme_desk_strip_cancel_output(output);
  if (output->desk_strip == NULL) {
    output->desk_strip = calloc(1, sizeof(*output->desk_strip));
    if (output->desk_strip == NULL) {
      return false;
    }
  }
  if (!leme_desk_strip_build(output, tag, count)) {
    output->desk_strip->dirty = true;
    leme_desk_strip_hide(output);
    return false;
  }
  return true;
}

bool leme_desk_strip_init(struct leme_server *server) {
  if (server == NULL || server->scene_minimized == NULL ||
      server->desk_strip != NULL) {
    return false;
  }
  struct leme_desk_strip *strip = calloc(1, sizeof(*strip));
  if (strip == NULL) {
    return false;
  }
  strip->icons = leme_desk_icons_create();
  if (strip->icons == NULL) {
    free(strip);
    return false;
  }
  server->desk_strip = strip;
  return true;
}

void leme_desk_strip_output_finish(struct leme_output *output) {
  if (output == NULL) {
    return;
  }
  leme_desk_strip_cancel_output(output);
  if (output->desk_strip != NULL) {
    leme_desk_strip_clear(output->desk_strip);
    free(output->desk_strip);
    output->desk_strip = NULL;
  }
}

void leme_desk_strip_finish(struct leme_server *server) {
  if (server == NULL || server->desk_strip == NULL) {
    return;
  }
  if (server->outputs.next != NULL) {
    struct leme_output *output = NULL;
    wl_list_for_each(output, &server->outputs, link) {
      leme_desk_strip_output_finish(output);
    }
  }
  leme_desk_icons_destroy(server->desk_strip->icons);
  free(server->desk_strip);
  server->desk_strip = NULL;
}

void leme_desk_strip_refresh(struct leme_server *server) {
  if (server == NULL || server->desk_strip == NULL) {
    return;
  }
  leme_desk_strip_cancel_click(server);
  leme_desk_icons_clear(server->desk_strip->icons);
  if (server->outputs.next != NULL) {
    struct leme_output *output = NULL;
    wl_list_for_each(output, &server->outputs, link) {
      if (output->desk_strip != NULL) {
        output->desk_strip->dirty = true;
        output->desk_strip->refresh = true;
      }
    }
    leme_view_arrange_instant(server);
  }
}

static bool leme_desk_strip_pointer_allowed(struct leme_server *server) {
  return server->cursor != NULL && server->seat != NULL &&
         leme_input_idle(server) &&
         server->seat->pointer_state.button_count == 0 &&
         leme_input_modifiers(server) == 0;
}

static void leme_desk_strip_motion(struct leme_server *server) {
  if (server->desk_strip == NULL) {
    return;
  }
  struct leme_desk_strip *strip = server->desk_strip;
  if (strip->pressed_output != NULL &&
      (!leme_desk_strip_pointer_allowed(server) ||
       !leme_desk_strip_ready(strip->pressed_output) ||
       strip->pressed_output != server->focused_output ||
       hypot(server->cursor->x - strip->press_x,
             server->cursor->y - strip->press_y) > 4.0)) {
    leme_desk_strip_cancel_click(server);
  }
}

static enum leme_desk_strip_action
leme_desk_strip_at(const struct leme_output *output, double x, double y,
                   struct leme_desk_target *target) {
  *target = (struct leme_desk_target){0};
  if (!leme_desk_strip_ready(output) || !isfinite(x) || !isfinite(y)) {
    return LEME_DESK_STRIP_NONE;
  }
  const struct leme_desk_strip_output *state = output->desk_strip;
  const struct leme_box area = state->area;
  x -= area.x;
  y -= (double)area.y + area.height - LEME_DESK_STRIP_CELL;
  if (x < 0.0 || x >= area.width || y < 0.0 || y >= LEME_DESK_STRIP_CELL) {
    return LEME_DESK_STRIP_NONE;
  }
  const size_t cell = (size_t)(x / LEME_DESK_STRIP_CELL);
  const size_t cells = (size_t)(area.width / LEME_DESK_STRIP_CELL);
  if (cell >= cells) {
    return LEME_DESK_STRIP_NONE;
  }
  const bool paging = state->count > state->capacity;
  if (paging && cell == 0) {
    return state->page > 0 ? LEME_DESK_STRIP_PREVIOUS : LEME_DESK_STRIP_NONE;
  }
  if (paging && cell == cells - 1) {
    return state->page < (state->count - 1) / state->capacity
               ? LEME_DESK_STRIP_NEXT
               : LEME_DESK_STRIP_NONE;
  }
  const size_t index =
      state->page * state->capacity + cell - (paging ? 1U : 0U);
  if (index >= state->count) {
    return LEME_DESK_STRIP_NONE;
  }
  *target = state->items[index].target;
  return LEME_DESK_STRIP_RESTORE;
}

static bool
leme_desk_strip_release(struct leme_server *server,
                        const struct wlr_pointer_button_event *event) {
  struct leme_desk_strip *strip = server->desk_strip;
  if (event->state != WL_POINTER_BUTTON_STATE_RELEASED ||
      event->button != BTN_LEFT || strip == NULL) {
    return false;
  }
  leme_desk_strip_motion(server);
  struct leme_output *output = strip->pressed_output;
  struct leme_desk_target target = {0};
  struct leme_render_hit current = {0};
  bool valid = output != NULL && leme_desk_strip_pointer_allowed(server);
  if (valid) {
    leme_render_at(server, server->cursor->x, server->cursor->y, &current);
    valid = current.strip_output == output &&
            leme_desk_strip_at(output, server->cursor->x, server->cursor->y,
                               &target) == strip->action &&
            leme_desk_target_equal(target, strip->pressed_target);
  }
  const enum leme_desk_strip_action action = strip->action;
  leme_desk_strip_cancel_click(server);
  if (!valid) {
    return true;
  }
  if (action == LEME_DESK_STRIP_RESTORE) {
    struct leme_container *container =
        leme_desk_target_container(server, target);
    struct leme_view *view = leme_desk_target_member(server, target);
    const struct leme_tag *tag =
        container != NULL ? container->tag : leme_ownership_tag(view);
    if (view == NULL || tag == NULL || tag->owner->output != output ||
        tag->id != output->tags.focused_id) {
      return true;
    }
    const struct leme_public_id id = view->public_meta.id;
    if (container != NULL ? leme_container_set_minimized(container, false)
                          : leme_view_set_minimized(view, false)) {
      leme_output_set_focused(server, output, false);
      leme_view_focus(leme_view_by_public_id(server, id));
    }
  } else if (action == LEME_DESK_STRIP_PREVIOUS ||
             action == LEME_DESK_STRIP_NEXT) {
    if (action == LEME_DESK_STRIP_PREVIOUS) {
      output->desk_strip->page--;
    } else {
      output->desk_strip->page++;
    }
    output->desk_strip->dirty = true;
    leme_tags_refresh_visibility_passive(&output->tags);
  }
  return true;
}

bool leme_desk_strip_handle_button(
    struct leme_server *server, const struct leme_render_hit *hit,
    const struct wlr_pointer_button_event *event) {
  struct leme_desk_strip *strip = server == NULL ? NULL : server->desk_strip;
  if (strip == NULL || event->button != BTN_LEFT ||
      event->state != WL_POINTER_BUTTON_STATE_PRESSED || hit == NULL ||
      hit->strip_output == NULL || !leme_desk_strip_pointer_allowed(server)) {
    return false;
  }
  struct leme_render_hit current = {0};
  leme_render_at(server, server->cursor->x, server->cursor->y, &current);
  if (current.strip_output != hit->strip_output ||
      !leme_desk_strip_ready(current.strip_output)) {
    return false;
  }
  strip->pressed_output = current.strip_output;
  strip->press_x = server->cursor->x;
  strip->press_y = server->cursor->y;
  strip->action = leme_desk_strip_at(current.strip_output, strip->press_x,
                                     strip->press_y, &strip->pressed_target);
  leme_desk_owner_claim(server, &leme_desk_strip_owner, BTN_LEFT);
  return true;
}

static const struct leme_desk_owner leme_desk_strip_owner = {
    .motion = leme_desk_strip_motion,
    .button = leme_desk_strip_release,
    .cancel = leme_desk_strip_cancel_click,
};
