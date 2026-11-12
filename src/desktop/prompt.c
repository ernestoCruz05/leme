#include "desktop/prompt.h"

#include "config/style.h"
#include "core/server.h"
#include "desktop/menu.h"
#include "desktop/buttons.h"
#include "desktop/owner.h"
#include "desktop/pixels.h"
#include "output/output.h"
#include "protocols/desktop.h"
#include "protocols/input.h"
#include "protocols/session.h"

#include <glib.h>
#include <stdlib.h>
#include <string.h>
#include <linux/input-event-codes.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

struct leme_desk_prompt {
  struct leme_server *server;
  struct wlr_scene_tree *tree;
  struct leme_box box;
  struct leme_box controls[3];
  double scale;
  enum leme_desk_prompt_kind kind;
  struct leme_desk_prompt_text text;
  char message[512];
  size_t focus;
  size_t pressed;
  void (*submit)(struct leme_server *server, const char *name);
};

static bool leme_desk_prompt_printable(const char *utf8, size_t length) {
  if (utf8 == NULL || length > LEME_DESK_PROMPT_TEXT_MAX ||
      !g_utf8_validate(utf8, (gssize)length, NULL)) {
    return false;
  }
  for (const char *p = utf8; p < utf8 + length; p = g_utf8_next_char(p)) {
    if (g_unichar_iscntrl(g_utf8_get_char(p))) {
      return false;
    }
  }
  return true;
}

static bool leme_desk_prompt_text_init(struct leme_desk_prompt_text *text,
                                       const char *initial) {
  if (text == NULL) {
    return false;
  }
  *text = (struct leme_desk_prompt_text){0};
  if (initial == NULL) {
    return false;
  }
  const size_t length = strnlen(initial, sizeof(text->bytes));
  if (!leme_desk_prompt_printable(initial, length)) {
    return false;
  }
  memcpy(text->bytes, initial, length);
  text->cursor = length;
  text->select_all = length > 0;
  return true;
}

static bool leme_desk_prompt_text_edit(struct leme_desk_prompt_text *text,
                                       xkb_keysym_t keysym, uint32_t modifiers,
                                       const char *utf8, size_t length) {
  if (text == NULL) {
    return false;
  }
  const size_t used = strnlen(text->bytes, sizeof(text->bytes));
  if (used > LEME_DESK_PROMPT_TEXT_MAX || text->cursor > used) {
    return false;
  }
  const uint32_t chords =
      modifiers & (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO);
  if (chords == WLR_MODIFIER_CTRL &&
      (keysym == XKB_KEY_a || keysym == XKB_KEY_A)) {
    text->select_all = true;
    text->cursor = used;
    return true;
  }
  if (chords != 0) {
    return false;
  }
  switch (keysym) {
  case XKB_KEY_Home:
    text->cursor = 0;
    text->select_all = false;
    return true;
  case XKB_KEY_End:
    text->cursor = used;
    text->select_all = false;
    return true;
  case XKB_KEY_Left:
    if (text->select_all) {
      text->cursor = 0;
    } else if (text->cursor > 0) {
      const char *previous =
          g_utf8_find_prev_char(text->bytes, text->bytes + text->cursor);
      text->cursor = (size_t)(previous - text->bytes);
    }
    text->select_all = false;
    return true;
  case XKB_KEY_Right:
    if (text->select_all) {
      text->cursor = used;
    } else if (text->cursor < used) {
      text->cursor =
          (size_t)(g_utf8_next_char(text->bytes + text->cursor) - text->bytes);
    }
    text->select_all = false;
    return true;
  case XKB_KEY_BackSpace:
  case XKB_KEY_Delete: {
    if (text->select_all) {
      *text = (struct leme_desk_prompt_text){0};
      return true;
    }
    size_t start = text->cursor;
    size_t end = start;
    if (keysym == XKB_KEY_BackSpace && start > 0) {
      start = (size_t)(g_utf8_find_prev_char(text->bytes, text->bytes + start) -
                       text->bytes);
    } else if (keysym == XKB_KEY_Delete && end < used) {
      end = (size_t)(g_utf8_next_char(text->bytes + end) - text->bytes);
    }
    memmove(text->bytes + start, text->bytes + end, used - end + 1);
    text->cursor = start;
    return true;
  }
  default:
    break;
  }
  if (length == 0) {
    return false;
  }
  const size_t remaining = text->select_all ? 0 : used;
  if (!leme_desk_prompt_printable(utf8, length) ||
      length > LEME_DESK_PROMPT_TEXT_MAX - remaining) {
    return true;
  }
  const size_t cursor = text->select_all ? 0 : text->cursor;
  if (text->select_all) {
    text->bytes[0] = '\0';
  }
  memmove(text->bytes + cursor + length, text->bytes + cursor,
          remaining - cursor + 1);
  memcpy(text->bytes + cursor, utf8, length);
  text->cursor = cursor + length;
  text->select_all = false;
  return true;
}

static const struct leme_desk_owner leme_desk_prompt_owner;

static bool leme_desk_prompt_active(const struct leme_server *server) {
  return server != NULL && server->desk_prompt != NULL &&
         server->desk_prompt->tree != NULL;
}

void leme_desk_prompt_close(struct leme_server *server) {
  if (server == NULL || server->desk_prompt == NULL) {
    return;
  }
  leme_desk_owner_release(server, &leme_desk_prompt_owner);
  struct leme_desk_prompt *prompt = server->desk_prompt;
  if (prompt->tree != NULL) {
    wlr_scene_node_destroy(&prompt->tree->node);
    prompt->tree = NULL;
  }
  prompt->submit = NULL;
  prompt->pressed = SIZE_MAX;
}

void leme_desk_prompt_finish(struct leme_server *server) {
  if (server != NULL) {
    leme_desk_prompt_close(server);
    free(server->desk_prompt);
    server->desk_prompt = NULL;
  }
}

static bool leme_desk_prompt_draw(struct leme_desk_prompt *prompt) {
  struct wlr_scene_tree *tree =
      wlr_scene_tree_create(prompt->server->scene_overlay);
  if (tree == NULL) {
    return false;
  }
  wlr_scene_node_set_enabled(&tree->node, false);
  wlr_scene_node_set_position(&tree->node, prompt->box.x, prompt->box.y);
  const struct leme_config *config = prompt->server->config;
  const uint32_t focus = LEME_DESK_STATE_BIT(LEME_DESK_STATE_FOCUS);
  struct leme_desk_look frame = {0};
  struct leme_desk_look field = {0};
  struct leme_desk_look selection = {0};
  leme_desk_style_resolve(config, LEME_DESK_ELEMENT_PROMPT, 0, &frame);
  leme_desk_style_resolve(config, LEME_DESK_ELEMENT_PROMPT_FIELD,
                          prompt->focus == 0 ? focus : 0, &field);
  leme_desk_style_resolve(config, LEME_DESK_ELEMENT_SELECTION, 0, &selection);
  const struct leme_box background = {0, 0, prompt->box.width,
                                      prompt->box.height};
  const char *title =
      prompt->kind == LEME_DESK_PROMPT_CREATE
          ? "New folder"
          : (prompt->kind == LEME_DESK_PROMPT_RENAME ? "Rename"
                                                     : "File operation failed");
  if (!leme_desk_pixels_rect(tree, background, frame.background, frame.radius,
                             false) ||
      !leme_desk_pixels_rect(tree, background, frame.border_color, frame.radius,
                             true) ||
      !leme_desk_pixels_place_line(
          tree, title, frame.font, frame.color, prompt->scale,
          (struct leme_box){12, 12, prompt->box.width - 24, 28}, false)) {
    goto fail;
  }
  if (prompt->kind != LEME_DESK_PROMPT_MESSAGE) {
    const struct leme_box box = prompt->controls[0];
    if (!leme_desk_pixels_place_line(
            tree, "Name", frame.font, frame.color, prompt->scale,
            (struct leme_box){12, 40, box.width, 28}, false) ||
        (field.background[3] > 0.0f &&
         !leme_desk_pixels_rect(tree, box, field.background, frame.radius,
                                false)) ||
        !leme_desk_pixels_rect(tree, box, field.border_color, frame.radius,
                               true)) {
      goto fail;
    }
    if (prompt->focus == 0 && prompt->text.select_all &&
        !leme_desk_pixels_rect(tree,
                               (struct leme_box){box.x + 2, box.y + 2,
                                                 box.width - 4, box.height - 4},
                               selection.background, 0, false)) {
      goto fail;
    }
    int height = 0;
    struct wlr_buffer *buffer = leme_desk_pixels_entry(
        prompt->text.bytes, field.font, field.color, box.width - 8,
        prompt->scale, prompt->focus == 0 ? prompt->text.cursor : SIZE_MAX,
        &height);
    if (buffer == NULL) {
      goto fail;
    }
    height = height < 24 ? height : 24;
    const bool placed = leme_desk_pixels_place_buffer(
        tree, buffer,
        (struct leme_box){box.x + 4, box.y + (28 - height) / 2, box.width - 8,
                          height});
    wlr_buffer_drop(buffer);
    if (!placed) {
      goto fail;
    }
  }
  if (!leme_desk_pixels_place_line(
          tree, prompt->message, frame.font, frame.color, prompt->scale,
          (struct leme_box){12, prompt->box.height - 76, prompt->box.width - 24,
                            28},
          false)) {
    goto fail;
  }
  for (size_t i = prompt->kind == LEME_DESK_PROMPT_MESSAGE ? 2 : 1; i < 3;
       i++) {
    const char *label =
        i == 1 ? "Cancel"
               : (prompt->kind == LEME_DESK_PROMPT_MESSAGE
                      ? "Dismiss"
                      : (prompt->kind == LEME_DESK_PROMPT_CREATE ? "Create"
                                                                 : "Rename"));
    struct leme_desk_look button = {0};
    leme_desk_style_resolve(config, LEME_DESK_ELEMENT_PROMPT_BUTTON,
                            prompt->focus == i ? focus : 0, &button);
    if ((button.background[3] > 0.0f &&
         !leme_desk_pixels_rect(tree, prompt->controls[i], button.background,
                                frame.radius, false)) ||
        !leme_desk_pixels_rect(tree, prompt->controls[i], button.border_color,
                               frame.radius, true) ||
        !leme_desk_pixels_place_line(tree, label, button.font, button.color,
                                     prompt->scale, prompt->controls[i],
                                     true)) {
      goto fail;
    }
  }
  if (prompt->tree != NULL) {
    wlr_scene_node_destroy(&prompt->tree->node);
  }
  prompt->tree = tree;
  wlr_scene_node_set_enabled(&tree->node, true);
  return true;
fail:
  wlr_scene_node_destroy(&tree->node);
  return false;
}

static void leme_desk_prompt_update(struct leme_desk_prompt *prompt) {
  if (!leme_desk_prompt_draw(prompt)) {
    wlr_log(WLR_ERROR, "%s", "leme: cannot draw file prompt");
    leme_desk_prompt_close(prompt->server);
  }
}

void leme_desk_prompt_status(struct leme_server *server, const char *message) {
  if (leme_desk_prompt_active(server)) {
    struct leme_desk_prompt *prompt = server->desk_prompt;
    leme_desk_utf8_copy(prompt->message, sizeof(prompt->message), message);
    leme_desk_prompt_update(prompt);
  }
}

bool leme_desk_prompt_open(struct leme_server *server,
                           struct leme_output *output,
                           enum leme_desk_prompt_kind kind, const char *initial,
                           void (*submit)(struct leme_server *server,
                                          const char *name)) {
  leme_desk_prompt_close(server);
  if (server == NULL || server->config == NULL ||
      server->scene_overlay == NULL || server->seat == NULL || output == NULL ||
      output->wlr_output == NULL || !output->wlr_output->enabled ||
      leme_session_locked(server) ||
      (kind != LEME_DESK_PROMPT_CREATE && kind != LEME_DESK_PROMPT_RENAME &&
       kind != LEME_DESK_PROMPT_MESSAGE) ||
      (kind != LEME_DESK_PROMPT_MESSAGE && submit == NULL)) {
    return false;
  }
  const struct leme_box area = output->usable_box;
  const int height = kind == LEME_DESK_PROMPT_MESSAGE ? 116 : 180;
  if (area.width < 256 || area.height < height + 16) {
    return false;
  }
  leme_desk_buttons_cancel(server);
  if (server->desk_prompt == NULL) {
    server->desk_prompt = calloc(1, sizeof(*server->desk_prompt));
    if (server->desk_prompt == NULL) {
      return false;
    }
  }
  struct leme_desk_prompt *prompt = server->desk_prompt;
  prompt->server = server;
  prompt->kind = kind;
  prompt->scale = (double)output->wlr_output->scale;
  prompt->submit = submit;
  prompt->pressed = SIZE_MAX;
  prompt->focus = kind == LEME_DESK_PROMPT_MESSAGE ? 2 : 0;
  prompt->message[0] = '\0';
  const int width = area.width - 16 < 360 ? area.width - 16 : 360;
  prompt->box = leme_desk_menu_place(
      area, area.x + ((double)area.width - width) / 2.0,
      area.y + ((double)area.height - height) / 2.0, width, height);
  prompt->controls[0] = (struct leme_box){12, 68, width - 24, 28};
  prompt->controls[1] = (struct leme_box){width - 212, height - 40, 96, 28};
  prompt->controls[2] = (struct leme_box){width - 108, height - 40, 96, 28};
  if (kind == LEME_DESK_PROMPT_MESSAGE) {
    leme_desk_utf8_copy(prompt->message, sizeof(prompt->message), initial);
  } else if (!leme_desk_prompt_text_init(&prompt->text, initial)) {
    leme_desk_utf8_copy(prompt->message, sizeof(prompt->message),
                        "Enter a new name; original cannot be edited");
  }
  if (!leme_desk_prompt_draw(prompt)) {
    leme_desk_prompt_close(server);
    return false;
  }
  wlr_seat_pointer_notify_clear_focus(server->seat);
  leme_input_protocols_update_pointer_focus(server);
  leme_desktop_cursor_default(server);
  leme_desk_owner_claim(server, &leme_desk_prompt_owner, 0);
  return true;
}

static void leme_desk_prompt_motion(struct leme_server *server) {
  wlr_seat_pointer_notify_clear_focus(server->seat);
}

static void leme_desk_prompt_activate(struct leme_desk_prompt *prompt) {
  if (prompt->focus == 1 || prompt->kind == LEME_DESK_PROMPT_MESSAGE) {
    leme_desk_prompt_close(prompt->server);
  } else if (prompt->submit != NULL) {
    prompt->submit(prompt->server, prompt->text.bytes);
  }
}

static bool leme_desk_prompt_key(struct leme_server *server,
                                 xkb_keysym_t keysym, uint32_t modifiers,
                                 const char *utf8, size_t length) {
  struct leme_desk_prompt *prompt = server->desk_prompt;
  if ((keysym >= XKB_KEY_Shift_L && keysym <= XKB_KEY_Hyper_R) ||
      (keysym >= XKB_KEY_ISO_Lock && keysym <= XKB_KEY_ISO_Last_Group_Lock) ||
      (keysym >= XKB_KEY_ISO_Level5_Shift &&
       keysym <= XKB_KEY_ISO_Level5_Lock) ||
      keysym == XKB_KEY_Mode_switch || keysym == XKB_KEY_Num_Lock) {
    return true;
  }
  const uint32_t chords =
      modifiers & (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO);
  const bool select_all = prompt->focus == 0 && chords == WLR_MODIFIER_CTRL &&
                          (keysym == XKB_KEY_a || keysym == XKB_KEY_A);
  if (chords != 0 && !select_all) {
    leme_desk_prompt_close(server);
    return false;
  }
  if (keysym == XKB_KEY_Escape) {
    leme_desk_prompt_close(server);
  } else if (keysym == XKB_KEY_Return || keysym == XKB_KEY_KP_Enter ||
             (keysym == XKB_KEY_space && prompt->focus != 0)) {
    leme_desk_prompt_activate(prompt);
  } else if (keysym == XKB_KEY_Tab || keysym == XKB_KEY_ISO_Left_Tab) {
    if (prompt->kind != LEME_DESK_PROMPT_MESSAGE) {
      const bool reverse = keysym == XKB_KEY_ISO_Left_Tab ||
                           (modifiers & WLR_MODIFIER_SHIFT) != 0;
      prompt->focus = reverse ? (prompt->focus == 0 ? 2 : prompt->focus - 1)
                              : (prompt->focus + 1) % 3;
      leme_desk_prompt_update(prompt);
    }
  } else if (prompt->focus == 0 &&
             leme_desk_prompt_text_edit(&prompt->text, keysym, modifiers, utf8,
                                        length)) {
    leme_desk_prompt_update(prompt);
  }
  return true;
}

static size_t leme_desk_prompt_at(const struct leme_desk_prompt *prompt) {
  const struct wlr_cursor *cursor = prompt->server->cursor;
  if (cursor == NULL) {
    return SIZE_MAX;
  }
  for (size_t i = prompt->kind == LEME_DESK_PROMPT_MESSAGE ? 2 : 0; i < 3;
       i++) {
    if (leme_box_contains(prompt->controls[i], cursor->x - prompt->box.x,
                          cursor->y - prompt->box.y)) {
      return i;
    }
  }
  return SIZE_MAX;
}

static bool
leme_desk_prompt_button(struct leme_server *server,
                        const struct wlr_pointer_button_event *event) {
  struct leme_desk_prompt *prompt = server->desk_prompt;
  if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
    if (event->button == BTN_LEFT) {
      const bool activate = prompt->pressed != SIZE_MAX &&
                            prompt->pressed == leme_desk_prompt_at(prompt);
      prompt->pressed = SIZE_MAX;
      if (activate && prompt->focus != 0) {
        leme_desk_prompt_activate(prompt);
      }
    }
    return true;
  }
  const struct wlr_cursor *cursor = server->cursor;
  if (cursor == NULL || !leme_box_contains(prompt->box, cursor->x, cursor->y)) {
    leme_desk_prompt_close(server);
    return true;
  }
  if (event->button == BTN_LEFT) {
    prompt->pressed = leme_desk_prompt_at(prompt);
    if (prompt->pressed != SIZE_MAX) {
      prompt->focus = prompt->pressed;
      leme_desk_prompt_update(prompt);
    }
  }
  return true;
}

static const struct leme_desk_owner leme_desk_prompt_owner = {
    .modal = true,
    .motion = leme_desk_prompt_motion,
    .button = leme_desk_prompt_button,
    .key = leme_desk_prompt_key,
    .cancel = leme_desk_prompt_close,
};
