#include "desktop/menu.h"

#include "config/style.h"
#include "core/server.h"
#include "core/command.h"
#include "desktop/buttons.h"
#include "desktop/desktop.h"
#include "desktop/owner.h"
#include "desktop/pixels.h"
#include "protocols/desktop.h"
#include "protocols/input.h"
#include "protocols/session.h"
#include "output/output.h"
#include "shell/container.h"
#include "shell/view.h"
#include "workspace/tag.h"

#include <glib.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_pointer.h>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>

struct leme_desk_menu_panel {
  struct leme_desk_menu_item items[LEME_DESK_MENU_ITEMS_MAX];
  struct wlr_scene_tree *rows[LEME_DESK_MENU_ITEMS_MAX];
  struct wlr_scene_tree *variants[LEME_DESK_MENU_ITEMS_MAX][2];
  struct wlr_scene_tree *tree;
  struct wlr_scene_tree *highlight;
  struct leme_box box;
  size_t count;
  size_t selected;
  size_t first;
};

struct leme_desk_menu {
  struct leme_server *server;
  struct leme_desk_target target;
  struct leme_box area;
  double scale;
  struct leme_desk_menu_panel panels[2];
  struct wl_event_source *timer;
  enum leme_desktop_menu desktop;
  size_t active;
  size_t pending;
  size_t submenu_row;
  double x;
  double y;
  double motion_x;
  double motion_y;
  bool opening;
};

static int leme_desk_menu_timeout(void *data);
static const struct leme_desk_owner leme_desk_menu_owner;

_Static_assert(LEME_DESKTOP_MENU_ENTRIES_MAX <= LEME_DESK_MENU_ITEMS_MAX,
               "a configured desktop menu fits in one panel");

static size_t leme_desk_menu_files(struct leme_server *server, bool entry,
                                   struct leme_desk_menu_item *items,
                                   size_t capacity) {
  if (server->config == NULL) {
    return 0;
  }
  const struct leme_desktop_menu_list *list =
      &server->config->desktop_menus[entry ? LEME_DESKTOP_MENU_FILES
                                           : LEME_DESKTOP_MENU_BLANK];
  const size_t count = list->count < capacity ? list->count : capacity;
  for (size_t i = 0; i < count; i++) {
    const struct leme_desktop_menu_entry *row = &list->entries[i];
    if (row->label == NULL) {
      items[i] = (struct leme_desk_menu_item){.separator = true};
      continue;
    }
    items[i] = (struct leme_desk_menu_item){
        .action = LEME_DESK_MENU_COMMAND,
        .value = i,
        .enabled = row->command.type != LEME_COMMAND_GRID_RENAME ||
                   leme_desk_context_entries(server) == 1};
    leme_desk_utf8_copy(items[i].label, sizeof(items[i].label), row->label);
  }
  return count;
}

size_t leme_desk_menu_build(struct leme_server *server,
                            struct leme_desk_target target,
                            enum leme_desk_menu_list list,
                            struct leme_desk_menu_item *items,
                            size_t capacity) {
  if (server == NULL || items == NULL || capacity == 0) {
    return 0;
  }
  if (list == LEME_DESK_MENU_DESKTOP || list == LEME_DESK_MENU_ENTRY) {
    return leme_desk_menu_files(server, list == LEME_DESK_MENU_ENTRY, items,
                                capacity);
  }
  struct leme_view *view = leme_desk_target_member(server, target);
  const struct leme_output *owner = leme_view_output(view);
  const struct leme_tag *tag = leme_ownership_tag(view);
  if (view == NULL || owner == NULL) {
    return 0;
  }
  const struct leme_container *container =
      target.kind == LEME_DESK_TARGET_CONTAINER ? view->container : NULL;
  struct leme_desk_menu_item rows[LEME_DESK_MENU_ITEMS_MAX] = {0};
  size_t count = 0;
  if (list == LEME_DESK_MENU_TAGS) {
    for (uint16_t id = 1;
         id <= owner->tags.max_tags && count < LEME_ARRAY_LENGTH(rows); id++) {
      rows[count] = (struct leme_desk_menu_item){
          .action = LEME_DESK_MENU_TAG,
          .value = id,
          .enabled = true,
          .checked = id == (tag == NULL ? owner->tags.focused_id : tag->id)};
      const int length =
          snprintf(rows[count].label, sizeof(rows[count].label), "%u", id);
      if (length < 0 || (size_t)length >= sizeof(rows[count].label)) {
        return 0;
      }
      count++;
    }
  } else if (list == LEME_DESK_MENU_OUTPUTS) {
    const struct leme_output *output;
    wl_list_for_each(output, &server->outputs, link) {
      if (output->wlr_output == NULL || !output->wlr_output->enabled ||
          count == LEME_ARRAY_LENGTH(rows)) {
        continue;
      }
      rows[count] =
          (struct leme_desk_menu_item){.action = LEME_DESK_MENU_OUTPUT,
                                       .value = output->public_id.serial,
                                       .enabled = true,
                                       .checked = output == owner};
      leme_desk_utf8_copy(rows[count].label, sizeof(rows[count].label),
                          output->wlr_output->name);
      count++;
    }
  } else if (list == LEME_DESK_MENU_LAYOUTS && container != NULL) {
    rows[count++] = (struct leme_desk_menu_item){.label = "Dwindle",
                                                 .value = LEME_LAYOUT_DWINDLE};
    rows[count++] = (struct leme_desk_menu_item){
        .label = "Master and stack", .value = LEME_LAYOUT_MASTER_STACK};
    rows[count++] = (struct leme_desk_menu_item){
        .label = "Accordion", .value = LEME_LAYOUT_ACCORDION};
    for (size_t i = 0; i < count; i++) {
      rows[i].action = LEME_DESK_MENU_LAYOUT;
      rows[i].enabled = true;
      rows[i].checked = rows[i].value == (uint64_t)container->layout.kind;
    }
  } else if (list == LEME_DESK_MENU_MAIN) {
    const bool maximized =
        container == NULL
            ? leme_view_is_maximized(view)
            : container->placement.kind == LEME_VIEW_PLACEMENT_MAXIMIZED;
    rows[count++] = (struct leme_desk_menu_item){
        .action = LEME_DESK_MENU_MINIMIZE,
        .enabled = container == NULL
                       ? leme_view_minimize_available(view)
                       : leme_container_minimize_available(container)};
    leme_desk_utf8_copy(rows[0].label, sizeof(rows[0].label),
                        container == NULL ? "Minimize" : "Minimize group");
    rows[count++] = (struct leme_desk_menu_item){
        .action = LEME_DESK_MENU_MAXIMIZE,
        .enabled = maximized || leme_view_can_maximize(view)};
    leme_desk_utf8_copy(rows[1].label, sizeof(rows[1].label),
                        container == NULL
                            ? (maximized ? "Restore" : "Maximize")
                            : (maximized ? "Restore group" : "Maximize group"));
    if (container != NULL) {
      rows[count++] = (struct leme_desk_menu_item){.separator = true};
      rows[count++] =
          (struct leme_desk_menu_item){.label = "Layout",
                                       .action = LEME_DESK_MENU_SUBMENU,
                                       .submenu = LEME_DESK_MENU_LAYOUTS,
                                       .enabled = true};
      rows[count] = (struct leme_desk_menu_item){
          .action = LEME_DESK_MENU_DETACH, .enabled = true};
      char title[52] = {0};
      const char *name = leme_view_title(view);
      leme_desk_utf8_copy(title, sizeof(title), name == NULL ? "Window" : name);
      const int length = snprintf(rows[count].label, sizeof(rows[count].label),
                                  "Detach \"%s\"", title);
      if (length < 0 || (size_t)length >= sizeof(rows[count].label)) {
        return 0;
      }
      count++;
      rows[count++] =
          (struct leme_desk_menu_item){.label = "Dissolve group",
                                       .action = LEME_DESK_MENU_DISSOLVE,
                                       .enabled = true};
    } else {
      rows[count++] =
          (struct leme_desk_menu_item){.label = "Fullscreen",
                                       .action = LEME_DESK_MENU_FULLSCREEN,
                                       .enabled = true};
      rows[count++] = (struct leme_desk_menu_item){.separator = true};
      rows[count++] =
          (struct leme_desk_menu_item){.label = "Keep on all tags",
                                       .action = LEME_DESK_MENU_STICKY,
                                       .enabled = true,
                                       .checked = leme_view_is_sticky(view)};
      rows[count++] =
          (struct leme_desk_menu_item){.label = "Move to tag",
                                       .action = LEME_DESK_MENU_SUBMENU,
                                       .submenu = LEME_DESK_MENU_TAGS,
                                       .enabled = true};
      size_t outputs = 0;
      const struct leme_output *output;
      wl_list_for_each(output, &server->outputs, link) {
        outputs += output->wlr_output != NULL && output->wlr_output->enabled;
      }
      if (outputs > 1) {
        rows[count++] =
            (struct leme_desk_menu_item){.label = "Move to output",
                                         .action = LEME_DESK_MENU_SUBMENU,
                                         .submenu = LEME_DESK_MENU_OUTPUTS,
                                         .enabled = true};
      }
    }
    rows[count++] = (struct leme_desk_menu_item){.separator = true};
    rows[count] = (struct leme_desk_menu_item){.action = LEME_DESK_MENU_CLOSE,
                                               .enabled = true};
    leme_desk_utf8_copy(rows[count].label, sizeof(rows[count].label),
                        container == NULL ? "Close" : "Close all");
    count++;
  }
  if (count > capacity) {
    count = capacity;
  }
  memcpy(items, rows, count * sizeof(*items));
  return count;
}

size_t leme_desk_menu_step(const struct leme_desk_menu_item *items,
                           size_t count, size_t selected, int direction) {
  if (items == NULL || count == 0) {
    return SIZE_MAX;
  }
  size_t next = selected < count ? selected : (direction > 0 ? count - 1 : 0);
  for (size_t i = 0; i < count; i++) {
    next = direction > 0 ? (next == count - 1 ? 0 : next + 1)
                         : (next == 0 ? count - 1 : next - 1);
    if (items[next].enabled && !items[next].separator) {
      return next;
    }
  }
  return SIZE_MAX;
}

struct leme_box leme_desk_menu_place(struct leme_box area, double x, double y,
                                     int width, int height) {
  if (area.width <= 0 || area.height <= 0 || width <= 0 || height <= 0 ||
      !isfinite(x) || !isfinite(y)) {
    return (struct leme_box){0};
  }
  width = width > area.width ? area.width : width;
  height = height > area.height ? area.height : height;
  x = fmax(area.x, fmin(x, fmin(INT_MAX, (double)area.x + area.width - width)));
  y = fmax(area.y,
           fmin(y, fmin(INT_MAX, (double)area.y + area.height - height)));
  return (struct leme_box){(int)x, (int)y, width, height};
}

struct leme_box leme_desk_menu_place_submenu(struct leme_box area,
                                             struct leme_box parent, int row_y,
                                             int width, int height) {
  double x = (double)parent.x + parent.width;
  if (x + width > (double)area.x + area.width) {
    x = (double)parent.x - width;
  }
  return leme_desk_menu_place(area, x, row_y, width, height);
}

static void leme_desk_menu_panel_clear(struct leme_desk_menu_panel *panel) {
  if (panel->tree != NULL) {
    wlr_scene_node_destroy(&panel->tree->node);
  }
  *panel = (struct leme_desk_menu_panel){.selected = SIZE_MAX};
}

static void leme_desk_menu_panel_layout(struct leme_desk_menu_panel *panel) {
  int y = 4;
  for (size_t i = 0; i < panel->count; i++) {
    const int height = panel->items[i].separator ? 9 : 28;
    const bool visible =
        i >= panel->first && y + height <= panel->box.height - 4;
    wlr_scene_node_set_enabled(&panel->rows[i]->node, visible);
    if (i >= panel->first) {
      wlr_scene_node_set_position(&panel->rows[i]->node, 4, y);
      y += height;
    }
    if (panel->variants[i][1] != NULL) {
      const bool hovered = i == panel->selected;
      wlr_scene_node_set_enabled(&panel->variants[i][0]->node, !hovered);
      wlr_scene_node_set_enabled(&panel->variants[i][1]->node, hovered);
    }
  }
  const bool selected = panel->selected < panel->count &&
                        panel->rows[panel->selected]->node.enabled;
  wlr_scene_node_set_enabled(&panel->highlight->node, selected);
  if (selected) {
    wlr_scene_node_set_position(&panel->highlight->node, 4,
                                panel->rows[panel->selected]->node.y);
  }
}

static bool leme_desk_menu_row(struct wlr_scene_tree *tree,
                               struct wlr_buffer *label, struct leme_box size,
                               const struct leme_desk_menu_item *item,
                               enum leme_desk_menu_list list,
                               const struct leme_desk_look *look, double scale,
                               int width) {
  const int shown = size.height > 28 ? 28 : size.height;
  return (look->background[3] <= 0.0f ||
          leme_desk_pixels_rect(tree, (struct leme_box){0, 0, width - 8, 28},
                                look->background, 0, false)) &&
         leme_desk_pixels_place_buffer(
             tree, label,
             (struct leme_box){20, (28 - shown) / 2, size.width, shown}) &&
         (!item->checked ||
          leme_desk_pixels_place_line(
              tree, list == LEME_DESK_MENU_MAIN ? "✓" : "●", look->font,
              look->color, scale, (struct leme_box){2, 0, 16, 28}, false)) &&
         (item->action != LEME_DESK_MENU_SUBMENU ||
          leme_desk_pixels_place_line(tree, "›", look->font, look->color, scale,
                                      (struct leme_box){width - 24, 0, 16, 28},
                                      false));
}

static bool leme_desk_menu_panel_build(enum leme_desk_menu_list list,
                                       struct leme_desk_menu *menu, double x,
                                       double y) {
  const size_t index = list == LEME_DESK_MENU_MAIN ||
                               list == LEME_DESK_MENU_DESKTOP ||
                               list == LEME_DESK_MENU_ENTRY
                           ? 0
                           : 1;
  struct leme_desk_menu_panel *panel = &menu->panels[index];
  leme_desk_menu_panel_clear(panel);
  panel->count =
      leme_desk_menu_build(menu->server, menu->target, list, panel->items,
                           LEME_ARRAY_LENGTH(panel->items));
  if (panel->count == 0 || menu->area.width < 64 || menu->area.height < 36) {
    return false;
  }
  struct wlr_buffer *labels[LEME_DESK_MENU_ITEMS_MAX][2] = {0};
  struct leme_box sizes[LEME_DESK_MENU_ITEMS_MAX] = {0};
  const struct leme_config *config = menu->server->config;
  struct leme_desk_look frame = {0};
  struct leme_desk_look separator = {0};
  struct leme_desk_look looks[3] = {0};
  leme_desk_style_resolve(config, LEME_DESK_ELEMENT_MENU, 0, &frame);
  leme_desk_style_resolve(config, LEME_DESK_ELEMENT_MENU_SEPARATOR, 0,
                          &separator);
  leme_desk_style_resolve(config, LEME_DESK_ELEMENT_MENU_ITEM, 0, &looks[0]);
  leme_desk_style_resolve(config, LEME_DESK_ELEMENT_MENU_ITEM,
                          LEME_DESK_STATE_BIT(LEME_DESK_STATE_HOVER),
                          &looks[1]);
  leme_desk_style_resolve(config, LEME_DESK_ELEMENT_MENU_ITEM,
                          LEME_DESK_STATE_BIT(LEME_DESK_STATE_DISABLED),
                          &looks[2]);
  const bool hover_text =
      memcmp(looks[0].color, looks[1].color, sizeof(looks[0].color)) != 0 ||
      memcmp(looks[0].background, looks[1].background,
             sizeof(looks[0].background)) != 0;
  int width = 180;
  int height = 8;
  const int text_width = (menu->area.width < 320 ? menu->area.width : 320) - 48;
  bool ok = false;
  for (size_t i = 0; i < panel->count; i++) {
    const struct leme_desk_menu_item *item = &panel->items[i];
    height += item->separator ? 9 : 28;
    if (item->separator) {
      continue;
    }
    const struct leme_desk_look *look = item->enabled ? &looks[0] : &looks[2];
    labels[i][0] =
        leme_desk_pixels_line(item->label, look->font, look->color, text_width,
                              menu->scale, &sizes[i].width, &sizes[i].height);
    if (labels[i][0] == NULL) {
      goto cleanup;
    }
    if (item->enabled && hover_text) {
      int hover_width = 0;
      int hover_height = 0;
      labels[i][1] = leme_desk_pixels_line(
          item->label, looks[1].font, looks[1].color, text_width, menu->scale,
          &hover_width, &hover_height);
      if (labels[i][1] == NULL) {
        goto cleanup;
      }
    }
    if (width < sizes[i].width + 48) {
      width = sizes[i].width + 48;
    }
  }
  panel->box =
      index == 0 ? leme_desk_menu_place(menu->area, x, y, width, height)
                 : leme_desk_menu_place_submenu(menu->area, menu->panels[0].box,
                                                (int)y, width, height);
  panel->tree = wlr_scene_tree_create(menu->server->scene_overlay);
  if (panel->tree == NULL) {
    goto cleanup;
  }
  wlr_scene_node_set_position(&panel->tree->node, panel->box.x, panel->box.y);
  const struct leme_box background = {0, 0, panel->box.width,
                                      panel->box.height};
  if (!leme_desk_pixels_rect(panel->tree, background, frame.background,
                             frame.radius, false) ||
      !leme_desk_pixels_rect(panel->tree, background, frame.border_color,
                             frame.radius, true)) {
    goto cleanup;
  }
  panel->highlight = wlr_scene_tree_create(panel->tree);
  const struct leme_box row = {0, 0, panel->box.width - 8, 28};
  if (panel->highlight == NULL ||
      !leme_desk_pixels_rect(panel->highlight, row, looks[1].background,
                             frame.radius, false) ||
      !leme_desk_pixels_rect(panel->highlight, row, looks[1].border_color,
                             frame.radius, true)) {
    goto cleanup;
  }
  for (size_t i = 0; i < panel->count; i++) {
    panel->rows[i] = wlr_scene_tree_create(panel->tree);
    if (panel->rows[i] == NULL) {
      goto cleanup;
    }
    const struct leme_desk_menu_item *item = &panel->items[i];
    if (item->separator) {
      if (!leme_desk_pixels_rect(
              panel->rows[i], (struct leme_box){4, 4, panel->box.width - 16, 1},
              separator.color, 0, false)) {
        goto cleanup;
      }
      continue;
    }
    for (size_t v = 0; v < 2; v++) {
      if (labels[i][v] == NULL) {
        continue;
      }
      panel->variants[i][v] = wlr_scene_tree_create(panel->rows[i]);
      const struct leme_desk_look *look =
          v == 1 ? &looks[1] : (item->enabled ? &looks[0] : &looks[2]);
      if (panel->variants[i][v] == NULL ||
          !leme_desk_menu_row(panel->variants[i][v], labels[i][v], sizes[i],
                              item, list, look, menu->scale,
                              panel->box.width)) {
        goto cleanup;
      }
    }
  }
  leme_desk_menu_panel_layout(panel);
  ok = true;
cleanup:
  for (size_t i = 0; i < panel->count; i++) {
    for (size_t v = 0; v < 2; v++) {
      if (labels[i][v] != NULL) {
        wlr_buffer_drop(labels[i][v]);
      }
    }
  }
  if (!ok) {
    leme_desk_menu_panel_clear(panel);
  }
  return ok;
}

bool leme_desk_menu_active(const struct leme_server *server) {
  return server != NULL && server->desk_menu != NULL &&
         server->desk_menu->panels[0].tree != NULL;
}

void leme_desk_menu_close(struct leme_server *server) {
  if (server == NULL || server->desk_menu == NULL) {
    return;
  }
  leme_desk_owner_release(server, &leme_desk_menu_owner);
  if (server->desk_menu->timer != NULL) {
    (void)wl_event_source_timer_update(server->desk_menu->timer, 0);
  }
  server->desk_menu->pending = SIZE_MAX;
  server->desk_menu->submenu_row = SIZE_MAX;
  server->desk_menu->active = 0;
  server->desk_menu->opening = false;
  for (size_t i = 0; i < LEME_ARRAY_LENGTH(server->desk_menu->panels); i++) {
    leme_desk_menu_panel_clear(&server->desk_menu->panels[i]);
  }
}

void leme_desk_menu_finish(struct leme_server *server) {
  if (server != NULL) {
    leme_desk_menu_close(server);
    if (server->desk_menu != NULL && server->desk_menu->timer != NULL) {
      wl_event_source_remove(server->desk_menu->timer);
    }
    free(server->desk_menu);
    server->desk_menu = NULL;
  }
}

void leme_desk_menu_invalidate(struct leme_server *server,
                               struct leme_desk_target target) {
  if (target.id.serial != 0 && leme_desk_menu_active(server) &&
      leme_desk_target_equal(server->desk_menu->target, target)) {
    leme_desk_menu_close(server);
  }
}

static bool leme_desk_menu_open_at(struct leme_server *server,
                                   struct leme_desk_target target,
                                   struct leme_output *output,
                                   enum leme_desk_menu_list list, double x,
                                   double y) {
  leme_desk_menu_close(server);
  if (server == NULL || server->config == NULL ||
      server->scene_overlay == NULL || server->seat == NULL ||
      server->display == NULL || !isfinite(x) || !isfinite(y) ||
      leme_session_locked(server)) {
    return false;
  }
  if (output == NULL || output->wlr_output == NULL ||
      !output->wlr_output->enabled) {
    return false;
  }
  leme_desk_buttons_cancel(server);
  if (server->desk_menu == NULL) {
    server->desk_menu = calloc(1, sizeof(*server->desk_menu));
    if (server->desk_menu == NULL) {
      return false;
    }
  }
  struct leme_desk_menu *menu = server->desk_menu;
  menu->server = server;
  menu->target = target;
  menu->area = output->usable_box;
  menu->scale = (double)output->wlr_output->scale;
  menu->active = 0;
  menu->pending = SIZE_MAX;
  menu->submenu_row = SIZE_MAX;
  menu->x = x;
  menu->y = y;
  menu->motion_x = x;
  menu->motion_y = y;
  if (menu->timer == NULL) {
    menu->timer =
        wl_event_loop_add_timer(wl_display_get_event_loop(server->display),
                                leme_desk_menu_timeout, menu);
  }
  if (menu->timer == NULL || !leme_desk_menu_panel_build(list, menu, x, y)) {
    return false;
  }
  wlr_seat_pointer_notify_clear_focus(server->seat);
  leme_input_protocols_update_pointer_focus(server);
  leme_desktop_cursor_default(server);
  menu->opening = true;
  leme_desk_owner_claim(server, &leme_desk_menu_owner, BTN_RIGHT);
  return true;
}

bool leme_desk_menu_open(struct leme_server *server,
                         struct leme_desk_target target, double x, double y) {
  leme_desk_menu_close(server);
  if (server == NULL || !isfinite(x) || !isfinite(y)) {
    return false;
  }
  struct leme_view *view = leme_desk_target_member(server, target);
  const struct leme_tag *tag = leme_ownership_tag(view);
  if (view == NULL || !view->mapped || tag == NULL ||
      tag->layout.kind != LEME_LAYOUT_DESKTOP) {
    return false;
  }
  struct leme_output *output = leme_output_at(server, x, y);
  if (output == NULL) {
    output = leme_view_output(view);
  }
  leme_desk_unfocus(server);
  return leme_desk_menu_open_at(server, target, output, LEME_DESK_MENU_MAIN, x,
                                y);
}

bool leme_desk_menu_open_desktop(struct leme_server *server,
                                 struct leme_output *output, bool entry,
                                 double x, double y) {
  if (!leme_desk_output_active(output) || output->server != server) {
    return false;
  }
  if (!leme_desk_menu_open_at(
          server, (struct leme_desk_target){0}, output,
          entry ? LEME_DESK_MENU_ENTRY : LEME_DESK_MENU_DESKTOP, x, y)) {
    return false;
  }
  server->desk_menu->desktop =
      entry ? LEME_DESKTOP_MENU_FILES : LEME_DESKTOP_MENU_BLANK;
  return true;
}

static void leme_desk_menu_select(struct leme_desk_menu_panel *panel,
                                  size_t row) {
  panel->selected = row;
  if (row < panel->first) {
    panel->first = row;
  }
  leme_desk_menu_panel_layout(panel);
  while (row < panel->count && !panel->rows[row]->node.enabled &&
         panel->first < row) {
    panel->first++;
    leme_desk_menu_panel_layout(panel);
  }
}

static void leme_desk_menu_submenu_close(struct leme_desk_menu *menu) {
  leme_desk_menu_panel_clear(&menu->panels[1]);
  menu->submenu_row = SIZE_MAX;
  menu->active = 0;
}

static void leme_desk_menu_submenu_open(struct leme_desk_menu *menu,
                                        bool keyboard) {
  struct leme_desk_menu_panel *main = &menu->panels[0];
  const size_t row = main->selected;
  if (row >= main->count || main->items[row].action != LEME_DESK_MENU_SUBMENU) {
    return;
  }
  if (menu->submenu_row != row &&
      !leme_desk_menu_panel_build(main->items[row].submenu, menu, 0,
                                  (double)main->box.y +
                                      main->rows[row]->node.y)) {
    leme_desk_menu_submenu_close(menu);
    return;
  }
  menu->submenu_row = row;
  if (keyboard) {
    menu->active = 1;
    struct leme_desk_menu_panel *sub = &menu->panels[1];
    leme_desk_menu_select(
        sub, leme_desk_menu_step(sub->items, sub->count, SIZE_MAX, 1));
  }
}

static int leme_desk_menu_timeout(void *data) {
  struct leme_desk_menu *menu = data;
  if (leme_desk_menu_active(menu->server) && menu->pending != SIZE_MAX &&
      menu->pending == menu->panels[0].selected) {
    leme_desk_menu_submenu_open(menu, false);
  }
  menu->pending = SIZE_MAX;
  return 0;
}

static size_t leme_desk_menu_at(struct leme_desk_menu *menu, size_t *row) {
  const struct wlr_cursor *cursor = menu->server->cursor;
  *row = SIZE_MAX;
  if (cursor == NULL) {
    return SIZE_MAX;
  }
  for (size_t p = 2; p-- > 0;) {
    const struct leme_desk_menu_panel *panel = &menu->panels[p];
    if (panel->tree == NULL ||
        !leme_box_contains(panel->box, cursor->x, cursor->y)) {
      continue;
    }
    for (size_t i = panel->first; i < panel->count; i++) {
      const struct wlr_scene_node *node = &panel->rows[i]->node;
      if (node->enabled && panel->items[i].enabled &&
          !panel->items[i].separator &&
          leme_box_contains((struct leme_box){panel->box.x,
                                              panel->box.y + node->y,
                                              panel->box.width, 28},
                            cursor->x, cursor->y)) {
        *row = i;
        break;
      }
    }
    return p;
  }
  return SIZE_MAX;
}

static void leme_desk_menu_motion(struct leme_server *server) {
  struct leme_desk_menu *menu = server->desk_menu;
  wlr_seat_pointer_notify_clear_focus(server->seat);
  if (server->cursor == NULL || (server->cursor->x == menu->motion_x &&
                                 server->cursor->y == menu->motion_y)) {
    return;
  }
  menu->motion_x = server->cursor->x;
  menu->motion_y = server->cursor->y;
  size_t row = SIZE_MAX;
  const size_t p = leme_desk_menu_at(menu, &row);
  if (p < 2) {
    menu->active = p;
    leme_desk_menu_select(&menu->panels[p], row);
  }
  const bool submenu =
      p == 0 && row < menu->panels[0].count &&
      menu->panels[0].items[row].action == LEME_DESK_MENU_SUBMENU;
  if (submenu && menu->submenu_row != row) {
    if (menu->pending != row) {
      menu->pending = row;
      if (wl_event_source_timer_update(menu->timer, 150) < 0) {
        menu->pending = SIZE_MAX;
      }
    }
  } else {
    menu->pending = SIZE_MAX;
    (void)wl_event_source_timer_update(menu->timer, 0);
    if (p == 0 && !submenu) {
      leme_desk_menu_submenu_close(menu);
    }
  }
}

static void leme_desk_menu_run(struct leme_desk_menu *menu,
                               struct leme_desk_menu_item item) {
  if (item.action == LEME_DESK_MENU_SUBMENU) {
    leme_desk_menu_submenu_open(menu, true);
    return;
  }
  struct leme_server *server = menu->server;
  const struct leme_desk_target target = menu->target;
  const enum leme_desktop_menu desktop = menu->desktop;
  leme_desk_menu_close(server);
  if (item.action == LEME_DESK_MENU_COMMAND) {
    const struct leme_desktop_menu_list *list =
        server->config == NULL ? NULL : &server->config->desktop_menus[desktop];
    if (list != NULL && item.value < list->count) {
      (void)leme_desk_context_command(server,
                                      &list->entries[item.value].command);
    }
    return;
  }
  struct leme_view *view = leme_desk_target_member(server, target);
  if (view == NULL || !view->mapped) {
    return;
  }
  leme_output_set_focused(server, leme_view_output(view), false);
  leme_view_focus(view);
  if (server->focused_view != view) {
    return;
  }
  struct leme_command command = {0};
  switch (item.action) {
  case LEME_DESK_MENU_MINIMIZE:
    leme_desk_target_act(server, target, LEME_RENDER_DECORATION_MINIMIZE);
    return;
  case LEME_DESK_MENU_FULLSCREEN:
    command.type = LEME_COMMAND_TOGGLE_FULLSCREEN;
    break;
  case LEME_DESK_MENU_STICKY:
    command.type = LEME_COMMAND_TOGGLE_STICKY;
    break;
  case LEME_DESK_MENU_DETACH:
    command.type = LEME_COMMAND_CONTAINER_DETACH;
    break;
  case LEME_DESK_MENU_DISSOLVE:
    command.type = LEME_COMMAND_CONTAINER_DISSOLVE;
    break;
  case LEME_DESK_MENU_TAG:
    if (item.value == 0 || item.value > UINT16_MAX) {
      return;
    }
    command.type = LEME_COMMAND_MOVE_VIEW_TO_TAG;
    command.tag_id = (uint16_t)item.value;
    break;
  case LEME_DESK_MENU_OUTPUT: {
    struct leme_output *output =
        leme_output_by_public_id(server, (struct leme_public_id){item.value});
    if (output == NULL || output->wlr_output == NULL ||
        !output->wlr_output->enabled) {
      return;
    }
    command.type = LEME_COMMAND_MOVE_VIEW_TO_OUTPUT;
    command.text = output->wlr_output->name;
    break;
  }
  case LEME_DESK_MENU_LAYOUT:
    if (item.value != LEME_LAYOUT_DWINDLE &&
        item.value != LEME_LAYOUT_MASTER_STACK &&
        item.value != LEME_LAYOUT_ACCORDION) {
      return;
    }
    command.type = LEME_COMMAND_CONTAINER_LAYOUT;
    command.layout = (enum leme_layout_kind)item.value;
    break;
  case LEME_DESK_MENU_MAXIMIZE:
    leme_desk_target_act(server, target, LEME_RENDER_DECORATION_MAXIMIZE);
    return;
  case LEME_DESK_MENU_CLOSE:
    leme_desk_target_act(server, target, LEME_RENDER_DECORATION_CLOSE);
    return;
  default:
    return;
  }
  (void)leme_command_execute(server, &command);
}

static bool leme_desk_menu_key(struct leme_server *server, xkb_keysym_t keysym,
                               uint32_t modifiers, const char *utf8,
                               size_t length) {
  (void)utf8;
  (void)length;
  if ((modifiers & (WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT |
                    WLR_MODIFIER_LOGO)) != 0) {
    leme_desk_menu_close(server);
    return false;
  }
  struct leme_desk_menu *menu = server->desk_menu;
  menu->pending = SIZE_MAX;
  (void)wl_event_source_timer_update(menu->timer, 0);
  struct leme_desk_menu_panel *panel = &menu->panels[menu->active];
  switch (keysym) {
  case XKB_KEY_Up:
  case XKB_KEY_Down:
    if (menu->active == 0) {
      leme_desk_menu_submenu_close(menu);
    }
    leme_desk_menu_select(
        panel, leme_desk_menu_step(panel->items, panel->count, panel->selected,
                                   keysym == XKB_KEY_Down ? 1 : -1));
    break;
  case XKB_KEY_Left:
  case XKB_KEY_Escape:
    if (menu->active == 1) {
      leme_desk_menu_submenu_close(menu);
    } else if (keysym == XKB_KEY_Escape) {
      leme_desk_menu_close(server);
    }
    break;
  case XKB_KEY_Right:
    if (menu->active == 0) {
      leme_desk_menu_submenu_open(menu, true);
    }
    break;
  case XKB_KEY_Return:
  case XKB_KEY_KP_Enter:
  case XKB_KEY_space:
    if (panel->selected < panel->count) {
      leme_desk_menu_run(menu, panel->items[panel->selected]);
    }
    break;
  default:
    leme_desk_menu_close(server);
    return false;
  }
  return true;
}

static bool
leme_desk_menu_button(struct leme_server *server,
                      const struct wlr_pointer_button_event *event) {
  struct leme_desk_menu *menu = server->desk_menu;
  size_t row = SIZE_MAX;
  const size_t p = leme_desk_menu_at(menu, &row);
  if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
    if (p == SIZE_MAX) {
      leme_desk_menu_close(server);
    }
    return true;
  }
  if (menu->opening && event->button == BTN_RIGHT) {
    menu->opening = false;
    if (server->cursor == NULL || hypot(server->cursor->x - menu->x,
                                        server->cursor->y - menu->y) <= 4.0) {
      return true;
    }
  }
  if (p < 2 && row < menu->panels[p].count) {
    menu->active = p;
    leme_desk_menu_select(&menu->panels[p], row);
    leme_desk_menu_run(menu, menu->panels[p].items[row]);
  }
  return true;
}

static void leme_desk_menu_axis(struct leme_server *server,
                                const struct wlr_pointer_axis_event *event) {
  size_t row = SIZE_MAX;
  struct leme_desk_menu *menu = server->desk_menu;
  const size_t p = leme_desk_menu_at(menu, &row);
  if (p == SIZE_MAX || event->orientation != WL_POINTER_AXIS_VERTICAL_SCROLL ||
      event->delta == 0) {
    return;
  }
  struct leme_desk_menu_panel *panel = &menu->panels[p];
  if (event->delta > 0 && !panel->rows[panel->count - 1]->node.enabled &&
      panel->first + 1 < panel->count) {
    panel->first++;
  } else if (event->delta < 0 && panel->first > 0) {
    panel->first--;
  } else {
    return;
  }
  if (p == 0) {
    leme_desk_menu_submenu_close(menu);
  }
  panel->selected = SIZE_MAX;
  leme_desk_menu_panel_layout(panel);
  menu->motion_x = NAN;
  leme_desk_menu_motion(server);
}

static const struct leme_desk_owner leme_desk_menu_owner = {
    .modal = true,
    .motion = leme_desk_menu_motion,
    .button = leme_desk_menu_button,
    .axis = leme_desk_menu_axis,
    .key = leme_desk_menu_key,
    .cancel = leme_desk_menu_close,
};
