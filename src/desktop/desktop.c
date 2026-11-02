#include "desktop/desktop.h"

#include "desktop/catalog.h"
#include "desktop/drag.h"
#include "desktop/grid.h"
#include "desktop/icons.h"
#include "desktop/pixels.h"
#include "desktop/positions.h"
#include "desktop/strip.h"
#include "desktop/menu.h"
#include "desktop/owner.h"
#include "desktop/prompt.h"
#include "desktop/theme.h"
#include "desktop/watch.h"
#include "config/config.h"
#include "config/style.h"
#include "core/command.h"
#include "core/process.h"
#include "core/server.h"
#include "output/output.h"
#include "desktop/open.h"
#include "input/input.h"
#include "protocols/input.h"
#include "protocols/session.h"
#include "render/animation.h"
#include "render/render.h"
#include "shell/layer.h"
#include "shell/view.h"

#include <pango/pangocairo.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>
#include <linux/input-event-codes.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

struct leme_desk_context {
  char *directory;
  char **basenames;
  size_t count;
  struct leme_desk_wish cell;
  struct leme_public_id output;
  uint16_t tag;
  uint64_t serial;
};

struct leme_desk {
  struct leme_server *server;
  struct leme_desk_catalog *catalog;
  struct leme_desk_icons *icons;
  struct leme_desk_watch *watch;
  struct leme_desk_positions *positions;
  char *positions_path;
  char *directory;
  struct leme_output *focus_output;
  uint16_t focus_tag;
  char **selected;
  size_t selected_count;
  char **base;
  size_t base_count;
  struct wlr_scene_tree *band;
  struct wlr_scene_tree *drop;
  struct leme_desk_context context;
  uint64_t context_serial;
  double press_x;
  double press_y;
  double click_x;
  double click_y;
  uint32_t click_time;
  uint32_t press_time;
  bool press_valid;
  bool press_single;
  bool clicked;
  bool loaded;
  bool syncing;
  bool prune_watches;
};

struct leme_desk_label {
  struct wlr_buffer *buffer;
  int height;
};

struct leme_desk_item {
  char *path;
  struct wlr_scene_tree *tree;
  struct wlr_scene_tree *outline;
  struct leme_box cell;
  size_t slot;
  bool directory;
};

struct leme_desk_output {
  struct wlr_scene_tree *tree;
  struct wlr_scene_rect *input;
  struct leme_box usable;
  struct leme_desk_item *items;
  size_t count;
  size_t columns;
  size_t rows;
  int row_height;
  struct leme_desk_grid_metrics metrics;
  float scale;
  bool dirty;
};

static void leme_desk_context_clear(struct leme_desk *desk);
static void leme_desk_context_check(struct leme_desk *desk);

static void leme_desk_cancel_click(struct leme_desk *desk) {
  desk->press_valid = false;
  desk->clicked = false;
}

static void leme_desk_items_destroy(struct leme_desk_item *items,
                                    size_t count) {
  if (items == NULL) {
    return;
  }
  for (size_t i = 0; i < count; i++) {
    free(items[i].path);
  }
  free(items);
}

static void leme_desk_output_clear_scene(struct leme_desk_output *state) {
  if (state->tree != NULL) {
    wlr_scene_node_destroy(&state->tree->node);
    state->tree = NULL;
  }
  leme_desk_items_destroy(state->items, state->count);
  state->items = NULL;
  state->count = 0;
  state->input = NULL;
}

bool leme_desk_output_active(const struct leme_output *output) {
  if (output == NULL || output->wlr_output == NULL ||
      !output->wlr_output->enabled || output->tags.table == NULL ||
      output->tags.focused_id == 0 ||
      output->tags.focused_id > output->tags.max_tags) {
    return false;
  }
  const struct leme_tag *tag = output->tags.table[output->tags.focused_id];
  if (tag != NULL) {
    return tag->layout.kind == LEME_LAYOUT_DESKTOP;
  }
  if (output->server == NULL || output->server->config == NULL) {
    return false;
  }
  struct leme_tag_settings settings = {0};
  leme_config_tag_settings(output->server->config, output->tags.focused_id,
                           &settings);
  return settings.layout == LEME_LAYOUT_DESKTOP;
}

static void leme_desk_mark_dirty(struct leme_desk *desk) {
  struct leme_output *output;
  wl_list_for_each(output, &desk->server->outputs, link) {
    if (output->desk != NULL) {
      output->desk->dirty = true;
    }
  }
}

static void leme_desk_watch_asset(const char *path, void *data) {
  struct leme_desk *desk = data;
  if (desk->watch != NULL && !leme_desk_watch_add_file(desk->watch, path)) {
    wlr_log(WLR_ERROR, "leme: could not watch desktop asset %s", path);
  }
}

static void leme_desk_handle_directory_changed(void *data);

static bool leme_desk_rescan(struct leme_desk *desk, bool refresh_watch) {
  leme_desk_cancel_click(desk);
  char *directory = leme_desk_catalog_directory();
  struct leme_desk_catalog *catalog = leme_desk_catalog_create();
  if (catalog == NULL ||
      (directory != NULL && !leme_desk_catalog_scan(catalog, directory))) {
    leme_desk_catalog_destroy(catalog);
    free(directory);
    wlr_log(WLR_ERROR, "%s", "leme: could not read desktop directory");
    return false;
  }
  const bool same_directory =
      directory == NULL
          ? desk->directory == NULL
          : desk->directory != NULL && strcmp(directory, desk->directory) == 0;
  leme_desk_catalog_destroy(desk->catalog);
  free(desk->directory);
  desk->catalog = catalog;
  desk->directory = directory;
  desk->loaded = true;
  leme_desk_context_check(desk);
  if (refresh_watch || !same_directory || desk->watch == NULL) {
    leme_desk_positions_destroy(desk->positions);
    desk->positions = leme_desk_positions_load(desk->positions_path, directory);
    leme_desk_watch_destroy(desk->watch);
    desk->watch = NULL;
    if (directory != NULL && desk->server->display != NULL) {
      desk->watch = leme_desk_watch_create(
          wl_display_get_event_loop(desk->server->display), directory,
          leme_desk_handle_directory_changed, desk);
      if (desk->watch == NULL) {
        wlr_log(
            WLR_ERROR, "%s",
            "leme: desktop updates need manual reload; directory watch unavailable");
      }
    }
  }
  leme_desk_watch_begin_files(desk->watch);
  desk->prune_watches = true;
  for (size_t i = 0; i < leme_desk_catalog_count(desk->catalog); i++) {
    const struct leme_desk_entry *entry =
        leme_desk_catalog_entry(desk->catalog, i);
    if (!entry->directory && g_str_has_suffix(entry->path, ".desktop")) {
      leme_desk_watch_asset(entry->path, desk);
    }
    if (entry->icon[0] == '/') {
      leme_desk_watch_asset(entry->icon, desk);
    }
  }
  leme_desk_mark_dirty(desk);
  return true;
}

static void leme_desk_handle_directory_changed(void *data) {
  struct leme_desk *desk = data;
  if (leme_desk_rescan(desk, false)) {
    leme_desk_sync(desk->server);
  }
}

#define LEME_DESK_ICON_NAMES 8

static struct wlr_buffer *
leme_desk_entry_image(struct leme_desk *desk,
                      const struct leme_desk_entry *entry, const char *theme,
                      int size, int pixels, int scale) {
  const char *names[LEME_DESK_ICON_NAMES] = {entry->icon};
  size_t count = 1;
  for (size_t i = 0; entry->fallbacks != NULL && entry->fallbacks[i] != NULL &&
                     count < LEME_ARRAY_LENGTH(names) - 1;
       i++) {
    names[count++] = entry->fallbacks[i];
  }
  names[count++] = entry->directory ? "folder" : "text-x-generic";
  const char *const themes[] = {theme, LEME_DESK_STYLE_ICON_THEME};
  for (size_t t = 0; t < LEME_ARRAY_LENGTH(themes); t++) {
    if (t > 0 && strcmp(themes[t], theme) == 0) {
      break;
    }
    for (size_t i = 0; i < count; i++) {
      char *path = NULL;
      if (!leme_desk_theme_resolve(themes[t], names[i], size, scale, &path)) {
        continue;
      }
      leme_desk_watch_asset(path, desk);
      struct wlr_buffer *buffer =
          leme_desk_icons_get(desk->icons, path, pixels);
      free(path);
      if (buffer != NULL) {
        return buffer;
      }
    }
  }
  return NULL;
}

static const char *leme_desk_entry_name(const struct leme_desk_entry *entry) {
  const char *slash = strrchr(entry->path, '/');
  return slash == NULL ? entry->path : slash + 1;
}

static bool leme_desk_label(struct leme_desk_label *label,
                            const struct leme_desk_entry *entry,
                            const struct leme_desk_look *look, double scale) {
  label->buffer =
      leme_desk_pixels_label(entry->label, look->font, look->color,
                             look->shadow, look->width, scale, &label->height);
  return label->buffer != NULL;
}

static bool leme_desk_outline(struct leme_desk_item *item,
                              const struct leme_desk_look *look) {
  item->outline = wlr_scene_tree_create(item->tree);
  if (item->outline == NULL) {
    return false;
  }
  wlr_scene_node_set_enabled(&item->outline->node, false);
  const int width = item->cell.width;
  const int height = item->cell.height;
  if (look->background[3] > 0.0f &&
      !leme_desk_pixels_rect(item->outline,
                             (struct leme_box){0, 0, width, height},
                             look->background, 0, false)) {
    return false;
  }
  const float *color = look->border_color;
  const struct leme_box edges[4] = {
      {0, 0, width, 2},
      {0, height - 2, width, 2},
      {0, 2, 2, height - 4},
      {width - 2, 2, 2, height - 4},
  };
  for (size_t i = 0; i < LEME_ARRAY_LENGTH(edges); i++) {
    struct wlr_scene_rect *rect = wlr_scene_rect_create(
        item->outline, edges[i].width, edges[i].height, color);
    if (rect == NULL) {
      return false;
    }
    wlr_scene_node_set_position(&rect->node, edges[i].x, edges[i].y);
  }
  return true;
}

static struct leme_desk_grid_metrics
leme_desk_metrics(const struct leme_desk_look *look) {
  int spacing = look->spacing < look->size ? look->size : look->spacing;
  spacing = spacing < look->width ? look->width : spacing;
  return (struct leme_desk_grid_metrics){
      .size = look->size,
      .spacing = spacing,
      .padding = look->padding,
      .width = look->width,
      .gap = look->gap,
  };
}

static bool leme_desk_build_output(struct leme_desk *desk,
                                   struct leme_output *output, int pixels) {
  if (desk->focus_output == output) {
    leme_desk_cancel_click(desk);
  }
  struct wlr_scene_tree *tree =
      wlr_scene_tree_create(desk->server->scene_desktop);
  if (tree == NULL) {
    return false;
  }
  wlr_scene_node_set_enabled(&tree->node, false);
  const struct leme_box area = leme_desk_strip_content_box(output);
  wlr_scene_node_set_position(&tree->node, area.x, area.y);
  const size_t count = leme_desk_catalog_count(desk->catalog);
  struct leme_desk_look look = {0};
  struct leme_desk_look selected = {0};
  leme_desk_style_resolve(desk->server->config, LEME_DESK_ELEMENT_ICON, 0,
                          &look);
  leme_desk_style_resolve(desk->server->config, LEME_DESK_ELEMENT_ICON,
                          LEME_DESK_STATE_BIT(LEME_DESK_STATE_SELECTED),
                          &selected);
  const struct leme_desk_grid_metrics metrics = leme_desk_metrics(&look);
  const size_t columns = leme_desk_grid_columns(area, &metrics);
  const double scale = (double)output->wlr_output->scale;
  struct leme_desk_wish *wishes = NULL;
  size_t *slots = NULL;
  struct leme_desk_label *labels = NULL;
  struct leme_desk_item *items = NULL;
  size_t visible = 0;
  bool ok = false;
  struct wlr_scene_rect *input = NULL;
  tree->node.data = output;
  if (area.width > 0 && area.height > 0) {
    const float transparent[4] = {0};
    input = wlr_scene_rect_create(tree, area.width, area.height, transparent);
    if (input == NULL) {
      goto cleanup;
    }
  }
  if (count > SIZE_MAX / sizeof(*labels)) {
    goto cleanup;
  }
  wishes = calloc(count + 1, sizeof(*wishes));
  slots = calloc(count + 1, sizeof(*slots));
  labels = calloc(count + 1, sizeof(*labels));
  if (wishes == NULL || slots == NULL || labels == NULL) {
    goto cleanup;
  }
  for (size_t i = 0; i < count; i++) {
    wishes[i].pinned = leme_desk_positions_get(
        desk->positions,
        leme_desk_entry_name(leme_desk_catalog_entry(desk->catalog, i)),
        &wishes[i].column, &wishes[i].row);
  }
  size_t placed = 0;
  if (!leme_desk_grid_assign(
          columns, leme_desk_grid_rows(area, &metrics, metrics.spacing), wishes,
          count, slots, &placed)) {
    goto cleanup;
  }
  int row_height = metrics.spacing;
  for (size_t i = 0; i < count; i++) {
    if (slots[i] == SIZE_MAX) {
      continue;
    }
    if (!leme_desk_label(&labels[i], leme_desk_catalog_entry(desk->catalog, i),
                         &look, scale)) {
      goto cleanup;
    }
    const int height = metrics.size + 2 * metrics.gap + labels[i].height;
    row_height = row_height < height ? height : row_height;
  }
  const size_t rows = leme_desk_grid_rows(area, &metrics, row_height);
  if (!leme_desk_grid_assign(columns, rows, wishes, count, slots, &visible)) {
    goto cleanup;
  }
  items = calloc(visible + 1, sizeof(*items));
  if (items == NULL) {
    goto cleanup;
  }
  const int image_scale = (int)ceil(scale);
  size_t item = 0;
  for (size_t i = 0; i < count; i++) {
    if (slots[i] == SIZE_MAX) {
      continue;
    }
    const struct leme_desk_entry *entry =
        leme_desk_catalog_entry(desk->catalog, i);
    struct leme_desk_item *current = &items[item++];
    if ((labels[i].buffer == NULL &&
         !leme_desk_label(&labels[i], entry, &look, scale)) ||
        !leme_desk_grid_cell(area, &metrics, row_height, slots[i],
                             &current->cell)) {
      goto cleanup;
    }
    current->slot = slots[i];
    current->directory = entry->directory;
    current->path = strdup(entry->path);
    current->tree = wlr_scene_tree_create(tree);
    if (current->path == NULL || current->tree == NULL) {
      goto cleanup;
    }
    wlr_scene_node_set_position(&current->tree->node, current->cell.x - area.x,
                                current->cell.y - area.y);
    struct wlr_buffer *image = leme_desk_entry_image(
        desk, entry, look.theme, metrics.size, pixels, image_scale);
    const struct leme_box icon_box = {
        .x = (metrics.spacing - metrics.size) / 2,
        .width = metrics.size,
        .height = metrics.size,
    };
    const struct leme_box label_box = {
        .x = (metrics.spacing - metrics.width) / 2,
        .y = metrics.size + metrics.gap,
        .width = metrics.width,
        .height = labels[i].height,
    };
    if ((image != NULL &&
         !leme_desk_pixels_place_buffer(current->tree, image, icon_box)) ||
        !leme_desk_pixels_place_buffer(current->tree, labels[i].buffer,
                                       label_box) ||
        !leme_desk_outline(current, &selected)) {
      goto cleanup;
    }
  }
  struct leme_desk_output *state = output->desk;
  leme_desk_output_clear_scene(state);
  state->tree = tree;
  state->input = input;
  state->items = items;
  items = NULL;
  state->count = visible;
  state->columns = columns;
  state->rows = rows;
  state->row_height = row_height;
  state->metrics = metrics;
  state->usable = area;
  state->scale = output->wlr_output->scale;
  state->dirty = false;
  wlr_scene_node_set_enabled(&tree->node, true);
  ok = true;
cleanup:
  leme_desk_items_destroy(items, visible);
  if (labels != NULL) {
    for (size_t i = 0; i < count; i++) {
      if (labels[i].buffer != NULL) {
        wlr_buffer_drop(labels[i].buffer);
      }
    }
  }
  free(labels);
  free(slots);
  free(wishes);
  if (!ok) {
    wlr_scene_node_destroy(&tree->node);
  }
  return ok;
}

static bool leme_desk_geometry_matches(const struct leme_output *output) {
  const struct leme_desk_output *state = output->desk;
  const struct leme_box area = leme_desk_strip_content_box(output);
  return leme_box_equal(state->usable, area) &&
         state->scale == output->wlr_output->scale;
}

bool leme_desk_at(const struct leme_output *output, double lx, double ly,
                  size_t *index) {
  if (index == NULL || !isfinite(lx) || !isfinite(ly) ||
      !leme_desk_output_active(output) || output->desk == NULL) {
    return false;
  }
  const struct leme_desk_output *state = output->desk;
  if (state->tree == NULL || !state->tree->node.enabled || state->dirty ||
      !leme_desk_geometry_matches(output)) {
    return false;
  }
  for (size_t i = 0; i < state->count; i++) {
    if (leme_box_contains(state->items[i].cell, lx, ly)) {
      *index = i;
      return true;
    }
  }
  return false;
}

static bool leme_desk_output_ready(const struct leme_output *output) {
  return leme_desk_output_active(output) && output->desk != NULL &&
         output->desk->tree != NULL && output->desk->tree->node.enabled &&
         output->desk->input != NULL && output->desk->input->node.enabled &&
         !output->desk->dirty && leme_desk_geometry_matches(output);
}

bool leme_desk_focused(const struct leme_server *server) {
  if (server == NULL || server->desk == NULL || server->seat == NULL) {
    return false;
  }
  const struct leme_desk *desk = server->desk;
  return desk->focus_output != NULL &&
         desk->focus_output == server->focused_output &&
         desk->focus_tag == desk->focus_output->tags.focused_id &&
         leme_desk_output_ready(desk->focus_output) &&
         !leme_session_locked(server) && server->focused_view == NULL &&
         server->focused_layer == NULL &&
         server->seat->keyboard_state.focused_surface == NULL;
}

static void leme_desk_paths_free(char **paths, size_t count) {
  for (size_t i = 0; paths != NULL && i < count; i++) {
    free(paths[i]);
  }
  free(paths);
}

static void leme_desk_band_finish(struct leme_desk *desk) {
  if (desk->band != NULL) {
    wlr_scene_node_destroy(&desk->band->node);
    desk->band = NULL;
  }
  leme_desk_paths_free(desk->base, desk->base_count);
  desk->base = NULL;
  desk->base_count = 0;
}

void leme_desk_unfocus(struct leme_server *server) {
  if (server == NULL || server->desk == NULL) {
    return;
  }
  struct leme_desk *desk = server->desk;
  leme_desk_context_clear(desk);
  struct leme_desk_output *state =
      desk->focus_output == NULL ? NULL : desk->focus_output->desk;
  for (size_t i = 0; state != NULL && i < state->count; i++) {
    wlr_scene_node_set_enabled(&state->items[i].outline->node, false);
  }
  desk->focus_output = NULL;
  desk->focus_tag = 0;
  leme_desk_paths_free(desk->selected, desk->selected_count);
  desk->selected = NULL;
  desk->selected_count = 0;
  leme_desk_cancel_click(desk);
}

static size_t leme_desk_item_index(const struct leme_desk_output *state,
                                   const char *path) {
  for (size_t i = 0; state != NULL && i < state->count; i++) {
    if (strcmp(path, state->items[i].path) == 0) {
      return i;
    }
  }
  return SIZE_MAX;
}

static bool leme_desk_is_selected(const struct leme_desk *desk,
                                  const char *path) {
  for (size_t i = 0; i < desk->selected_count; i++) {
    if (strcmp(desk->selected[i], path) == 0) {
      return true;
    }
  }
  return false;
}

static size_t leme_desk_selected_index(const struct leme_desk *desk) {
  if (desk->selected_count == 0 || desk->focus_output == NULL) {
    return SIZE_MAX;
  }
  return leme_desk_item_index(desk->focus_output->desk,
                              desk->selected[desk->selected_count - 1]);
}

static void leme_desk_update_selection(struct leme_desk *desk) {
  if (!leme_desk_focused(desk->server)) {
    leme_desk_unfocus(desk->server);
    return;
  }
  struct leme_desk_output *state = desk->focus_output->desk;
  size_t kept = 0;
  for (size_t i = 0; i < desk->selected_count; i++) {
    if (leme_desk_item_index(state, desk->selected[i]) == SIZE_MAX) {
      free(desk->selected[i]);
    } else {
      desk->selected[kept++] = desk->selected[i];
    }
  }
  desk->selected_count = kept;
  for (size_t i = 0; i < state->count; i++) {
    wlr_scene_node_set_enabled(
        &state->items[i].outline->node,
        leme_desk_is_selected(desk, state->items[i].path));
  }
}

static bool leme_desk_select_set(struct leme_desk *desk,
                                 const char *const *paths, size_t count) {
  bool same = count == desk->selected_count;
  for (size_t i = 0; same && i < count; i++) {
    same = strcmp(paths[i], desk->selected[i]) == 0;
  }
  if (!same) {
    char **copy = calloc(count + 1, sizeof(*copy));
    for (size_t i = 0; copy != NULL && i < count; i++) {
      copy[i] = strdup(paths[i]);
      if (copy[i] == NULL) {
        leme_desk_paths_free(copy, i);
        copy = NULL;
      }
    }
    if (copy == NULL) {
      wlr_log(WLR_ERROR, "%s", "leme: could not select desktop entry");
      leme_desk_cancel_click(desk);
      return false;
    }
    leme_desk_paths_free(desk->selected, desk->selected_count);
    desk->selected = copy;
    desk->selected_count = count;
    leme_desk_cancel_click(desk);
  }
  leme_desk_update_selection(desk);
  return true;
}

static bool leme_desk_select(struct leme_desk *desk, const char *path) {
  const char *const paths[] = {path};
  return leme_desk_select_set(desk, paths, path == NULL ? 0 : 1);
}

static bool leme_desk_toggle(struct leme_desk *desk, const char *path) {
  const char **paths = calloc(desk->selected_count + 1, sizeof(*paths));
  if (paths == NULL) {
    return false;
  }
  size_t count = 0;
  for (size_t i = 0; i < desk->selected_count; i++) {
    if (strcmp(desk->selected[i], path) != 0) {
      paths[count++] = desk->selected[i];
    }
  }
  if (count == desk->selected_count) {
    paths[count++] = path;
  }
  const bool ok = leme_desk_select_set(desk, paths, count);
  free(paths);
  return ok;
}

static void leme_desk_raise(struct leme_desk *desk, const char *path) {
  for (size_t i = 0; i + 1 < desk->selected_count; i++) {
    if (strcmp(desk->selected[i], path) == 0) {
      char *raised = desk->selected[i];
      memmove(&desk->selected[i], &desk->selected[i + 1],
              (desk->selected_count - i - 1) * sizeof(*desk->selected));
      desk->selected[desk->selected_count - 1] = raised;
      return;
    }
  }
}

static bool leme_desk_boxes_overlap(struct leme_box a, struct leme_box b) {
  return a.x < (int64_t)b.x + b.width && b.x < (int64_t)a.x + a.width &&
         a.y < (int64_t)b.y + b.height && b.y < (int64_t)a.y + a.height;
}

static void leme_desk_band_update(struct leme_server *server) {
  struct leme_desk *desk = server->desk;
  const struct leme_desk_output *state = desk->focus_output->desk;
  const double x = fmin(desk->press_x, server->cursor->x);
  const double y = fmin(desk->press_y, server->cursor->y);
  const struct leme_box band = {
      (int)x, (int)y, (int)(fmax(desk->press_x, server->cursor->x) - x) + 1,
      (int)(fmax(desk->press_y, server->cursor->y) - y) + 1};
  const char **paths =
      calloc(desk->base_count + state->count + 1, sizeof(*paths));
  if (paths == NULL) {
    return;
  }
  size_t count = 0;
  for (size_t i = 0; i < desk->base_count; i++) {
    paths[count++] = desk->base[i];
  }
  for (size_t i = 0; i < state->count; i++) {
    if (leme_desk_boxes_overlap(band, state->items[i].cell)) {
      bool listed = false;
      for (size_t j = 0; j < desk->base_count && !listed; j++) {
        listed = strcmp(desk->base[j], state->items[i].path) == 0;
      }
      if (!listed) {
        paths[count++] = state->items[i].path;
      }
    }
  }
  (void)leme_desk_select_set(desk, paths, count);
  free(paths);
  if (desk->band != NULL) {
    wlr_scene_node_destroy(&desk->band->node);
  }
  desk->band = wlr_scene_tree_create(server->scene_overlay);
  if (desk->band == NULL || server->config == NULL) {
    return;
  }
  struct leme_desk_look look = {0};
  leme_desk_style_resolve(server->config, LEME_DESK_ELEMENT_SELECTION, 0,
                          &look);
  wlr_scene_node_set_position(&desk->band->node, band.x, band.y);
  const struct leme_box box = {0, 0, band.width, band.height};
  (void)(leme_desk_pixels_rect(desk->band, box, look.background, 0, false) &&
         leme_desk_pixels_rect(desk->band, box, look.border_color, 0, true));
}

static bool leme_desk_pointer_allowed(const struct leme_server *server) {
  return server->cursor != NULL && server->seat != NULL &&
         leme_input_idle(server) &&
         server->seat->pointer_state.button_count == 0;
}

static const char *leme_desk_item_name(const struct leme_desk_item *item) {
  return strrchr(item->path, '/') + 1;
}

static size_t leme_desk_clamp(double value, size_t limit) {
  return value < 1.0 ? 0 : value >= (double)limit ? limit - 1 : (size_t)value;
}

static size_t leme_desk_slot_at(const struct leme_desk_output *state, double lx,
                                double ly) {
  if (state->columns == 0 || state->rows == 0 || !isfinite(lx) ||
      !isfinite(ly)) {
    return SIZE_MAX;
  }
  const double column =
      (lx - state->usable.x - state->metrics.padding) / state->metrics.spacing;
  const double row =
      (ly - state->usable.y - state->metrics.padding) / state->row_height;
  return leme_desk_clamp(row, state->rows) * state->columns +
         leme_desk_clamp(column, state->columns);
}

static void leme_desk_positions_commit(struct leme_desk *desk) {
  if (!leme_desk_positions_save(desk->positions, desk->positions_path)) {
    wlr_log(WLR_ERROR, "%s", "leme: could not save desktop icon positions");
  }
}

static void leme_desk_context_finish(struct leme_desk_context *context) {
  free(context->directory);
  leme_desk_paths_free(context->basenames, context->count);
  *context = (struct leme_desk_context){0};
}

static void leme_desk_context_clear(struct leme_desk *desk) {
  if (desk->context.directory != NULL) {
    leme_desk_menu_close(desk->server);
    leme_desk_prompt_close(desk->server);
    leme_desk_context_finish(&desk->context);
  }
}

static bool leme_desk_catalog_has(const struct leme_desk *desk,
                                  const char *name) {
  for (size_t i = 0; i < leme_desk_catalog_count(desk->catalog); i++) {
    if (strcmp(leme_desk_entry_name(leme_desk_catalog_entry(desk->catalog, i)),
               name) == 0) {
      return true;
    }
  }
  return false;
}

static void leme_desk_context_check(struct leme_desk *desk) {
  const struct leme_desk_context *context = &desk->context;
  if (context->directory == NULL) {
    return;
  }
  bool valid = desk->directory != NULL &&
               strcmp(desk->directory, context->directory) == 0;
  for (size_t i = 0; valid && i < context->count; i++) {
    valid = leme_desk_catalog_has(desk, context->basenames[i]);
  }
  if (!valid) {
    leme_desk_context_clear(desk);
  }
}

static struct leme_output *leme_desk_context_output(struct leme_desk *desk) {
  if (desk->context.directory == NULL || !leme_desk_focused(desk->server)) {
    return NULL;
  }
  struct leme_output *output =
      leme_output_by_public_id(desk->server, desk->context.output);
  return output != NULL && desk->focus_output == output &&
                 output->tags.focused_id == desk->context.tag
             ? output
             : NULL;
}

static bool leme_desk_context_capture(struct leme_desk *desk,
                                      struct leme_output *output, bool entries,
                                      double x, double y) {
  leme_desk_context_clear(desk);
  if (desk->directory == NULL || output->public_id.serial == 0 ||
      desk->context_serial == UINT64_MAX ||
      (entries && desk->selected_count == 0)) {
    return false;
  }
  struct leme_desk_context context = {
      .directory = strdup(desk->directory),
      .basenames = calloc(desk->selected_count + 1, sizeof(char *)),
      .output = output->public_id,
      .tag = output->tags.focused_id,
      .serial = ++desk->context_serial};
  const struct leme_desk_output *state = output->desk;
  const size_t slot = entries ? SIZE_MAX : leme_desk_slot_at(state, x, y);
  bool free_cell = slot != SIZE_MAX;
  for (size_t i = 0; free_cell && i < state->count; i++) {
    free_cell = state->items[i].slot != slot;
  }
  if (free_cell) {
    context.cell =
        (struct leme_desk_wish){.pinned = true,
                                .column = (uint32_t)(slot % state->columns),
                                .row = (uint32_t)(slot / state->columns)};
  }
  bool ok = context.directory != NULL && context.basenames != NULL;
  for (size_t i = 0; ok && entries && i < desk->selected_count; i++) {
    const char *slash = strrchr(desk->selected[i], '/');
    context.basenames[i] = slash == NULL ? NULL : strdup(slash + 1);
    context.count = i + 1;
    ok = context.basenames[i] != NULL &&
         leme_desk_catalog_has(desk, context.basenames[i]);
  }
  if (!ok) {
    leme_desk_context_finish(&context);
    return false;
  }
  desk->context = context;
  return true;
}

size_t leme_desk_context_entries(const struct leme_server *server) {
  return server == NULL || server->desk == NULL ? 0
                                                : server->desk->context.count;
}

static void leme_desk_context_message(struct leme_desk *desk,
                                      struct leme_output *output,
                                      const char *message) {
  wlr_log(WLR_ERROR, "leme: %s", message);
  if (output != NULL &&
      !leme_desk_prompt_open(desk->server, output, LEME_DESK_PROMPT_MESSAGE,
                             message, NULL)) {
    wlr_log(WLR_ERROR, "%s", "leme: cannot display file operation error");
  }
}

bool leme_desk_context_open(struct leme_server *server) {
  struct leme_desk *desk = server == NULL ? NULL : server->desk;
  struct leme_output *output =
      desk == NULL ? NULL : leme_desk_context_output(desk);
  if (output == NULL) {
    return false;
  }
  const struct leme_desk_context *context = &desk->context;
  bool ok = true;
  for (size_t i = 0; ok && i < (context->count == 0 ? 1 : context->count);
       i++) {
    char *path = g_build_filename(
        context->directory, context->count == 0 ? NULL : context->basenames[i],
        NULL);
    ok = leme_desk_open_path(server, path);
    g_free(path);
  }
  if (!ok) {
    leme_desk_context_message(
        desk, output, "Cannot open entry; check its type and permissions");
  }
  return ok;
}

bool leme_desk_context_command(struct leme_server *server,
                               const struct leme_command *command) {
  struct leme_desk *desk = server == NULL ? NULL : server->desk;
  if (desk == NULL || command == NULL) {
    return false;
  }
  switch (command->type) {
  case LEME_COMMAND_GRID_OPEN:
    return leme_desk_context_open(server);
  case LEME_COMMAND_GRID_NEW_FOLDER:
    return leme_desk_context_begin(server, LEME_DESK_FILE_CREATE_FOLDER);
  case LEME_COMMAND_GRID_RENAME:
    return leme_desk_context_begin(server, LEME_DESK_FILE_RENAME);
  case LEME_COMMAND_GRID_TRASH:
    return leme_desk_context_begin(server, LEME_DESK_FILE_TRASH);
  case LEME_COMMAND_SPAWN: {
    if (leme_desk_context_output(desk) == NULL) {
      return false;
    }
    char **argv =
        leme_desk_open_expand(command->argv, desk->context.directory,
                              desk->context.basenames, desk->context.count);
    const bool spawned =
        argv != NULL && leme_process_spawn_detached(server, argv);
    g_strfreev(argv);
    return spawned;
  }
  default:
    return leme_command_execute(server, command);
  }
}

static void leme_desk_context_run(struct leme_desk *desk,
                                  enum leme_desk_file_action action,
                                  const char *name) {
  const struct leme_desk_context *context = &desk->context;
  struct leme_desk_file_result result = {0};
  bool ok = true;
  const size_t runs =
      action == LEME_DESK_FILE_CREATE_FOLDER ? 1 : context->count;
  for (size_t i = 0; ok && i < runs; i++) {
    const struct leme_desk_file_request request = {
        .action = action,
        .directory = context->directory,
        .basename = action == LEME_DESK_FILE_CREATE_FOLDER
                        ? NULL
                        : context->basenames[i],
        .name = name};
    ok = leme_desk_file_run(&request, &result);
    if (ok && action == LEME_DESK_FILE_TRASH) {
      leme_desk_positions_forget(desk->positions, context->basenames[i]);
    }
  }
  if (!ok && action != LEME_DESK_FILE_TRASH) {
    wlr_log(WLR_ERROR, "leme: %s", result.message);
    leme_desk_prompt_status(desk->server, result.message);
    return;
  }
  struct leme_output *output = leme_desk_context_output(desk);
  char *encoded = ok && name != NULL
                      ? g_filename_from_utf8(name, -1, NULL, NULL, NULL)
                      : NULL;
  if (encoded != NULL && action == LEME_DESK_FILE_RENAME) {
    (void)leme_desk_positions_rename(desk->positions, context->basenames[0],
                                     encoded);
  } else if (encoded != NULL && context->cell.pinned) {
    (void)leme_desk_positions_set(desk->positions, encoded,
                                  context->cell.column, context->cell.row);
  }
  leme_desk_positions_commit(desk);
  char *selected = encoded != NULL && action == LEME_DESK_FILE_RENAME
                       ? g_build_filename(context->directory, encoded, NULL)
                       : NULL;
  g_free(encoded);
  if (ok) {
    leme_desk_context_clear(desk);
  }
  if (leme_desk_rescan(desk, false)) {
    leme_desk_sync(desk->server);
  }
  if (!ok) {
    leme_desk_context_message(desk, output, result.message);
  } else if ((action == LEME_DESK_FILE_TRASH || selected != NULL) &&
             leme_desk_focused(desk->server)) {
    (void)leme_desk_select(desk, selected);
  }
  g_free(selected);
}

static void leme_desk_context_submit(struct leme_server *server,
                                     const char *name) {
  struct leme_desk *desk = server->desk;
  if (desk == NULL || leme_desk_context_output(desk) == NULL) {
    return;
  }
  if (!leme_desk_file_name_valid(name)) {
    leme_desk_prompt_status(
        server, "Use a nonempty filename without / or control characters");
    return;
  }
  leme_desk_context_run(desk,
                        desk->context.count == 0 ? LEME_DESK_FILE_CREATE_FOLDER
                                                 : LEME_DESK_FILE_RENAME,
                        name);
}

bool leme_desk_context_begin(struct leme_server *server,
                             enum leme_desk_file_action action) {
  if (server == NULL || server->desk == NULL) {
    return false;
  }
  struct leme_desk *desk = server->desk;
  struct leme_output *output = leme_desk_context_output(desk);
  if (output == NULL || !leme_desk_pointer_allowed(server)) {
    return false;
  }
  const size_t count = desk->context.count;
  if (action == LEME_DESK_FILE_CREATE_FOLDER && count == 0) {
    return leme_desk_prompt_open(server, output, LEME_DESK_PROMPT_CREATE,
                                 "New folder", leme_desk_context_submit);
  }
  if (action == LEME_DESK_FILE_RENAME && count == 1) {
    char *name =
        g_filename_to_utf8(desk->context.basenames[0], -1, NULL, NULL, NULL);
    const bool opened =
        leme_desk_prompt_open(server, output, LEME_DESK_PROMPT_RENAME, name,
                              leme_desk_context_submit);
    g_free(name);
    return opened;
  }
  if (action == LEME_DESK_FILE_TRASH && count > 0) {
    leme_desk_context_run(desk, action, NULL);
    return true;
  }
  return false;
}

static struct leme_output *leme_desk_drop_output(struct leme_server *server) {
  if (server == NULL || server->desk == NULL || server->cursor == NULL) {
    return NULL;
  }
  struct leme_render_hit hit = {0};
  leme_render_at(server, server->cursor->x, server->cursor->y, &hit);
  return hit.surface == NULL && hit.desk_output != NULL &&
                 leme_desk_output_ready(hit.desk_output)
             ? hit.desk_output
             : NULL;
}

static bool leme_desk_listed(char *const *paths, size_t count,
                             const char *path) {
  for (size_t i = 0; i < count; i++) {
    if (strcmp(paths[i], path) == 0) {
      return true;
    }
  }
  return false;
}

static size_t leme_desk_shift(size_t value, size_t add, size_t back,
                              size_t limit) {
  return value + add < back            ? 0
         : value + add - back >= limit ? limit - 1
                                       : value + add - back;
}

static bool leme_desk_drop_plan(const struct leme_output *output,
                                char *const *paths, size_t count,
                                size_t *targets, size_t *folder) {
  const struct leme_desk_output *state = output->desk;
  const double lx = output->server->cursor->x;
  const double ly = output->server->cursor->y;
  size_t index = 0;
  *folder = SIZE_MAX;
  if (leme_desk_at(output, lx, ly, &index) && state->items[index].directory &&
      !leme_desk_listed(paths, count, state->items[index].path)) {
    *folder = index;
    return true;
  }
  const size_t columns = state->columns;
  const size_t drop = leme_desk_slot_at(state, lx, ly);
  const size_t anchor = leme_desk_item_index(state, paths[count - 1]);
  bool *occupied = drop == SIZE_MAX || anchor == SIZE_MAX
                       ? NULL
                       : calloc(columns * state->rows, sizeof(*occupied));
  if (occupied == NULL) {
    return false;
  }
  for (size_t i = 0; i < state->count; i++) {
    if (!leme_desk_listed(paths, count, state->items[i].path)) {
      occupied[state->items[i].slot] = true;
    }
  }
  const size_t from = state->items[anchor].slot;
  for (size_t n = count; n-- > 0;) {
    const size_t item = leme_desk_item_index(state, paths[n]);
    targets[n] = SIZE_MAX;
    if (item == SIZE_MAX) {
      continue;
    }
    const size_t slot = state->items[item].slot;
    targets[n] = leme_desk_grid_nearest_free(
        columns, state->rows, occupied,
        leme_desk_shift(slot % columns, drop % columns, from % columns,
                        columns),
        leme_desk_shift(slot / columns, drop / columns, from / columns,
                        state->rows));
    if (targets[n] != SIZE_MAX) {
      occupied[targets[n]] = true;
    }
  }
  free(occupied);
  return true;
}

void leme_desk_drop_feedback(struct leme_server *server, char *const *paths,
                             size_t count) {
  struct leme_desk *desk = server == NULL ? NULL : server->desk;
  if (desk == NULL) {
    return;
  }
  if (desk->drop != NULL) {
    wlr_scene_node_destroy(&desk->drop->node);
    desk->drop = NULL;
  }
  struct leme_output *output = count == 0 || server->config == NULL
                                   ? NULL
                                   : leme_desk_drop_output(server);
  size_t *targets = output == NULL ? NULL : calloc(count, sizeof(*targets));
  size_t folder = SIZE_MAX;
  if (targets == NULL ||
      !leme_desk_drop_plan(output, paths, count, targets, &folder)) {
    free(targets);
    return;
  }
  desk->drop = wlr_scene_tree_create(server->scene_drag);
  if (desk->drop != NULL) {
    wlr_scene_node_lower_to_bottom(&desk->drop->node);
  }
  struct leme_desk_look look = {0};
  leme_desk_style_resolve(server->config, LEME_DESK_ELEMENT_ICON,
                          LEME_DESK_STATE_BIT(LEME_DESK_STATE_DROP), &look);
  const struct leme_desk_output *state = output->desk;
  if (desk->drop != NULL && folder != SIZE_MAX) {
    (void)(leme_desk_pixels_rect(desk->drop, state->items[folder].cell,
                                 look.background, 0, false) &&
           leme_desk_pixels_rect(desk->drop, state->items[folder].cell,
                                 look.border_color, 0, true));
  }
  for (size_t i = 0; desk->drop != NULL && folder == SIZE_MAX && i < count;
       i++) {
    struct leme_box cell = {0};
    if (targets[i] != SIZE_MAX &&
        leme_desk_grid_cell(state->usable, &state->metrics, state->row_height,
                            targets[i], &cell)) {
      (void)leme_desk_pixels_rect(desk->drop, cell, look.border_color, 0, true);
    }
  }
  free(targets);
}

static bool leme_desk_drop_move(struct leme_desk *desk,
                                struct leme_output *output, char *const *paths,
                                size_t count, size_t folder) {
  const struct leme_desk_output *state = output->desk;
  struct leme_desk_file_result result = {0};
  bool ok = true;
  for (size_t i = 0; ok && i < count; i++) {
    const size_t item = leme_desk_item_index(state, paths[i]);
    if (item == SIZE_MAX) {
      continue;
    }
    const struct leme_desk_file_request request = {
        .action = LEME_DESK_FILE_MOVE_INTO,
        .directory = desk->directory,
        .basename = leme_desk_item_name(&state->items[item]),
        .name = leme_desk_item_name(&state->items[folder])};
    ok = leme_desk_file_run(&request, &result);
    if (ok) {
      leme_desk_positions_forget(desk->positions, request.basename);
    }
  }
  leme_desk_positions_commit(desk);
  if (leme_desk_rescan(desk, false)) {
    leme_desk_sync(desk->server);
  }
  if (!ok) {
    leme_desk_context_message(desk, output, result.message);
  }
  return ok;
}

static bool leme_desk_drop_arrange(struct leme_desk *desk,
                                   const struct leme_desk_output *state,
                                   char *const *paths, size_t count,
                                   const size_t *targets) {
  struct leme_desk_positions *positions =
      leme_desk_positions_load(NULL, desk->directory);
  if (positions == NULL) {
    return false;
  }
  for (size_t i = 0; i < state->count; i++) {
    size_t slot = state->items[i].slot;
    for (size_t n = 0; n < count; n++) {
      if (targets[n] != SIZE_MAX &&
          strcmp(paths[n], state->items[i].path) == 0) {
        slot = targets[n];
      }
    }
    (void)leme_desk_positions_set(
        positions, leme_desk_item_name(&state->items[i]),
        (uint32_t)(slot % state->columns), (uint32_t)(slot / state->columns));
  }
  leme_desk_positions_destroy(desk->positions);
  desk->positions = positions;
  leme_desk_positions_commit(desk);
  leme_desk_mark_dirty(desk);
  leme_desk_sync(desk->server);
  return true;
}

bool leme_desk_drop(struct leme_server *server, char *const *paths,
                    size_t count) {
  struct leme_output *output =
      count == 0 ? NULL : leme_desk_drop_output(server);
  size_t *targets = output == NULL ? NULL : calloc(count, sizeof(*targets));
  size_t folder = SIZE_MAX;
  bool ok = targets != NULL &&
            leme_desk_drop_plan(output, paths, count, targets, &folder);
  if (ok) {
    ok = folder == SIZE_MAX
             ? leme_desk_drop_arrange(server->desk, output->desk, paths, count,
                                      targets)
             : leme_desk_drop_move(server->desk, output, paths, count, folder);
  }
  free(targets);
  return ok;
}

static bool leme_desk_press_moved(const struct leme_server *server) {
  const struct leme_desk *desk = server->desk;
  return hypot(server->cursor->x - desk->press_x,
               server->cursor->y - desk->press_y) > 4.0;
}

static struct wlr_scene_tree *leme_desk_drag_image(struct leme_desk *desk) {
  struct leme_server *server = desk->server;
  struct wlr_scene_tree *image =
      server->scene_drag == NULL ? NULL
                                 : wlr_scene_tree_create(server->scene_drag);
  const struct leme_desk_output *state = desk->focus_output->desk;
  for (size_t i = 0; image != NULL && i < state->count; i++) {
    const struct leme_desk_item *item = &state->items[i];
    if (!leme_desk_is_selected(desk, item->path)) {
      continue;
    }
    int x = 0;
    int y = 0;
    (void)wlr_scene_node_coords(&item->tree->node, &x, &y);
    struct wlr_scene_tree *copy = leme_animation_snapshot(item->tree, image);
    if (copy != NULL) {
      wlr_scene_node_set_position(&copy->node, x - (int)desk->press_x,
                                  y - (int)desk->press_y);
    }
  }
  return image;
}

static const struct leme_desk_owner leme_desk_press_owner;

static void leme_desk_press_motion(struct leme_server *server) {
  struct leme_desk *desk = server->desk;
  if (desk == NULL || server->cursor == NULL || !desk->press_valid ||
      !leme_desk_press_moved(server)) {
    return;
  }
  leme_desk_cancel_click(desk);
  leme_desk_owner_handoff(server, &leme_desk_press_owner, BTN_LEFT);
  if (!leme_desk_focused(server) ||
      !leme_desk_drag_begin(server, desk->press_time, desk->selected,
                            desk->selected_count, leme_desk_drag_image(desk))) {
    wlr_log(WLR_ERROR, "%s", "leme: cannot start desktop drag");
  }
}

static void leme_desk_press_cancel(struct leme_server *server) {
  if (server->desk != NULL) {
    leme_desk_cancel_click(server->desk);
  }
}

static bool
leme_desk_press_release(struct leme_server *server,
                        const struct wlr_pointer_button_event *event) {
  struct leme_desk *desk = server->desk;
  if (event->state != WL_POINTER_BUTTON_STATE_RELEASED ||
      event->button != BTN_LEFT || desk == NULL) {
    return false;
  }
  leme_desk_owner_release(server, &leme_desk_press_owner);
  bool valid = desk->press_valid && !leme_desk_press_moved(server) &&
               leme_desk_focused(server) && leme_desk_pointer_allowed(server) &&
               leme_input_modifiers(server) == 0;
  struct leme_render_hit current = {0};
  size_t index = 0;
  if (valid) {
    leme_render_at(server, server->cursor->x, server->cursor->y, &current);
    valid = current.desk_output == desk->focus_output &&
            leme_desk_at(current.desk_output, server->cursor->x,
                         server->cursor->y, &index) &&
            leme_desk_is_selected(desk,
                                  current.desk_output->desk->items[index].path);
  }
  desk->press_valid = false;
  if (!valid) {
    desk->clicked = false;
    return true;
  }
  char *path = strdup(current.desk_output->desk->items[index].path);
  if (path == NULL) {
    return true;
  }
  if (desk->press_single) {
    (void)leme_desk_select(desk, path);
  }
  const bool activate = desk->clicked &&
                        event->time_msec - desk->click_time <= 400U &&
                        hypot(desk->press_x - desk->click_x,
                              desk->press_y - desk->click_y) <= 4.0;
  desk->clicked = !activate;
  desk->click_time = event->time_msec;
  desk->click_x = desk->press_x;
  desk->click_y = desk->press_y;
  if (activate) {
    (void)leme_desk_open_path(server, path);
  }
  free(path);
  return true;
}

static const struct leme_desk_owner leme_desk_press_owner = {
    .motion = leme_desk_press_motion,
    .button = leme_desk_press_release,
    .cancel = leme_desk_press_cancel,
};

static void leme_desk_band_motion(struct leme_server *server) {
  struct leme_desk *desk = server->desk;
  if (desk == NULL || server->cursor == NULL || !leme_desk_focused(server) ||
      (desk->band == NULL && hypot(server->cursor->x - desk->press_x,
                                   server->cursor->y - desk->press_y) <= 4.0)) {
    return;
  }
  leme_desk_band_update(server);
}

static void leme_desk_band_cancel(struct leme_server *server) {
  if (server->desk != NULL) {
    leme_desk_band_finish(server->desk);
  }
}

static const struct leme_desk_owner leme_desk_band_owner;

static bool
leme_desk_band_release(struct leme_server *server,
                       const struct wlr_pointer_button_event *event) {
  struct leme_desk *desk = server->desk;
  if (event->state != WL_POINTER_BUTTON_STATE_RELEASED ||
      event->button != BTN_LEFT || desk == NULL) {
    return false;
  }
  leme_desk_owner_release(server, &leme_desk_band_owner);
  leme_desk_band_motion(server);
  if (desk->band == NULL && desk->base_count == 0 &&
      leme_desk_focused(server)) {
    (void)leme_desk_select(desk, NULL);
  }
  leme_desk_band_finish(desk);
  return true;
}

static const struct leme_desk_owner leme_desk_band_owner = {
    .motion = leme_desk_band_motion,
    .button = leme_desk_band_release,
    .cancel = leme_desk_band_cancel,
};

static bool leme_desk_band_begin(struct leme_server *server, bool add) {
  struct leme_desk *desk = server->desk;
  leme_desk_band_finish(desk);
  if (add && desk->selected_count > 0) {
    desk->base = calloc(desk->selected_count, sizeof(*desk->base));
    for (size_t i = 0; desk->base != NULL && i < desk->selected_count; i++) {
      desk->base[i] = strdup(desk->selected[i]);
      desk->base_count = i + 1;
      if (desk->base[i] == NULL) {
        leme_desk_band_finish(desk);
        return false;
      }
    }
    if (desk->base == NULL) {
      return false;
    }
  }
  desk->press_x = server->cursor->x;
  desk->press_y = server->cursor->y;
  leme_desk_owner_claim(server, &leme_desk_band_owner, BTN_LEFT);
  return true;
}

static bool leme_desk_step(const struct leme_desk_output *state, size_t index,
                           enum leme_direction direction, size_t *next) {
  bool *occupied = calloc(state->columns * state->rows, sizeof(*occupied));
  if (occupied == NULL) {
    return false;
  }
  for (size_t i = 0; i < state->count; i++) {
    occupied[state->items[i].slot] = true;
  }
  size_t slot = 0;
  const bool found =
      leme_desk_grid_step(state->columns, state->rows, occupied,
                          state->items[index].slot, direction, &slot);
  free(occupied);
  for (size_t i = 0; found && i < state->count; i++) {
    if (state->items[i].slot == slot) {
      *next = i;
      return true;
    }
  }
  return false;
}

static void leme_desk_take_focus(struct leme_desk *desk,
                                 struct leme_output *output) {
  leme_desk_context_clear(desk);
  if (desk->focus_output != output ||
      desk->focus_tag != output->tags.focused_id) {
    leme_desk_unfocus(desk->server);
  }
  leme_layer_release_for_view(desk->server);
  leme_view_clear_focus(desk->server);
  desk->focus_output = output;
  desk->focus_tag = output->tags.focused_id;
}

bool leme_desk_focus(struct leme_server *server) {
  struct leme_output *output = server == NULL || server->desk == NULL
                                   ? NULL
                                   : leme_output_focused(server);
  if (output == NULL || !leme_desk_output_ready(output) ||
      leme_session_locked(server)) {
    return false;
  }
  if (!leme_desk_focused(server)) {
    leme_desk_take_focus(server->desk, output);
    leme_desk_update_selection(server->desk);
  }
  return leme_desk_focused(server);
}

bool leme_desk_open_menu(struct leme_server *server) {
  if (!leme_desk_focus(server)) {
    return false;
  }
  struct leme_desk *desk = server->desk;
  struct leme_output *output = desk->focus_output;
  const struct leme_desk_output *state = output->desk;
  const size_t index = leme_desk_selected_index(desk);
  const bool entry = index != SIZE_MAX;
  const struct leme_box anchor =
      entry ? state->items[index].cell
            : (struct leme_box){state->usable.x + state->metrics.padding,
                                state->usable.y + state->metrics.padding, 0, 0};
  leme_desk_cancel_click(desk);
  if (leme_desk_context_capture(desk, output, entry, NAN, NAN) &&
      leme_desk_menu_open_desktop(server, output, entry,
                                  anchor.x + anchor.width / 2.0,
                                  anchor.y + anchor.height / 2.0)) {
    return true;
  }
  leme_desk_context_clear(desk);
  return false;
}

static bool leme_desk_select_items(struct leme_desk *desk,
                                   const struct leme_desk_output *state,
                                   size_t anchor, size_t low, size_t high,
                                   size_t cursor) {
  const char **paths = calloc(state->count + 1, sizeof(*paths));
  if (paths == NULL) {
    return false;
  }
  size_t count = 0;
  if (anchor != SIZE_MAX) {
    paths[count++] = state->items[anchor].path;
  }
  for (size_t i = 0; i < state->count; i++) {
    if (i != anchor && i != cursor && state->items[i].slot >= low &&
        state->items[i].slot <= high) {
      paths[count++] = state->items[i].path;
    }
  }
  if (cursor != SIZE_MAX && cursor != anchor) {
    paths[count++] = state->items[cursor].path;
  }
  const bool ok = leme_desk_select_set(desk, paths, count);
  free(paths);
  return ok;
}

bool leme_desk_grid_act(struct leme_server *server,
                        enum leme_desk_grid_action action) {
  if (!leme_desk_focused(server)) {
    return false;
  }
  struct leme_desk *desk = server->desk;
  struct leme_output *output = desk->focus_output;
  leme_desk_context_clear(desk);
  leme_desk_cancel_click(desk);
  switch (action) {
  case LEME_DESK_GRID_OPEN:
    for (size_t i = 0; i < desk->selected_count; i++) {
      (void)leme_desk_open_path(server, desk->selected[i]);
    }
    return true;
  case LEME_DESK_GRID_CLEAR:
    return leme_desk_select(desk, NULL);
  case LEME_DESK_GRID_SELECT_ALL:
    return leme_desk_select_items(desk, output->desk, SIZE_MAX, 0, SIZE_MAX,
                                  SIZE_MAX);
  case LEME_DESK_GRID_TRASH:
  case LEME_DESK_GRID_RENAME:
  case LEME_DESK_GRID_NEW_FOLDER: {
    const enum leme_desk_file_action file =
        action == LEME_DESK_GRID_TRASH    ? LEME_DESK_FILE_TRASH
        : action == LEME_DESK_GRID_RENAME ? LEME_DESK_FILE_RENAME
                                          : LEME_DESK_FILE_CREATE_FOLDER;
    if (leme_desk_context_capture(
            desk, output, action != LEME_DESK_GRID_NEW_FOLDER, NAN, NAN) &&
        leme_desk_context_begin(server, file)) {
      return true;
    }
    leme_desk_context_clear(desk);
    return false;
  }
  }
  return false;
}

bool leme_desk_grid_select(struct leme_server *server,
                           enum leme_direction direction, bool extend) {
  if (!leme_desk_focused(server)) {
    return false;
  }
  struct leme_desk *desk = server->desk;
  leme_desk_context_clear(desk);
  leme_desk_cancel_click(desk);
  const struct leme_desk_output *state = desk->focus_output->desk;
  if (state->count == 0) {
    return true;
  }
  const size_t index = leme_desk_selected_index(desk);
  size_t next = 0;
  if (index == SIZE_MAX) {
    for (size_t i = 1; i < state->count; i++) {
      next = state->items[i].slot < state->items[next].slot ? i : next;
    }
  } else if (!leme_desk_step(state, index, direction, &next)) {
    return true;
  }
  const size_t anchor = extend && desk->selected_count > 0
                            ? leme_desk_item_index(state, desk->selected[0])
                            : SIZE_MAX;
  if (anchor == SIZE_MAX) {
    return leme_desk_select(desk, state->items[next].path);
  }
  const size_t from = state->items[anchor].slot;
  const size_t to = state->items[next].slot;
  return leme_desk_select_items(desk, state, anchor, from < to ? from : to,
                                from < to ? to : from, next);
}

bool leme_desk_handle_button(struct leme_server *server,
                             const struct leme_render_hit *hit,
                             const struct wlr_pointer_button_event *event) {
  struct leme_desk *desk = server->desk;
  if (desk == NULL || event->state != WL_POINTER_BUTTON_STATE_PRESSED) {
    return false;
  }
  const uint32_t modifiers = leme_input_modifiers(server);
  if (hit == NULL ||
      (event->button != BTN_LEFT && event->button != BTN_RIGHT) ||
      hit->desk_output == NULL || !leme_desk_pointer_allowed(server) ||
      (modifiers != 0 &&
       (modifiers != WLR_MODIFIER_CTRL || event->button != BTN_LEFT)) ||
      (event->button == BTN_RIGHT &&
       leme_input_protocols_shortcuts_inhibited(server)) ||
      !leme_desk_output_ready(hit->desk_output)) {
    leme_desk_cancel_click(desk);
    return false;
  }
  struct leme_render_hit current = {0};
  leme_render_at(server, server->cursor->x, server->cursor->y, &current);
  if (current.desk_output != hit->desk_output) {
    leme_desk_cancel_click(desk);
    return false;
  }
  struct leme_output *output = current.desk_output;
  leme_desk_take_focus(desk, output);
  size_t index = 0;
  const char *path =
      leme_desk_at(output, server->cursor->x, server->cursor->y, &index)
          ? output->desk->items[index].path
          : NULL;
  const bool kept = path != NULL && leme_desk_is_selected(desk, path);
  if (event->button == BTN_RIGHT) {
    leme_desk_cancel_click(desk);
    const enum leme_desktop_menu menu =
        path != NULL ? LEME_DESKTOP_MENU_FILES : LEME_DESKTOP_MENU_BLANK;
    if (server->config->desktop_menus[menu].count == 0) {
      leme_desk_owner_swallow(server, BTN_RIGHT);
      return true;
    }
    const bool selected = kept || leme_desk_select(desk, path);
    if (!selected ||
        !leme_desk_context_capture(desk, output, path != NULL,
                                   server->cursor->x, server->cursor->y) ||
        !leme_desk_menu_open_desktop(server, output, path != NULL,
                                     server->cursor->x, server->cursor->y)) {
      leme_desk_owner_swallow(server, BTN_RIGHT);
      leme_desk_context_clear(desk);
      wlr_log(WLR_ERROR, "%s", "leme: cannot open desktop menu");
    }
    return true;
  }
  const bool add = modifiers == WLR_MODIFIER_CTRL;
  if (path == NULL) {
    if (!leme_desk_band_begin(server, add)) {
      leme_desk_owner_swallow(server, BTN_LEFT);
    }
    return true;
  }
  leme_desk_owner_claim(server, &leme_desk_press_owner, BTN_LEFT);
  if (add) {
    (void)leme_desk_toggle(desk, path);
    desk->press_valid = false;
    return true;
  }
  desk->press_single = kept && desk->selected_count > 1;
  if (kept) {
    leme_desk_raise(desk, path);
  }
  const bool selected = kept || leme_desk_select(desk, path);
  desk->press_x = server->cursor->x;
  desk->press_y = server->cursor->y;
  desk->press_time = event->time_msec;
  desk->press_valid = selected;
  return true;
}

bool leme_desk_handle_key(struct leme_server *server, xkb_keysym_t keysym,
                          uint32_t modifiers) {
  if (!leme_desk_focused(server) || !leme_desk_pointer_allowed(server) ||
      leme_input_protocols_shortcuts_inhibited(server) ||
      (modifiers & (WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT |
                    WLR_MODIFIER_LOGO)) != 0) {
    return false;
  }
  if (keysym == XKB_KEY_Return || keysym == XKB_KEY_KP_Enter ||
      keysym == XKB_KEY_Escape) {
    (void)leme_desk_grid_act(server, keysym == XKB_KEY_Escape
                                         ? LEME_DESK_GRID_CLEAR
                                         : LEME_DESK_GRID_OPEN);
    return true;
  }
  enum leme_direction direction = LEME_DIRECTION_LEFT;
  switch (keysym) {
  case XKB_KEY_Left:
    direction = LEME_DIRECTION_LEFT;
    break;
  case XKB_KEY_Right:
    direction = LEME_DIRECTION_RIGHT;
    break;
  case XKB_KEY_Up:
    direction = LEME_DIRECTION_UP;
    break;
  case XKB_KEY_Down:
    direction = LEME_DIRECTION_DOWN;
    break;
  default:
    return false;
  }
  (void)leme_desk_grid_select(server, direction, false);
  return true;
}

static void leme_desk_sync_output(struct leme_desk *desk,
                                  struct leme_output *output) {
  const bool active = leme_desk_output_active(output);
  if (output->desk == NULL) {
    if (!active) {
      return;
    }
    output->desk = calloc(1, sizeof(*output->desk));
    if (output->desk == NULL) {
      return;
    }
    output->desk->dirty = true;
  }
  struct leme_desk_output *state = output->desk;
  if (!active) {
    if (state->tree != NULL) {
      if (state->dirty || !leme_desk_geometry_matches(output)) {
        leme_desk_output_clear_scene(state);
      } else {
        wlr_scene_node_set_enabled(&state->tree->node, false);
      }
    }
    return;
  }
  if (state->tree != NULL && !state->dirty &&
      leme_desk_geometry_matches(output)) {
    wlr_scene_node_set_enabled(&state->tree->node, true);
    return;
  }
  struct leme_desk_look look = {0};
  leme_desk_style_resolve(desk->server->config, LEME_DESK_ELEMENT_ICON, 0,
                          &look);
  int pixels = 0;
  if (!leme_desk_grid_pixels(output->wlr_output->scale, look.size, &pixels) ||
      !leme_desk_build_output(desk, output, pixels)) {
    if (state->tree != NULL) {
      wlr_scene_node_set_enabled(&state->tree->node,
                                 leme_desk_geometry_matches(output));
      if (state->input != NULL) {
        wlr_scene_node_set_enabled(&state->input->node, false);
      }
    }
    state->dirty = true;
    wlr_log(WLR_ERROR, "%s", "leme: could not build desktop scene");
  }
}

bool leme_desk_init(struct leme_server *server) {
  if (server == NULL || server->scene_desktop == NULL || server->desk != NULL) {
    return false;
  }
  struct leme_desk *desk = calloc(1, sizeof(*desk));
  if (desk == NULL) {
    return false;
  }
  desk->server = server;
  desk->icons = leme_desk_icons_create();
  desk->positions_path = leme_desk_positions_path();
  if (desk->icons == NULL) {
    g_free(desk->positions_path);
    free(desk);
    return false;
  }
  server->desk = desk;
  return true;
}

void leme_desk_sync(struct leme_server *server) {
  if (server == NULL || server->desk == NULL || server->scene_desktop == NULL ||
      server->outputs.next == NULL || server->desk->syncing) {
    return;
  }
  struct leme_desk *desk = server->desk;
  desk->syncing = true;
  struct leme_output *output;
  bool active = false;
  wl_list_for_each(output, &server->outputs, link) {
    active = active || leme_desk_output_active(output);
  }
  if (!desk->loaded && active && !leme_desk_rescan(desk, true)) {
    desk->syncing = false;
    return;
  }
  wl_list_for_each(output, &server->outputs, link) {
    leme_desk_sync_output(desk, output);
  }
  leme_desk_update_selection(desk);
  if (leme_desk_context_output(desk) == NULL) {
    leme_desk_context_clear(desk);
  }
  leme_desk_icons_prune(desk->icons);
  if (desk->prune_watches) {
    leme_desk_icons_for_each_path(desk->icons, leme_desk_watch_asset, desk);
    leme_desk_watch_end_files(desk->watch);
    desk->prune_watches = false;
  }
  desk->syncing = false;
}

bool leme_desk_refresh(struct leme_server *server) {
  if (server == NULL || server->desk == NULL || !server->desk->loaded) {
    return true;
  }
  struct leme_desk *desk = server->desk;
  if (desk->syncing || !leme_desk_rescan(desk, true)) {
    return false;
  }
  leme_desk_icons_clear(desk->icons);
  leme_desk_sync(server);
  return true;
}

void leme_desk_output_finish(struct leme_output *output) {
  if (output == NULL || output->desk == NULL) {
    return;
  }
  if (output->server != NULL && output->server->desk != NULL &&
      output->server->desk->focus_output == output) {
    leme_desk_unfocus(output->server);
  }
  leme_desk_output_clear_scene(output->desk);
  free(output->desk);
  output->desk = NULL;
  if (output->server != NULL && output->server->desk != NULL) {
    leme_desk_icons_prune(output->server->desk->icons);
  }
}

void leme_desk_finish(struct leme_server *server) {
  leme_desk_menu_finish(server);
  leme_desk_prompt_finish(server);
  if (server == NULL || server->desk == NULL) {
    return;
  }
  struct leme_desk *desk = server->desk;
  leme_desk_unfocus(server);
  leme_desk_band_finish(desk);
  leme_desk_drop_feedback(server, NULL, 0);
  server->desk = NULL;
  leme_desk_watch_destroy(desk->watch);
  if (server->outputs.next != NULL) {
    struct leme_output *output;
    wl_list_for_each(output, &server->outputs, link) {
      leme_desk_output_finish(output);
    }
  }
  leme_desk_icons_destroy(desk->icons);
  pango_cairo_font_map_set_default(NULL);
  leme_desk_catalog_destroy(desk->catalog);
  leme_desk_positions_destroy(desk->positions);
  g_free(desk->positions_path);
  free(desk->directory);
  free(desk);
}
