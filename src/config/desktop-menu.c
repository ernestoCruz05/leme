#include "config/internal.h"

#include "config/diagnostics.h"
#include "config/scfg.h"
#include "core/command.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct leme_config_desktop_menu_row {
  const char *label;
  enum leme_command_type type;
};

static const char *const leme_config_desktop_menu_names[] = {
    [LEME_DESKTOP_MENU_BLANK] = "blank",
    [LEME_DESKTOP_MENU_FILES] = "files",
};

static const struct leme_config_desktop_menu_row
    leme_config_desktop_menu_blank[] = {
        {"New folder", LEME_COMMAND_GRID_NEW_FOLDER},
        {0},
        {"Open desktop folder", LEME_COMMAND_GRID_OPEN},
};

static const struct leme_config_desktop_menu_row
    leme_config_desktop_menu_files[] = {
        {"Open", LEME_COMMAND_GRID_OPEN},
        {0},
        {"Rename", LEME_COMMAND_GRID_RENAME},
        {"Move to Trash", LEME_COMMAND_GRID_TRASH},
};

static void
leme_config_desktop_menu_clear(struct leme_desktop_menu_list *list) {
  for (size_t i = 0; i < list->count; i++) {
    free(list->entries[i].label);
    leme_config_finish_command(&list->entries[i].command);
  }
  free(list->entries);
  *list = (struct leme_desktop_menu_list){0};
}

void leme_config_desktop_menu_finish(struct leme_config *config) {
  for (size_t i = 0; i < LEME_DESKTOP_MENU_COUNT; i++) {
    leme_config_desktop_menu_clear(&config->desktop_menus[i]);
  }
}

static bool
leme_config_desktop_menu_fill(struct leme_desktop_menu_list *list,
                              const struct leme_config_desktop_menu_row *rows,
                              size_t count) {
  list->entries = calloc(count, sizeof(*list->entries));
  if (list->entries == NULL) {
    return false;
  }
  for (size_t i = 0; i < count; i++) {
    struct leme_desktop_menu_entry *entry = &list->entries[list->count++];
    if (rows[i].label == NULL) {
      continue;
    }
    entry->label = strdup(rows[i].label);
    entry->command.type = rows[i].type;
    if (entry->label == NULL) {
      return false;
    }
  }
  return true;
}

bool leme_config_desktop_menu_defaults(struct leme_config *config) {
  leme_config_desktop_menu_finish(config);
  if (leme_config_desktop_menu_fill(
          &config->desktop_menus[LEME_DESKTOP_MENU_BLANK],
          leme_config_desktop_menu_blank,
          LEME_ARRAY_LENGTH(leme_config_desktop_menu_blank)) &&
      leme_config_desktop_menu_fill(
          &config->desktop_menus[LEME_DESKTOP_MENU_FILES],
          leme_config_desktop_menu_files,
          LEME_ARRAY_LENGTH(leme_config_desktop_menu_files))) {
    return true;
  }
  leme_config_desktop_menu_finish(config);
  return false;
}

static bool
leme_config_desktop_menu_reject_name(struct leme_config *config,
                                     const struct leme_scfg_directive *block) {
  char help[128] = {0};
  struct leme_reject_extra extra = {0};
  const char *nearest = leme_config_nearest_key(
      block->params[0], leme_config_desktop_menu_names,
      LEME_ARRAY_LENGTH(leme_config_desktop_menu_names));
  if (nearest != NULL) {
    (void)snprintf(help, sizeof(help), "a name like this exists: `%s`",
                   nearest);
    extra.help = help;
  }
  return leme_config_reject_detailed(config, block, 0, &extra,
                                     "unknown desktop_menu %s; use blank or "
                                     "files",
                                     block->params[0]);
}

static bool leme_config_desktop_menu_placeholders(
    struct leme_config *config, const struct leme_scfg_directive *row,
    enum leme_desktop_menu menu, const struct leme_command *command,
    bool *fatal) {
  if (command->type != LEME_COMMAND_SPAWN) {
    return true;
  }
  for (size_t i = 0; command->argv[i] != NULL; i++) {
    const char *word = command->argv[i];
    if (strstr(word, "%files") == NULL) {
      continue;
    }
    const char *problem =
        menu != LEME_DESKTOP_MENU_FILES
            ? "%files is only available in desktop_menu \"files\""
            : (strcmp(word, "%files") != 0 ? "%files must be a whole argument"
                                           : NULL);
    if (problem != NULL) {
      *fatal = !leme_config_reject(config, row, (int)i + 2, "%s", problem);
      return false;
    }
  }
  return true;
}

static bool leme_config_desktop_menu_row(struct leme_config *config,
                                         const struct leme_scfg_directive *row,
                                         enum leme_desktop_menu menu,
                                         struct leme_desktop_menu_list *list,
                                         bool *fatal) {
  const bool separator = strcmp(row->name, "separator") == 0;
  if (!separator && strcmp(row->name, "entry") != 0) {
    *fatal = !leme_config_reject(config, row, -1,
                                 "unknown desktop_menu row %s; use entry or "
                                 "separator",
                                 row->name);
    return false;
  }
  if (row->has_block || row->children.directives_len != 0) {
    *fatal =
        !leme_config_reject(config, row, -1, "%s takes no block", row->name);
    return false;
  }
  if (list->count == LEME_DESKTOP_MENU_ENTRIES_MAX) {
    *fatal = !leme_config_reject(config, row, -1,
                                 "desktop_menu holds at most %d rows",
                                 LEME_DESKTOP_MENU_ENTRIES_MAX);
    return false;
  }
  struct leme_desktop_menu_entry *entry = &list->entries[list->count];
  *entry = (struct leme_desktop_menu_entry){.lineno = row->lineno};
  if (separator) {
    if (row->params_len != 0) {
      *fatal = !leme_config_reject(config, row, 0, "separator takes no values");
      return false;
    }
    list->count++;
    return true;
  }
  if (row->params_len < 2) {
    *fatal = !leme_config_reject(config, row, -1,
                                 "entry needs a label and a command");
    return false;
  }
  const size_t length = strlen(row->params[0]);
  if (length == 0 || length > 63) {
    *fatal = !leme_config_reject(config, row, 0,
                                 "entry label must be 1 to 63 bytes");
    return false;
  }
  char *error = NULL;
  if (!leme_command_parse(&entry->command, row->params + 1, row->params_len - 1,
                          &error)) {
    *fatal =
        !leme_config_reject(config, row, 1, "invalid command for entry: %s",
                            error != NULL ? error : "invalid command");
    free(error);
    return false;
  }
  if (!leme_config_check_command(&entry->command, &error)) {
    *fatal =
        !leme_config_reject(config, row, 1, "invalid command for entry: %s",
                            error != NULL ? error : "invalid command");
    free(error);
    leme_config_finish_command(&entry->command);
    return false;
  }
  if (!leme_config_desktop_menu_placeholders(config, row, menu, &entry->command,
                                             fatal)) {
    leme_config_finish_command(&entry->command);
    return false;
  }
  entry->label = strdup(row->params[0]);
  if (entry->label == NULL) {
    leme_config_finish_command(&entry->command);
    *fatal = true;
    return false;
  }
  list->count++;
  return true;
}

bool leme_config_parse_desktop_menu(struct leme_config *config,
                                    const struct leme_scfg_directive *directive,
                                    const struct leme_scfg_directive **seen) {
  if (directive->params_len != 1 || !directive->has_block) {
    return leme_config_reject(config, directive, -1,
                              "desktop_menu needs one name, blank or files, "
                              "and a block");
  }
  size_t menu = 0;
  while (menu < LEME_DESKTOP_MENU_COUNT &&
         strcmp(directive->params[0], leme_config_desktop_menu_names[menu]) !=
             0) {
    menu++;
  }
  if (menu == LEME_DESKTOP_MENU_COUNT) {
    return leme_config_desktop_menu_reject_name(config, directive);
  }
  if (seen[menu] != NULL) {
    const struct leme_reject_extra extra = {
        .secondary = seen[menu],
        .secondary_label = "first defined here",
    };
    return leme_config_reject_detailed(config, directive, 0, &extra,
                                       "duplicate desktop_menu %s",
                                       leme_config_desktop_menu_names[menu]);
  }
  seen[menu] = directive;
  const size_t rows = directive->children.directives_len;
  const size_t capacity = rows < LEME_DESKTOP_MENU_ENTRIES_MAX
                              ? rows
                              : LEME_DESKTOP_MENU_ENTRIES_MAX;
  struct leme_desktop_menu_list list = {
      .entries = calloc(capacity == 0 ? 1 : capacity, sizeof(*list.entries)),
  };
  if (list.entries == NULL) {
    return false;
  }
  for (size_t i = 0; i < rows; i++) {
    bool fatal = false;
    (void)leme_config_desktop_menu_row(
        config, &directive->children.directives[i],
        (enum leme_desktop_menu)menu, &list, &fatal);
    if (fatal) {
      leme_config_desktop_menu_clear(&list);
      return false;
    }
  }
  leme_config_desktop_menu_clear(&config->desktop_menus[menu]);
  config->desktop_menus[menu] = list;
  return true;
}

bool leme_config_drop_invalid_desktop_menu(struct leme_config *config) {
  for (size_t menu = 0; menu < LEME_DESKTOP_MENU_COUNT; menu++) {
    struct leme_desktop_menu_list *list = &config->desktop_menus[menu];
    size_t index = 0;
    while (index < list->count) {
      struct leme_desktop_menu_entry *entry = &list->entries[index];
      if (entry->label == NULL ||
          leme_config_validate_command(config, &entry->command)) {
        index++;
        continue;
      }
      if (!leme_diagnostics_add(&config->diagnostics, entry->lineno,
                                "desktop_menu entry %s refers to something "
                                "undefined",
                                entry->label)) {
        return false;
      }
      free(entry->label);
      leme_config_finish_command(&entry->command);
      memmove(entry, entry + 1, (list->count - index - 1) * sizeof(*entry));
      list->count--;
    }
  }
  return true;
}
