#include "config/parse-internal.h"

#include "config/config.h"
#include "config/internal.h"
#include "config/scfg.h"
#include "control/action.h"
#include "core/command.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wlr/types/wlr_keyboard.h>
#include <xkbcommon/xkbcommon-keysyms.h>

static bool leme_config_parse_trigger(const char *text,
                                      struct leme_binding *binding) {
  char *copy = strdup(text);
  char *key;
  char *prefix;
  char *save = NULL;
  char *token;
  char *separator;
  bool valid = true;

  if (copy == NULL) {
    return false;
  }
  binding->modifiers = 0;
  binding->trigger = LEME_BINDING_KEY;
  binding->fingers = 0;
  binding->direction = LEME_GESTURE_NONE;
  binding->edge = LEME_EDGE_NONE;
  binding->switch_kind = LEME_SWITCH_NONE;
  binding->switch_on = false;
  separator = strrchr(copy, '+');
  if (separator == NULL) {
    key = copy;
    prefix = NULL;
  } else {
    *separator = '\0';
    key = separator + 1;
    prefix = copy;
  }
  if (key[0] == '\0' || (prefix != NULL && (prefix[0] == '\0' ||
                                            prefix[strlen(prefix) - 1] == '+' ||
                                            strstr(prefix, "++") != NULL))) {
    valid = false;
  }
  for (token = prefix == NULL ? NULL : strtok_r(prefix, "+", &save);
       valid && token != NULL; token = strtok_r(NULL, "+", &save)) {
    uint32_t modifier;

    if (strcasecmp(token, "SUPER") == 0) {
      modifier = WLR_MODIFIER_LOGO;
    } else if (strcasecmp(token, "ALT") == 0) {
      modifier = WLR_MODIFIER_ALT;
    } else if (strcasecmp(token, "CTRL") == 0 ||
               strcasecmp(token, "CONTROL") == 0) {
      modifier = WLR_MODIFIER_CTRL;
    } else if (strcasecmp(token, "SHIFT") == 0) {
      modifier = WLR_MODIFIER_SHIFT;
    } else {
      valid = false;
      break;
    }
    if ((binding->modifiers & modifier) != 0) {
      valid = false;
      break;
    }
    binding->modifiers |= modifier;
  }
  binding->keysym = XKB_KEY_NoSymbol;
  char *gesture = NULL;
  if (strncasecmp(key, "swipe:", 6) == 0) {
    binding->trigger = LEME_BINDING_SWIPE;
    gesture = key + 6;
  } else if (strncasecmp(key, "pinch:", 6) == 0) {
    binding->trigger = LEME_BINDING_PINCH;
    gesture = key + 6;
  } else if (strncasecmp(key, "hold:", 5) == 0) {
    binding->trigger = LEME_BINDING_HOLD;
    gesture = key + 5;
  } else if (strncasecmp(key, "edge:", 5) == 0) {
    binding->trigger = LEME_BINDING_EDGE;
    gesture = key + 5;
  } else if (strncasecmp(key, "switch:", 7) == 0) {
    binding->trigger = LEME_BINDING_SWITCH;
    gesture = key + 7;
  }
  if (gesture == NULL) {
    binding->keysym =
        valid ? xkb_keysym_from_name(key, XKB_KEYSYM_CASE_INSENSITIVE)
              : XKB_KEY_NoSymbol;
    valid = valid && binding->keysym != XKB_KEY_NoSymbol;
    free(copy);
    return valid;
  }
  char *direction = strchr(gesture, ':');
  if (direction != NULL) {
    *direction = '\0';
    direction++;
  }
  if (binding->trigger == LEME_BINDING_SWITCH) {
    binding->switch_kind = strcasecmp(gesture, "lid") == 0 ? LEME_SWITCH_LID
                           : strcasecmp(gesture, "tablet") == 0
                               ? LEME_SWITCH_TABLET
                               : LEME_SWITCH_NONE;
    binding->switch_on = direction != NULL && strcasecmp(direction, "on") == 0;
    valid = valid && binding->modifiers == 0 &&
            binding->switch_kind != LEME_SWITCH_NONE && direction != NULL &&
            (binding->switch_on || strcasecmp(direction, "off") == 0);
    free(copy);
    return valid;
  }
  if (binding->trigger == LEME_BINDING_EDGE) {
    binding->fingers = 1;
    if (strcasecmp(gesture, "left") == 0) {
      binding->edge = LEME_EDGE_LEFT;
    } else if (strcasecmp(gesture, "right") == 0) {
      binding->edge = LEME_EDGE_RIGHT;
    } else if (strcasecmp(gesture, "top") == 0) {
      binding->edge = LEME_EDGE_TOP;
    } else if (strcasecmp(gesture, "bottom") == 0) {
      binding->edge = LEME_EDGE_BOTTOM;
    } else {
      valid = false;
    }
  } else {
    const uint32_t least = binding->trigger == LEME_BINDING_SWIPE   ? 3
                           : binding->trigger == LEME_BINDING_PINCH ? 2
                                                                    : 1;
    valid =
        valid && gesture[0] >= '1' && gesture[0] <= '5' && gesture[1] == '\0';
    if (valid) {
      binding->fingers = (uint32_t)(gesture[0] - '0');
      valid = binding->fingers >= least;
    }
  }
  if (direction != NULL) {
    if (strcasecmp(direction, "up") == 0) {
      binding->direction = LEME_GESTURE_UP;
    } else if (strcasecmp(direction, "down") == 0) {
      binding->direction = LEME_GESTURE_DOWN;
    } else if (strcasecmp(direction, "left") == 0) {
      binding->direction = LEME_GESTURE_LEFT;
    } else if (strcasecmp(direction, "right") == 0) {
      binding->direction = LEME_GESTURE_RIGHT;
    } else if (strcasecmp(direction, "in") == 0) {
      binding->direction = LEME_GESTURE_IN;
    } else if (strcasecmp(direction, "out") == 0) {
      binding->direction = LEME_GESTURE_OUT;
    } else {
      valid = false;
    }
  }
  switch (binding->trigger) {
  case LEME_BINDING_SWIPE:
    valid = valid && binding->direction != LEME_GESTURE_IN &&
            binding->direction != LEME_GESTURE_OUT;
    break;
  case LEME_BINDING_PINCH:
    valid = valid && (binding->direction == LEME_GESTURE_IN ||
                      binding->direction == LEME_GESTURE_OUT);
    break;
  case LEME_BINDING_HOLD:
    valid = valid && direction == NULL;
    break;
  case LEME_BINDING_EDGE:
    valid = valid &&
            (binding->edge == LEME_EDGE_LEFT || binding->edge == LEME_EDGE_RIGHT
                 ? binding->direction == LEME_GESTURE_UP ||
                       binding->direction == LEME_GESTURE_DOWN
                 : binding->direction == LEME_GESTURE_LEFT ||
                       binding->direction == LEME_GESTURE_RIGHT);
    break;
  case LEME_BINDING_KEY:
  case LEME_BINDING_SWITCH:
    break;
  }
  free(copy);
  return valid;
}

bool leme_config_bindings_overlap(const struct leme_binding *first,
                                  const struct leme_binding *second) {
  if (first->trigger != second->trigger ||
      first->modifiers != second->modifiers) {
    return false;
  }
  if (first->trigger == LEME_BINDING_KEY) {
    return first->keysym == second->keysym;
  }
  if (first->trigger == LEME_BINDING_SWITCH) {
    return first->switch_kind == second->switch_kind &&
           first->switch_on == second->switch_on;
  }
  return first->fingers == second->fingers && first->edge == second->edge &&
         (first->direction == second->direction ||
          first->direction == LEME_GESTURE_NONE ||
          second->direction == LEME_GESTURE_NONE);
}

static bool leme_config_parse_command(struct leme_binding *binding,
                                      const struct leme_scfg_directive *entry,
                                      char **error) {
  const size_t skip =
      entry->params_len > 0 && strcmp(entry->params[0], "repeat") == 0 ? 1 : 0;

  if (entry->params_len == skip) {
    leme_config_set_error(error, "%s", "binding requires a command");
    return false;
  }
  binding->repeat = skip == 1;
  if (!leme_command_parse(&binding->command, entry->params + skip,
                          entry->params_len - skip, error)) {
    return false;
  }
  if (!leme_config_check_command(&binding->command, error)) {
    leme_command_finish(&binding->command);
    return false;
  }
  return true;
}

bool leme_config_check_command(const struct leme_command *command,
                               char **error) {
  if (command->type != LEME_COMMAND_ACT) {
    return true;
  }
  struct leme_control_error failure = {0};
  const enum leme_control_code code = leme_control_check_act(
      (struct leme_public_text){command->text, strlen(command->text)},
      &failure);
  if (code == LEME_CONTROL_OK) {
    return true;
  }
  const char *message = failure.message[0] != '\0'
                            ? failure.message
                            : leme_control_code_to_string(code);
  if (failure.has_byte_offset) {
    leme_config_set_error(error, "act: %s at byte %zu", message,
                          failure.byte_offset);
  } else {
    leme_config_set_error(error, "act: %s", message);
  }
  return false;
}

static struct leme_bind_group *
leme_bind_scope_find(struct leme_bind_scope *scope, const char *name) {
  size_t index;

  for (index = 0; index < scope->count; index++) {
    if (strcmp(scope->groups[index].name, name) == 0) {
      return &scope->groups[index];
    }
  }
  return NULL;
}

static bool leme_bind_group_add_inherit(struct leme_bind_group *group,
                                        const char *name, const char *path,
                                        int lineno, char **error) {
  char **names =
      realloc(group->inherits, (group->inherit_count + 1) * sizeof(*names));

  if (names == NULL) {
    leme_config_set_error(error, "%s:%d: out of memory", path, lineno);
    return false;
  }
  group->inherits = names;
  group->inherits[group->inherit_count] = strdup(name);
  if (group->inherits[group->inherit_count] == NULL) {
    leme_config_set_error(error, "%s:%d: out of memory", path, lineno);
    return false;
  }
  group->inherit_count++;
  return true;
}

bool leme_config_parse_bind_block(struct leme_config *config,
                                  struct leme_bind_scope *scope,
                                  const struct leme_scfg_directive *directive,
                                  bool is_mode, const char *path,
                                  char **error) {
  struct leme_bind_group *groups;
  struct leme_bind_group *group;
  size_t index;
  bool seen_escape_exits = false;

  if (directive->params_len != 1) {
    leme_config_set_error(error, "%s:%d: %s requires one name", path,
                          directive->lineno, directive->name);
    return false;
  }
  if (leme_bind_scope_find(scope, directive->params[0]) != NULL) {
    leme_config_set_error(error, "%s:%d: duplicate binding block name %s", path,
                          directive->lineno, directive->params[0]);
    return false;
  }
  groups = realloc(scope->groups, (scope->count + 1) * sizeof(*groups));
  if (groups == NULL) {
    leme_config_set_error(error, "%s:%d: out of memory", path,
                          directive->lineno);
    return false;
  }
  scope->groups = groups;
  group = &scope->groups[scope->count];
  *group = (struct leme_bind_group){0};
  group->is_mode = is_mode;
  group->escape_exits = true;
  group->lineno = directive->lineno;
  group->name = strdup(directive->params[0]);
  if (group->name == NULL) {
    leme_config_set_error(error, "%s:%d: out of memory", path,
                          directive->lineno);
    return false;
  }
  scope->count++;

  const struct leme_scfg_directive **binding_directives = NULL;

  for (index = 0; index < directive->children.directives_len; index++) {
    const struct leme_scfg_directive *entry =
        &directive->children.directives[index];
    const struct leme_scfg_directive **dirs;
    struct leme_binding *bindings;
    struct leme_binding *binding;
    size_t previous;
    bool duplicate = false;

    if (entry->children.directives_len != 0) {
      if (!leme_config_reject(config, entry, -1,
                              "binding cannot contain a block")) {
        free(binding_directives);
        return false;
      }
      continue;
    }
    if (strcmp(entry->name, "inherit") == 0) {
      if (entry->params_len != 1) {
        if (!leme_config_reject(config, entry, 0,
                                "inherit requires one bind group name")) {
          free(binding_directives);
          return false;
        }
        continue;
      }
      if (!leme_bind_group_add_inherit(group, entry->params[0], path,
                                       entry->lineno, error)) {
        free(binding_directives);
        return false;
      }
      continue;
    }
    if (strcmp(entry->name, "escape_exits") == 0) {
      if (!is_mode) {
        if (!leme_config_reject(config, entry, -1,
                                "escape_exits belongs to a binds block, "
                                "not a bind_group")) {
          free(binding_directives);
          return false;
        }
        continue;
      }
      if (seen_escape_exits || entry->params_len != 1) {
        if (!leme_config_reject(
                config, entry, -1,
                "escape_exits takes one value and may appear once")) {
          free(binding_directives);
          return false;
        }
        continue;
      }
      if (strcmp(entry->params[0], "true") == 0) {
        group->escape_exits = true;
      } else if (strcmp(entry->params[0], "false") == 0) {
        group->escape_exits = false;
      } else {
        if (!leme_config_reject(config, entry, 0,
                                "escape_exits must be true or false")) {
          free(binding_directives);
          return false;
        }
        continue;
      }
      seen_escape_exits = true;
      continue;
    }
    bindings = realloc(group->bindings,
                       (group->binding_count + 1) * sizeof(*bindings));
    if (bindings == NULL) {
      free(binding_directives);
      leme_config_set_error(error, "%s:%d: out of memory", path, entry->lineno);
      return false;
    }
    group->bindings = bindings;
    dirs =
        realloc(binding_directives, (group->binding_count + 1) * sizeof(*dirs));
    if (dirs == NULL) {
      free(binding_directives);
      leme_config_set_error(error, "%s:%d: out of memory", path, entry->lineno);
      return false;
    }
    binding_directives = dirs;
    binding = &group->bindings[group->binding_count];
    *binding = (struct leme_binding){0};
    binding->lineno = entry->lineno;
    if (!leme_config_parse_trigger(entry->name, binding)) {
      const char *what = "key specification";
      struct leme_reject_extra extra = {0};
      if (binding->trigger == LEME_BINDING_SWITCH) {
        what = "switch";
        extra.help = binding->modifiers != 0
                         ? "switch bindings take no modifiers"
                         : "switches are switch:lid and switch:tablet, each "
                           "with on or off";
      } else if (binding->trigger != LEME_BINDING_KEY) {
        what = "gesture";
        extra.help = "gestures are swipe:3-5 with up, down, left or right, "
                     "pinch:2-5 with in or out, hold:1-5, edge:left or "
                     "edge:right with up or down, and edge:top or "
                     "edge:bottom with left or right";
      }
      if (!leme_config_reject_detailed(config, entry, -1, &extra,
                                       "invalid %s %s", what, entry->name)) {
        free(binding_directives);
        return false;
      }
      continue;
    }
    if (binding->trigger != LEME_BINDING_KEY && entry->params_len > 0 &&
        strcmp(entry->params[0], "repeat") == 0) {
      if (!leme_config_reject(config, entry, 0,
                              "repeat only works on key bindings")) {
        free(binding_directives);
        return false;
      }
      continue;
    }
    const uint32_t swipe_fingers = config->gestures.workspace_switch.fingers;
    if (binding->trigger == LEME_BINDING_SWIPE && swipe_fingers != 0 &&
        binding->fingers == swipe_fingers &&
        binding->direction != LEME_GESTURE_UP &&
        binding->direction != LEME_GESTURE_DOWN) {
      char help[64];
      (void)snprintf(help, sizeof(help),
                     "the tag swipe uses %" PRIu32
                     "-finger left and right swipes",
                     swipe_fingers);
      const struct leme_reject_extra extra = {.help = help};
      if (!leme_config_reject_detailed(config, entry, -1, &extra,
                                       "%s overlaps the tag swipe",
                                       entry->name)) {
        free(binding_directives);
        return false;
      }
      continue;
    }
    for (previous = 0; previous < group->binding_count; previous++) {
      if (leme_config_bindings_overlap(&group->bindings[previous], binding)) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      const struct leme_reject_extra extra = {
          .secondary = binding_directives[previous],
          .secondary_label = "first defined here",
      };
      if (!leme_config_reject_detailed(config, entry, -1, &extra,
                                       "duplicate binding %s", entry->name)) {
        free(binding_directives);
        return false;
      }
      continue;
    }
    if (!leme_config_parse_command(binding, entry, error)) {
      const bool kept = leme_config_reject(
          config, entry, 0, "invalid command for %s: %s", entry->name,
          *error != NULL ? *error : "invalid command");

      free(*error);
      *error = NULL;
      if (!kept) {
        free(binding_directives);
        return false;
      }
      continue;
    }
    const struct leme_command_spec *spec =
        leme_command_spec(binding->command.type);
    const bool gesture_command =
        (spec->flags & LEME_COMMAND_FLAG_GESTURE_ONLY) != 0;
    const bool free_swipe = binding->trigger == LEME_BINDING_SWIPE &&
                            binding->direction == LEME_GESTURE_NONE;
    if (gesture_command != free_swipe) {
      leme_command_finish(&binding->command);
      const bool kept =
          gesture_command
              ? leme_config_reject(
                    config, entry, binding->repeat ? 1 : 0,
                    "%s only works on a swipe binding without a direction",
                    spec->name)
              : leme_config_reject(config, entry, -1,
                                   "%s needs gesture_move or gesture_resize; "
                                   "other commands need a direction",
                                   entry->name);
      if (!kept) {
        free(binding_directives);
        return false;
      }
      continue;
    }
    binding_directives[group->binding_count] = entry;
    group->binding_count++;
  }
  free(binding_directives);
  return true;
}

static bool leme_bind_group_append(struct leme_bind_group *group,
                                   const struct leme_binding *binding,
                                   const char *origin, const char *path,
                                   char **error) {
  const struct leme_binding **resolved;
  const char **origins;
  size_t index;

  for (index = 0; index < group->resolved_count; index++) {
    if (!leme_config_bindings_overlap(group->resolved[index], binding)) {
      continue;
    }
    if (group->origin[index] == origin) {
      return true;
    }
    if (origin != group->name ||
        group->resolved[index]->direction != binding->direction) {
      leme_config_set_error(
          error, "%s:%d: groups %s and %s both bind the same key in %s", path,
          group->lineno, group->origin[index], origin, group->name);
      return false;
    }
    group->resolved[index] = binding;
    group->origin[index] = origin;
    return true;
  }
  resolved =
      realloc(group->resolved, (group->resolved_count + 1) * sizeof(*resolved));
  if (resolved == NULL) {
    leme_config_set_error(error, "%s:%d: out of memory", path, group->lineno);
    return false;
  }
  group->resolved = resolved;
  origins =
      realloc(group->origin, (group->resolved_count + 1) * sizeof(*origins));
  if (origins == NULL) {
    leme_config_set_error(error, "%s:%d: out of memory", path, group->lineno);
    return false;
  }
  group->origin = origins;
  group->resolved[group->resolved_count] = binding;
  group->origin[group->resolved_count] = origin;
  group->resolved_count++;
  return true;
}

static bool leme_bind_group_resolve(struct leme_bind_scope *scope,
                                    struct leme_bind_group *group,
                                    const char *path, char **error) {
  size_t index;
  size_t entry;

  if (group->state == LEME_BIND_RESOLVED) {
    return true;
  }
  if (group->state == LEME_BIND_RESOLVING) {
    leme_config_set_error(error, "%s:%d: bind group %s inherits itself", path,
                          group->lineno, group->name);
    return false;
  }
  group->state = LEME_BIND_RESOLVING;
  for (index = 0; index < group->inherit_count; index++) {
    struct leme_bind_group *parent =
        leme_bind_scope_find(scope, group->inherits[index]);

    if (parent == NULL) {
      leme_config_set_error(error, "%s:%d: %s inherits unknown bind group %s",
                            path, group->lineno, group->name,
                            group->inherits[index]);
      return false;
    }
    if (parent->is_mode) {
      leme_config_set_error(error,
                            "%s:%d: %s is a binding mode, not a bind group",
                            path, group->lineno, parent->name);
      return false;
    }
    if (parent->state == LEME_BIND_RESOLVING) {
      leme_config_set_error(
          error,
          parent == group
              ? "%s:%d: bind group %s inherits itself"
              : "%s:%d: bind groups %s and %s form an inheritance cycle",
          path, group->lineno, group->name, parent->name);
      return false;
    }
    if (!leme_bind_group_resolve(scope, parent, path, error)) {
      return false;
    }
    for (entry = 0; entry < parent->resolved_count; entry++) {
      if (!leme_bind_group_append(group, parent->resolved[entry],
                                  parent->origin[entry], path, error)) {
        return false;
      }
    }
  }
  for (index = 0; index < group->binding_count; index++) {
    if (!leme_bind_group_append(group, &group->bindings[index], group->name,
                                path, error)) {
      return false;
    }
  }
  group->state = LEME_BIND_RESOLVED;
  return true;
}

static bool leme_config_materialize_mode(struct leme_config *config,
                                         const struct leme_bind_group *group,
                                         const char *path, char **error) {
  struct leme_mode *modes;
  struct leme_mode *mode;
  size_t index;
  bool has_exit = false;

  modes = realloc(config->modes, (config->mode_count + 1) * sizeof(*modes));
  if (modes == NULL) {
    leme_config_set_error(error, "%s:%d: out of memory", path, group->lineno);
    return false;
  }
  config->modes = modes;
  mode = &config->modes[config->mode_count];
  *mode = (struct leme_mode){0};
  mode->escape_exits = group->escape_exits;
  mode->name = strdup(group->name);
  if (mode->name == NULL) {
    leme_config_set_error(error, "%s:%d: out of memory", path, group->lineno);
    return false;
  }
  config->mode_count++;
  if (group->resolved_count == 0) {
    return true;
  }
  mode->bindings = calloc(group->resolved_count, sizeof(*mode->bindings));
  if (mode->bindings == NULL) {
    leme_config_set_error(error, "%s:%d: out of memory", path, group->lineno);
    return false;
  }
  for (index = 0; index < group->resolved_count; index++) {
    const struct leme_binding *source = group->resolved[index];

    mode->bindings[index].modifiers = source->modifiers;
    mode->bindings[index].keysym = source->keysym;
    mode->bindings[index].trigger = source->trigger;
    mode->bindings[index].fingers = source->fingers;
    mode->bindings[index].direction = source->direction;
    mode->bindings[index].edge = source->edge;
    mode->bindings[index].switch_kind = source->switch_kind;
    mode->bindings[index].switch_on = source->switch_on;
    mode->bindings[index].lineno = source->lineno;
    mode->bindings[index].repeat = source->repeat;
    if (!leme_config_copy_command(&mode->bindings[index].command,
                                  &source->command)) {
      leme_config_set_error(error, "%s:%d: out of memory", path, group->lineno);
      return false;
    }
    mode->binding_count++;
    if (source->command.type == LEME_COMMAND_SET_MODE) {
      has_exit = true;
    }
  }
  if (!mode->escape_exits && !has_exit) {
    leme_config_set_error(
        error, "%s:%d: mode %s keeps Escape but binds no mode command", path,
        group->lineno, mode->name);
    return false;
  }
  return true;
}

bool leme_config_resolve_binds(struct leme_config *config,
                               struct leme_bind_scope *scope, const char *path,
                               char **error) {
  size_t index;

  for (index = 0; index < scope->count; index++) {
    if (!scope->groups[index].is_mode) {
      continue;
    }
    if (!leme_bind_group_resolve(scope, &scope->groups[index], path, error)) {
      return false;
    }
    if (!leme_config_materialize_mode(config, &scope->groups[index], path,
                                      error)) {
      return false;
    }
  }
  return true;
}

void leme_bind_scope_finish(struct leme_bind_scope *scope) {
  size_t index;

  for (index = 0; index < scope->count; index++) {
    struct leme_bind_group *group = &scope->groups[index];
    size_t entry;

    for (entry = 0; entry < group->binding_count; entry++) {
      leme_config_finish_command(&group->bindings[entry].command);
    }
    for (entry = 0; entry < group->inherit_count; entry++) {
      free(group->inherits[entry]);
    }
    free(group->bindings);
    free(group->inherits);
    free(group->resolved);
    free(group->origin);
    free(group->name);
  }
  free(scope->groups);
  *scope = (struct leme_bind_scope){0};
}

bool leme_config_drop_invalid_binds(struct leme_config *config) {
  size_t mode_index;

  for (mode_index = 0; mode_index < config->mode_count; mode_index++) {
    struct leme_mode *mode = &config->modes[mode_index];
    size_t index = 0;

    while (index < mode->binding_count) {
      struct leme_binding *binding = &mode->bindings[index];

      if (leme_config_validate_command(config, &binding->command)) {
        index++;
        continue;
      }
      if (!leme_diagnostics_add(
              &config->diagnostics, binding->lineno,
              "binding in mode %s refers to something undefined", mode->name)) {
        return false;
      }
      leme_config_finish_command(&binding->command);
      memmove(binding, binding + 1,
              (mode->binding_count - index - 1) * sizeof(*binding));
      mode->binding_count--;
    }
  }
  return true;
}
