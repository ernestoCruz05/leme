#include "output/home.h"

#include "core/server.h"
#include "output/output.h"
#include "render/render.h"
#include "shell/ownership.h"
#include "shell/container.h"
#include "shell/policy.h"
#include "shell/view.h"
#include "workspace/tag.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <wlr/types/wlr_output.h>

struct leme_output_home_tag {
  uint16_t id;
  enum leme_layout_kind kind;
};

struct leme_output_home {
  struct wl_list link;
  char *name;
  uint16_t focused_id;
  struct leme_output_home_tag *tags;
  size_t tag_count;
};

static void leme_output_home_destroy(struct leme_output_home *home) {
  wl_list_remove(&home->link);
  free(home->tags);
  free(home->name);
  free(home);
}

static struct leme_output_home *
leme_output_home_find(struct leme_server *server, const char *name) {
  struct leme_output_home *home;

  if (server == NULL || name == NULL || server->output_homes.next == NULL) {
    return NULL;
  }
  wl_list_for_each(home, &server->output_homes, link) {
    if (strcmp(home->name, name) == 0) {
      return home;
    }
  }
  return NULL;
}

bool leme_output_home_tracked(const struct leme_output *output) {
  const struct leme_server *server = output->server;

  return output->wlr_output != NULL && output->wlr_output->name != NULL &&
         (server == NULL || server->headless_backend == NULL ||
          output->wlr_output->backend != server->headless_backend);
}

static void leme_output_home_remember(struct leme_output *output) {
  struct leme_server *server = output->server;
  const struct leme_tags *tags = &output->tags;
  struct leme_output_home_tag *entries;
  struct leme_output_home *home;
  size_t count = 0;
  size_t index = 0;

  if (server == NULL || server->output_homes.next == NULL ||
      tags->table == NULL) {
    return;
  }
  for (uint16_t id = 1; id <= tags->max_tags; id++) {
    if (tags->table[id] != NULL) {
      count++;
    }
  }
  entries = calloc(count == 0 ? 1 : count, sizeof(*entries));
  if (entries == NULL) {
    return;
  }
  for (uint16_t id = 1; id <= tags->max_tags; id++) {
    if (tags->table[id] != NULL) {
      entries[index++] = (struct leme_output_home_tag){
          .id = id,
          .kind = tags->table[id]->layout.kind,
      };
    }
  }
  home = leme_output_home_find(server, output->wlr_output->name);
  if (home == NULL) {
    char *name = strdup(output->wlr_output->name);

    home = calloc(1, sizeof(*home));
    if (home == NULL || name == NULL) {
      free(home);
      free(name);
      free(entries);
      return;
    }
    home->name = name;
    wl_list_insert(&server->output_homes, &home->link);
  } else {
    free(home->tags);
  }
  home->tags = entries;
  home->tag_count = count;
  home->focused_id = tags->focused_id;
}

static void leme_output_home_reanchor(struct leme_view *view,
                                      struct leme_box from, struct leme_box to,
                                      enum leme_layout_kind previous) {
  if (leme_view_geometry_owner(view) != LEME_VIEW_GEOMETRY_FLOATING ||
      view->fullscreen) {
    return;
  }
  const struct leme_tag *tag = leme_ownership_tag(view);
  if (!view->floating && !view->detached && tag != NULL &&
      tag->layout.kind == LEME_LAYOUT_DESKTOP &&
      previous != LEME_LAYOUT_DESKTOP) {
    return;
  }
  view->box = leme_view_policy_reanchor_box(view->box, from, to);
  leme_render_view_set_box(view, view->box);
}

void leme_output_home_init(struct leme_server *server) {
  wl_list_init(&server->output_homes);
}

void leme_output_home_finish(struct leme_server *server) {
  struct leme_output_home *home;
  struct leme_output_home *temporary;

  if (server->output_homes.next == NULL) {
    return;
  }
  wl_list_for_each_safe(home, temporary, &server->output_homes, link) {
    leme_output_home_destroy(home);
  }
}

void leme_output_home_evacuate(struct leme_output *from,
                               struct leme_output *to) {
  struct leme_tags *source = leme_output_tags(from);
  struct leme_tags *destination = leme_output_tags(to);
  bool tracked;

  if (source == NULL || destination == NULL || source == destination) {
    return;
  }
  tracked = leme_output_home_tracked(from);
  if (tracked) {
    leme_output_home_remember(from);
  }
  for (uint16_t id = 1; id <= source->max_tags; id++) {
    while (source->table[id] != NULL &&
           !wl_list_empty(&source->table[id]->views)) {
      if (!wl_list_empty(&source->table[id]->containers)) {
        struct leme_container *container = wl_container_of(
            source->table[id]->containers.next, container, link);
        struct leme_view *member = NULL;
        wl_list_for_each(member, &source->table[id]->views, tag_link) {
          if (tracked && member->home_output == NULL &&
              leme_container_for_view(member) == container) {
            member->home_output = strdup(from->wlr_output->name);
            member->home_tag = member->home_output == NULL ? 0 : id;
          }
        }
        if (leme_container_adopt_output(container, to, id)) {
          continue;
        }
        (void)leme_container_dissolve(container);
      }
      struct leme_view *view =
          wl_container_of(source->table[id]->views.next, view, tag_link);
      char *home = view->home_output;
      uint16_t home_tag = view->home_tag;
      bool adopted;

      view->home_output = NULL;
      if (home == NULL && tracked) {
        home = strdup(from->wlr_output->name);
        home_tag = id;
      }
      const enum leme_layout_kind previous =
          leme_ownership_tag(view)->layout.kind;
      const bool placed = leme_view_is_placed(view);
      const struct leme_box saved_restore = view->placement.restore_box;
      if (placed) {
        view->placement.restore_box = leme_view_policy_reanchor_box(
            saved_restore, from->usable_box, to->usable_box);
      }
      adopted = leme_tags_adopt_view(destination, view, id);
      if (!adopted && placed) {
        view->placement.restore_box = saved_restore;
      }
      view->home_output = home;
      view->home_tag = home == NULL ? 0 : home_tag;
      if (!adopted) {
        break;
      }
      if (!placed) {
        leme_output_home_reanchor(view, from->usable_box, to->usable_box,
                                  previous);
      }
    }
  }
}

bool leme_output_home_restore(struct leme_output *output, const char *name) {
  struct leme_tags *tags = leme_output_tags(output);
  struct leme_output_home *home;
  struct leme_server *server;
  struct leme_view *view;
  bool restored = false;

  if (tags == NULL || name == NULL) {
    return false;
  }
  server = output->server;
  wl_list_for_each(view, &server->views, link) {
    char *view_home = view->home_output;
    struct leme_output *source;

    if (view_home == NULL || strcmp(view_home, name) != 0 ||
        leme_ownership_tag(view) == NULL) {
      continue;
    }
    if (view->container != NULL) {
      (void)leme_container_dissolve(view->container);
    }
    source = leme_view_output(view);
    const enum leme_layout_kind previous =
        leme_ownership_tag(view)->layout.kind;
    const bool placed = leme_view_is_placed(view);
    const struct leme_box saved_restore = view->placement.restore_box;
    if (placed && source != NULL && source != output) {
      view->placement.restore_box = leme_view_policy_reanchor_box(
          saved_restore, source->usable_box, output->usable_box);
    }
    view->home_output = NULL;
    if (source != output && leme_tags_adopt_view(tags, view, view->home_tag)) {
      if (source != NULL && !placed) {
        leme_output_home_reanchor(view, source->usable_box, output->usable_box,
                                  previous);
      }
      restored = true;
    } else if (placed) {
      view->placement.restore_box = saved_restore;
    }
    view->home_tag = 0;
    free(view_home);
  }
  home = leme_output_home_find(server, name);
  if (home == NULL) {
    return restored;
  }
  for (size_t index = 0; index < home->tag_count; index++) {
    const uint16_t id = home->tags[index].id;

    if (id <= tags->max_tags && tags->table[id] != NULL) {
      (void)leme_tag_set_layout(tags->table[id], home->tags[index].kind);
    }
  }
  (void)leme_tags_restore_focus(tags, home->focused_id);
  leme_output_home_destroy(home);
  return true;
}

void leme_output_home_forget_view(struct leme_view *view) {
  if (view == NULL) {
    return;
  }
  free(view->home_output);
  view->home_output = NULL;
  view->home_tag = 0;
}
