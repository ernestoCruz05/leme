#include "render/render.h"
#include "desktop/decoration.h"
#include "desktop/frame.h"
#include "desktop/buttons.h"
#include "public/server.h"

#include "config/config.h"
#include "config/style.h"
#include "core/server.h"
#include "input/input.h"
#include "output/output.h"
#include "protocols/capture.h"
#include "protocols/decoration.h"
#include "protocols/session.h"
#include "shell/rules.h"
#include "shell/view.h"
#include "shell/container.h"
#include "workspace/tag.h"
#include "render/workspace_transition.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>
#include <wlr/util/edges.h>
#include <wlr/util/transform.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/xwayland/xwayland.h>

static void leme_render_view_identify(struct leme_view *view) {
  view->render_owner = (struct leme_render_owner){
      .kind = LEME_RENDER_OWNER_VIEW, .value.view = view};
  if (view->render_tree != NULL) {
    view->render_tree->node.data = &view->render_owner;
  }
}

static struct wlr_scene_tree *
leme_render_view_parent(const struct leme_view *view) {
  if (view == NULL || view->server == NULL) {
    return NULL;
  }
  if (leme_ownership_kind(view) == LEME_VIEW_OWNER_DURABLE) {
    return view->server->scene_durable;
  }
  if (view->unmanaged) {
    return view->server->scene_unmanaged;
  }
  if (view->server->config == NULL ||
      view->server->config->fullscreen_covers != LEME_FULLSCREEN_COVERS_NONE) {
    const struct leme_tag *tag = leme_ownership_tag(view);
    const struct leme_view *other;

    if (view->fullscreen) {
      return view->server->scene_fullscreen;
    }
    if (tag != NULL) {
      wl_list_for_each(other, &tag->views, tag_link) {
        if (other->mapped && other->fullscreen &&
            leme_view_is_transient_of(view, other)) {
          return view->server->scene_fullscreen;
        }
      }
    }
  }
  return leme_view_geometry_owner(view) != LEME_VIEW_GEOMETRY_TAG_LAYOUT ||
                 view->detached || view->fullscreen
             ? view->server->scene_floating
             : view->server->scene_tiled;
}

/*
 * Um scratchpad ou grupo pegajoso invoca-se por cima do que estiver a dar,
 * ecrã inteiro incluído. A árvore `durable` só sobe enquanto houver conteúdo
 * promovido: sem isso a ordem com as barras ficava mudada no caso comum.
 */
static void leme_render_sync_durable(struct leme_server *server) {
  if (server == NULL || server->scene_durable == NULL ||
      server->scene_fullscreen == NULL || server->scene_top == NULL ||
      server->scene_unmanaged == NULL) {
    return;
  }
  if (wl_list_empty(&server->scene_fullscreen->children)) {
    wlr_scene_node_place_below(&server->scene_durable->node,
                               &server->scene_top->node);
    wlr_scene_node_place_above(&server->scene_unmanaged->node,
                               &server->scene_top->node);
  } else {
    wlr_scene_node_place_above(&server->scene_durable->node,
                               &server->scene_fullscreen->node);
    wlr_scene_node_place_above(&server->scene_unmanaged->node,
                               &server->scene_durable->node);
  }
}

struct leme_render_surface_tracker {
  struct leme_render_view_state *state;
  struct wlr_surface *surface;
  struct wlr_subsurface *subsurface;
  struct wl_list children;
  struct wl_list link;
  struct wl_listener commit;
  struct wl_listener map;
  struct wl_listener destroy;
  struct wl_listener new_subsurface;
  struct wl_listener subsurface_destroy;
  struct wlr_scene_buffer *buffer;
  struct wl_listener buffer_destroy;
};

struct leme_render_view_state {
  struct leme_view *view;
  struct wl_list roots;
  struct wlr_box clip;
  struct wlr_box surface_clip;
  bool clip_applied;
};

struct leme_render_view_content_base {
  struct wlr_scene_node *node;
  int x;
  int y;
  int width;
  int height;
  float opacity;
  float color[4];
  bool enabled;
  int root_x;
  int root_y;
  struct leme_animation_buffer_geometry geometry;
};

struct leme_render_view_copy {
  struct wlr_scene_tree *root;
  struct leme_render_view_frame_nodes nodes;
  struct wl_array content_bases;
  int content_width;
  int content_height;
};

struct leme_render_view_animation {
  struct leme_view *view;
  struct leme_output *output;
  struct wlr_scene_tree *root;
  struct leme_render_view_frame_nodes nodes;
  struct wl_array content_bases;
  struct wl_array decoration_bases;
  struct wl_array shell_bases;
  int shell_x[LEME_RENDER_VIEW_SHELL_MAX];
  int shell_y[LEME_RENDER_VIEW_SHELL_MAX];
  float border_color[4][4];
  int border_width;
  int final_content_width;
  int final_content_height;
  bool restore_on_done;
  bool moving;
  bool flying;
  struct leme_box current;
  struct leme_render_view_copy redrawn;
};

static void
leme_render_view_animation_begin(struct leme_view *view,
                                 const struct leme_animation_settings *settings,
                                 bool opening);
static bool
leme_render_surface_tracker_create(struct leme_render_view_state *state,
                                   struct leme_render_surface_tracker *parent,
                                   struct wlr_surface *surface,
                                   struct wlr_subsurface *owner);
static void
leme_render_surface_tracker_finish(struct leme_render_surface_tracker *tracker);
static bool leme_render_view_tracking_init(struct leme_view *view,
                                           struct wlr_surface *surface);
static bool leme_render_view_track_root(struct leme_view *view,
                                        struct wlr_surface *surface);
static void leme_render_view_untrack_root(struct leme_view *view,
                                          struct wlr_surface *surface);
static void leme_render_view_tracking_finish(struct leme_view *view);
static void leme_render_view_apply_surface_effects(struct leme_view *view,
                                                   int corner_radius,
                                                   int border_width, int blur,
                                                   struct leme_box content,
                                                   struct leme_gaps insets);

void leme_render_view_create(struct leme_view *view) {
  static const float fallback[4] = {0.16f, 0.42f, 0.72f, 1.0f};

  if (view == NULL || view->server == NULL) {
    return;
  }
  const float *border_color = view->server->config != NULL
                                  ? view->server->config->border_inactive
                                  : fallback;
  struct wlr_surface *surface = NULL;
  size_t index;

  view->render_tree = wlr_scene_tree_create(leme_render_view_parent(view));
  if (view->render_tree == NULL) {
    return;
  }
  leme_render_view_identify(view);
  for (index = 0; index < LEME_ARRAY_LENGTH(view->border); index++) {
    view->border[index] =
        wlr_scene_rect_create(view->render_tree, 1, 1, border_color);
    if (view->border[index] == NULL) {
      leme_render_view_destroy(view);
      return;
    }
  }
  if (view->kind == LEME_VIEW_XDG) {
    surface = view->xdg_toplevel->base->surface;
    view->scene_tree = wlr_scene_xdg_surface_create(view->render_tree,
                                                    view->xdg_toplevel->base);
    if (view->scene_tree != NULL) {
      view->xdg_toplevel->base->data = view->scene_tree;
    }
  } else {
    surface =
        view->xwayland_surface == NULL ? NULL : view->xwayland_surface->surface;
    if (!leme_render_xwayland_create(view)) {
      view->scene_tree = NULL;
    }
  }
  /* A cena regista primeiro os listeners de commit existentes. */
  if (view->scene_tree == NULL ||
      !leme_render_view_tracking_init(view, surface)) {
    leme_render_view_destroy(view);
    return;
  }
  leme_session_refresh_idle_inhibitors(view->server);
}

#define LEME_ANIMATION_OPEN_TIMEOUT_MS 200

static void leme_render_view_cancel_open_timeout(struct leme_view *view) {
  view->open_animation_pending = false;
  if (view->open_animation_timeout != NULL) {
    wl_event_source_remove(view->open_animation_timeout);
    view->open_animation_timeout = NULL;
  }
}

static bool leme_render_view_animation_allowed(const struct leme_view *view) {
  /* Um destino mascarado continua vivo para frames e commits, mas não
   * pode acrescentar um instantâneo de janela por cima da composição da
   * área de trabalho. */
  return !view->unmanaged && !view->fullscreen &&
         !leme_view_is_minimized(view) &&
         !leme_render_workspace_transition_hides_view(view) &&
         (view->server->session_protocols == NULL ||
          !view->server->session_protocols->locked);
}

static void leme_render_view_start_open(struct leme_view *view) {
  leme_render_view_cancel_open_timeout(view);
  if (view->render_tree == NULL) {
    return;
  }
  /* O estado pode ter mudado enquanto esperávamos pela confirmação. */
  leme_render_view_sync_presentation(view);
  if (!leme_render_view_animation_allowed(view)) {
    wlr_scene_node_set_enabled(&view->render_tree->node,
                               !leme_view_is_minimized(view));
    return;
  }
  wlr_scene_node_set_enabled(&view->render_tree->node, false);
  leme_render_view_animation_begin(
      view, &view->server->config->animation[LEME_ANIMATION_OPEN], true);
}

/* Um cliente que nunca mais confirma não pode ficar invisível. */
static int leme_render_view_handle_open_timeout(void *data) {
  struct leme_view *view = data;

  leme_render_view_cancel_open_timeout(view);
  if (view->render_tree != NULL) {
    leme_render_view_sync_presentation(view);
    wlr_scene_node_set_enabled(&view->render_tree->node,
                               !leme_view_is_minimized(view));
  }
  return 0;
}

/*
 * Só depois de o cliente confirmar o configure é que a vista mostra o
 * tamanho que lhe demos; copiá-la antes disso capta o buffer inicial.
 */
static bool leme_render_view_open_settled(const struct leme_view *view) {
  const struct wlr_xdg_surface *base;

  if (leme_decoration_protocol_holds_open(view)) {
    return false;
  }
  if (view->kind != LEME_VIEW_XDG || view->xdg_toplevel == NULL) {
    return true;
  }
  base = view->xdg_toplevel->base;
  return base->current.configure_serial >= view->open_animation_serial;
}

void leme_render_view_open_ready(struct leme_view *view) {
  if (view == NULL || !view->open_animation_pending ||
      !leme_render_view_open_settled(view)) {
    return;
  }
  leme_render_view_start_open(view);
}

void leme_render_view_animate(struct leme_view *view,
                              enum leme_animation_event event) {
  const struct leme_animation_settings *settings;

  if (view == NULL || view->server == NULL || view->server->config == NULL ||
      view->render_tree == NULL) {
    return;
  }
  /* Largar a animação anterior antes de a vista poder desaparecer. */
  if (view->animation_state != NULL) {
    leme_animation_manager_finish_data(&view->server->animations,
                                       view->animation_state);
  }
  leme_render_view_cancel_open_timeout(view);
  if ((event == LEME_ANIMATION_OPEN && leme_container_for_view(view) != NULL) ||
      !leme_render_view_animation_allowed(view) ||
      !view->render_tree->node.enabled) {
    return;
  }
  settings = &view->server->config->animation[event];
  if (!settings->configured) {
    return;
  }
  if (event != LEME_ANIMATION_OPEN) {
    leme_render_view_animation_begin(view, settings, false);
    return;
  }
  /*
   * A vista ainda mostra o primeiro buffer do cliente, não o tamanho que
   * acabámos de configurar. Esperar pela confirmação antes de a copiar.
   */
  view->open_animation_pending = true;
  view->open_animation_serial =
      view->kind == LEME_VIEW_XDG && view->xdg_toplevel != NULL
          ? view->xdg_toplevel->base->scheduled_serial
          : 0;
  leme_render_view_sync_presentation(view);
  view->open_animation_timeout =
      wl_event_loop_add_timer(wl_display_get_event_loop(view->server->display),
                              leme_render_view_handle_open_timeout, view);
  if (view->open_animation_timeout == NULL) {
    leme_render_view_start_open(view);
    return;
  }
  wl_event_source_timer_update(view->open_animation_timeout,
                               LEME_ANIMATION_OPEN_TIMEOUT_MS);
}

void leme_render_view_finish_animation(struct leme_view *view) {
  if (view == NULL) {
    return;
  }
  leme_render_view_cancel_open_timeout(view);
  if (view->server != NULL && view->animation_state != NULL) {
    leme_animation_manager_finish_data(&view->server->animations,
                                       view->animation_state);
  }
  leme_render_view_sync_presentation(view);
}

void leme_render_view_destroy(struct leme_view *view) {
  struct leme_render_view_animation *state = view->animation_state;

  /* A animação de fecho sobrevive à vista: fica sem quem restaurar. */
  if (state != NULL) {
    state->view = NULL;
    view->animation_state = NULL;
  }
  leme_render_view_cancel_open_timeout(view);
  leme_capture_invalidate_view(view);
  leme_desk_decoration_view_finish(view);
  leme_render_view_tracking_finish(view);
  if (view->kind == LEME_VIEW_XDG && view->xdg_toplevel != NULL) {
    view->xdg_toplevel->base->data = NULL;
    if (view->render_tree != NULL) {
      wlr_scene_node_destroy(&view->render_tree->node);
    }
  } else if (view->kind == LEME_VIEW_XWAYLAND) {
    leme_render_xwayland_destroy(view);
  }
  view->render_tree = NULL;
  view->scene_tree = NULL;
  view->drop_preview = NULL;
  for (size_t index = 0; index < LEME_ARRAY_LENGTH(view->border); index++) {
    view->border[index] = NULL;
  }
  /* Se era a última vista promovida, a árvore `durable` volta ao lugar. */
  leme_render_sync_durable(view->server);
  leme_session_refresh_idle_inhibitors(view->server);
}

bool leme_render_view_popup_create(struct leme_view_popup *popup) {
  struct wlr_xdg_surface *parent =
      wlr_xdg_surface_try_from_wlr_surface(popup->wlr_popup->parent);
  struct wlr_scene_tree *parent_tree = parent == NULL ? NULL : parent->data;

  if (parent_tree == NULL) {
    return false;
  }
  popup->scene_tree =
      wlr_scene_xdg_surface_create(parent_tree, popup->wlr_popup->base);
  /* Repõe a opacidade do popup depois dos listeners da cena. */
  if (popup->scene_tree == NULL ||
      !leme_render_view_track_root(popup->view,
                                   popup->wlr_popup->base->surface)) {
    if (popup->scene_tree != NULL) {
      wlr_scene_node_destroy(&popup->scene_tree->node);
      popup->scene_tree = NULL;
    }
    return false;
  }
  popup->wlr_popup->base->data = popup->scene_tree;
  leme_capture_popup_create(popup);
  leme_session_refresh_idle_inhibitors(popup->view->server);
  return true;
}

static int leme_render_saturate_coordinate(int64_t value) {
  if (value > INT_MAX) {
    return INT_MAX;
  }
  if (value < INT_MIN) {
    return INT_MIN;
  }
  return (int)value;
}

static struct leme_box
leme_render_view_popup_constraint_box(struct leme_box full, int view_x,
                                      int view_y,
                                      struct leme_box root_geometry) {
  full.x = leme_render_saturate_coordinate((int64_t)full.x - view_x +
                                           root_geometry.x);
  full.y = leme_render_saturate_coordinate((int64_t)full.y - view_y +
                                           root_geometry.y);
  return full;
}

void leme_render_view_popup_unconstrain(struct leme_view_popup *popup,
                                        struct leme_box full) {
  const struct wlr_box root = popup->view->xdg_toplevel->base->geometry;
  struct leme_box converted;
  struct wlr_box constraint;
  int view_x;
  int view_y;

  if (popup->view->scene_tree == NULL ||
      !wlr_scene_node_coords(&popup->view->scene_tree->node, &view_x,
                             &view_y)) {
    return;
  }
  converted = leme_render_view_popup_constraint_box(full, view_x, view_y,
                                                    (struct leme_box){
                                                        .x = root.x,
                                                        .y = root.y,
                                                        .width = root.width,
                                                        .height = root.height,
                                                    });
  constraint = (struct wlr_box){
      .x = converted.x,
      .y = converted.y,
      .width = converted.width,
      .height = converted.height,
  };
  wlr_xdg_popup_unconstrain_from_box(popup->wlr_popup, &constraint);
}

void leme_render_view_popup_destroy(struct leme_view_popup *popup) {
  leme_capture_popup_destroy(popup);
  leme_render_view_untrack_root(popup->view, popup->wlr_popup->base->surface);
  popup->wlr_popup->base->data = NULL;
  if (popup->scene_tree != NULL) {
    wlr_scene_node_destroy(&popup->scene_tree->node);
    popup->scene_tree = NULL;
  }
  leme_session_refresh_idle_inhibitors(popup->view->server);
}

void leme_render_set_view_visible(struct leme_view *view, bool visible) {
  if (!visible || leme_view_is_minimized(view)) {
    leme_desk_buttons_invalidate_view(view);
  }
  if (view->render_tree != NULL) {
    wlr_scene_node_set_enabled(&view->render_tree->node,
                               visible && !leme_view_is_minimized(view));
  }
  leme_render_view_sync_presentation(view);
  leme_session_refresh_idle_inhibitors(view->server);
}

static void leme_render_view_apply_opacity(struct wlr_scene_buffer *buffer,
                                           int sx, int sy, void *data) {
  const float *opacity = data;

  (void)sx;
  (void)sy;
  wlr_scene_buffer_set_opacity(buffer, *opacity);
}

/*
 * Enquanto a apresentação está mascarada, a vista fica transparente em vez
 * de desactivada. Assim continua a receber frames e commits sem aparecer por
 * cima do instantâneo que representa a transição.
 */
bool leme_render_view_presentation_hidden(const struct leme_view *view) {
  const struct leme_render_view_animation *state = view->animation_state;

  return view->open_animation_pending || (state != NULL && state->moving) ||
         leme_render_workspace_transition_hides_view(view);
}

static float leme_render_view_effective_opacity(const struct leme_view *view) {
  return leme_render_view_presentation_hidden(view) ? 0.0f
                                                    : view->render_opacity;
}

static void leme_render_view_apply_cached_opacity(struct leme_view *view) {
  float opacity;

  if (view == NULL || view->scene_tree == NULL) {
    return;
  }
  opacity = leme_render_view_effective_opacity(view);
  wlr_scene_node_for_each_buffer(&view->scene_tree->node,
                                 leme_render_view_apply_opacity, &opacity);
}

/* A espessura nunca é escalada; só cede quando a caixa não a comporta. */
static int leme_render_view_clamp_border(int border_width,
                                         struct leme_box frame) {
  int maximum = frame.width < frame.height ? frame.width : frame.height;

  maximum = maximum > 0 ? (maximum - 1) / 2 : 0;
  return border_width < maximum ? border_width : maximum;
}

static int leme_render_view_border_width(const struct leme_view *view,
                                         struct leme_box frame) {
  if (view == NULL || view->fullscreen || view->unmanaged ||
      view->server == NULL || view->server->config == NULL ||
      leme_decoration_protocol_replaces_border(view, false)) {
    return 0;
  }
  return leme_render_view_clamp_border(view->server->config->border_width,
                                       frame);
}

uint32_t leme_render_view_resize_edges(const struct leme_view *view, double lx,
                                       double ly) {
  if (!leme_desk_decoration_eligible(view) ||
      !leme_ownership_hit_test_eligible(view) || leme_view_is_maximized(view) ||
      view->render_tree == NULL || leme_render_view_presentation_hidden(view)) {
    return WLR_EDGE_NONE;
  }
  const struct leme_output *output = leme_view_output(view);
  const struct leme_tag *tag = leme_ownership_tag(view);
  int x = 0;
  int y = 0;
  if (output == NULL || output->wlr_output == NULL ||
      !output->wlr_output->enabled || output->tags.focused_is_candidate ||
      output->tags.focused_id != tag->id ||
      !wlr_scene_node_coords(&view->render_tree->node, &x, &y)) {
    return WLR_EDGE_NONE;
  }
  struct leme_box frame = view->box;
  frame.x = x;
  frame.y = y;
  return leme_desk_decoration_resize_edges(
      frame, leme_render_view_border_width(view, frame), lx, ly);
}

/* A visibilidade da moldura respeita a espessura, regras e efeitos. */
static bool leme_render_view_border_shown(const struct leme_view *view,
                                          size_t index) {
  int border_width;

  if (view == NULL || index >= LEME_ARRAY_LENGTH(view->border) ||
      view->border[index] == NULL) {
    return false;
  }
  if (view->border[1] == NULL) {
    return index == 0;
  }
  if (view->fullscreen || view->unmanaged || view->server == NULL ||
      view->server->config == NULL) {
    return false;
  }
  if (view->box.width > 0 && view->box.height > 0) {
    border_width = leme_render_view_border_width(view, view->box);
  } else if (leme_decoration_protocol_replaces_border(view, false)) {
    border_width = 0;
  } else {
    border_width = view->server->config->border_width;
  }
  if (border_width <= 0) {
    return false;
  }
#ifdef LEME_HAVE_EFFECTS
  return index == 0;
#else
  return true;
#endif
}

/*
 * Esconder com set_enabled(false) corta os frame callbacks ao cliente, e um
 * cliente que só desenha neles nunca confirma o configure: a espera pela
 * confirmação impedia-a. A vista fica visível para o wlroots e transparente
 * para quem olha. As molduras são rectângulos e não precisam de frames.
 */
void leme_render_view_sync_presentation(struct leme_view *view) {
  const bool hidden =
      view != NULL && leme_render_view_presentation_hidden(view);
  size_t index;

  leme_render_view_apply_cached_opacity(view);
  if (view == NULL) {
    return;
  }
  struct wlr_scene_tree *decoration = leme_desk_decoration_tree(view);
  if (decoration != NULL) {
    wlr_scene_node_set_enabled(
        &decoration->node,
        !hidden && leme_desk_decoration_eligible(view) &&
            leme_desk_decoration_height(view, view->box) > 0);
  }
  for (index = 0; index < LEME_ARRAY_LENGTH(view->border); index++) {
    if (view->border[index] != NULL) {
      bool shown = !hidden && leme_render_view_border_shown(view, index);

      wlr_scene_node_set_enabled(&view->border[index]->node, shown);
    }
  }
  leme_decoration_protocol_view_present(view);
}

static void leme_render_tracker_find_buffer(struct wlr_scene_buffer *buffer,
                                            int sx, int sy, void *data) {
  struct leme_render_surface_tracker *tracker = data;
  struct wlr_scene_surface *scene_surface;

  (void)sx;
  (void)sy;
  if (tracker->buffer != NULL) {
    return;
  }
  scene_surface = wlr_scene_surface_try_from_buffer(buffer);
  if (scene_surface != NULL && scene_surface->surface == tracker->surface) {
    tracker->buffer = buffer;
  }
}

static void
leme_render_tracker_handle_buffer_destroy(struct wl_listener *listener,
                                          void *data) {
  struct leme_render_surface_tracker *tracker =
      wl_container_of(listener, tracker, buffer_destroy);

  (void)data;
  wl_list_remove(&tracker->buffer_destroy.link);
  wl_list_init(&tracker->buffer_destroy.link);
  tracker->buffer = NULL;
}

static void
leme_render_tracker_apply_opacity(struct leme_render_surface_tracker *tracker) {
  struct leme_view *view = tracker->state->view;

  if (tracker->buffer == NULL && view->scene_tree != NULL) {
    wlr_scene_node_for_each_buffer(&view->scene_tree->node,
                                   leme_render_tracker_find_buffer, tracker);
    if (tracker->buffer != NULL) {
      tracker->buffer_destroy.notify =
          leme_render_tracker_handle_buffer_destroy;
      wl_signal_add(&tracker->buffer->node.events.destroy,
                    &tracker->buffer_destroy);
    }
  }
  if (tracker->buffer != NULL) {
    wlr_scene_buffer_set_opacity(tracker->buffer,
                                 leme_render_view_effective_opacity(view));
  }
}

static void leme_render_surface_tracker_finish(
    struct leme_render_surface_tracker *tracker) {
  struct leme_render_surface_tracker *child;
  struct leme_render_surface_tracker *tmp;

  wl_list_for_each_safe(child, tmp, &tracker->children, link) {
    leme_render_surface_tracker_finish(child);
  }
  wl_list_remove(&tracker->commit.link);
  wl_list_remove(&tracker->map.link);
  wl_list_remove(&tracker->destroy.link);
  wl_list_remove(&tracker->new_subsurface.link);
  wl_list_remove(&tracker->buffer_destroy.link);
  if (tracker->subsurface != NULL) {
    wl_list_remove(&tracker->subsurface_destroy.link);
  }
  wl_list_remove(&tracker->link);
  free(tracker);
}

static void
leme_render_surface_tracker_handle_commit(struct wl_listener *listener,
                                          void *data) {
  struct leme_render_surface_tracker *tracker =
      wl_container_of(listener, tracker, commit);

  (void)data;
  leme_render_tracker_apply_opacity(tracker);
  if (tracker->subsurface == NULL) {
    leme_render_view_open_ready(tracker->state->view);
  }
}

static void leme_render_surface_tracker_handle_map(struct wl_listener *listener,
                                                   void *data) {
  struct leme_render_surface_tracker *tracker =
      wl_container_of(listener, tracker, map);

  (void)data;
  leme_render_tracker_apply_opacity(tracker);
}

static void
leme_render_surface_tracker_handle_destroy(struct wl_listener *listener,
                                           void *data) {
  struct leme_render_surface_tracker *tracker =
      wl_container_of(listener, tracker, destroy);

  (void)data;
  leme_render_surface_tracker_finish(tracker);
}

static void leme_render_surface_tracker_handle_subsurface_destroy(
    struct wl_listener *listener, void *data) {
  struct leme_render_surface_tracker *tracker =
      wl_container_of(listener, tracker, subsurface_destroy);

  (void)data;
  leme_render_surface_tracker_finish(tracker);
}

static void
leme_render_surface_tracker_handle_new_subsurface(struct wl_listener *listener,
                                                  void *data) {
  struct leme_render_surface_tracker *tracker =
      wl_container_of(listener, tracker, new_subsurface);
  struct wlr_subsurface *subsurface = data;

  /* O listener anterior da cena já criou a superfície filha. */
  if (!leme_render_surface_tracker_create(tracker->state, tracker,
                                          subsurface->surface, subsurface)) {
    wl_resource_post_no_memory(subsurface->resource);
  }
}

static bool
leme_render_surface_tracker_create(struct leme_render_view_state *state,
                                   struct leme_render_surface_tracker *parent,
                                   struct wlr_surface *surface,
                                   struct wlr_subsurface *owner) {
  struct leme_render_surface_tracker *tracker = calloc(1, sizeof(*tracker));
  struct wlr_subsurface *subsurface;

  if (tracker == NULL) {
    return false;
  }
  tracker->state = state;
  tracker->surface = surface;
  tracker->subsurface = owner;
  wl_list_init(&tracker->children);
  wl_list_init(&tracker->buffer_destroy.link);
  wl_list_insert(parent == NULL ? &state->roots : &parent->children,
                 &tracker->link);
  tracker->commit.notify = leme_render_surface_tracker_handle_commit;
  wl_signal_add(&surface->events.commit, &tracker->commit);
  tracker->map.notify = leme_render_surface_tracker_handle_map;
  wl_signal_add(&surface->events.map, &tracker->map);
  tracker->destroy.notify = leme_render_surface_tracker_handle_destroy;
  wl_signal_add(&surface->events.destroy, &tracker->destroy);
  tracker->new_subsurface.notify =
      leme_render_surface_tracker_handle_new_subsurface;
  wl_signal_add(&surface->events.new_subsurface, &tracker->new_subsurface);
  if (owner != NULL) {
    tracker->subsurface_destroy.notify =
        leme_render_surface_tracker_handle_subsurface_destroy;
    wl_signal_add(&owner->events.destroy, &tracker->subsurface_destroy);
  }

  wl_list_for_each(subsurface, &surface->current.subsurfaces_below,
                   current.link) {
    if (!leme_render_surface_tracker_create(state, tracker, subsurface->surface,
                                            subsurface)) {
      leme_render_surface_tracker_finish(tracker);
      return false;
    }
  }
  wl_list_for_each(subsurface, &surface->current.subsurfaces_above,
                   current.link) {
    if (!leme_render_surface_tracker_create(state, tracker, subsurface->surface,
                                            subsurface)) {
      leme_render_surface_tracker_finish(tracker);
      return false;
    }
  }
  leme_render_tracker_apply_opacity(tracker);
  return true;
}

static bool leme_render_view_tracking_init(struct leme_view *view,
                                           struct wlr_surface *surface) {
  struct leme_render_view_state *state;

  if (view == NULL || surface == NULL || view->render_state != NULL) {
    return false;
  }
  state = calloc(1, sizeof(*state));
  if (state == NULL) {
    return false;
  }
  state->view = view;
  wl_list_init(&state->roots);
  view->render_state = state;
  if (!leme_render_surface_tracker_create(state, NULL, surface, NULL)) {
    leme_render_view_tracking_finish(view);
    return false;
  }
  return true;
}

static bool leme_render_view_track_root(struct leme_view *view,
                                        struct wlr_surface *surface) {
  return view != NULL && view->render_state != NULL && surface != NULL &&
         leme_render_surface_tracker_create(view->render_state, NULL, surface,
                                            NULL);
}

static void leme_render_view_untrack_root(struct leme_view *view,
                                          struct wlr_surface *surface) {
  struct leme_render_surface_tracker *tracker;
  struct leme_render_surface_tracker *tmp;

  if (view == NULL || view->render_state == NULL || surface == NULL) {
    return;
  }
  wl_list_for_each_safe(tracker, tmp, &view->render_state->roots, link) {
    if (tracker->surface == surface) {
      leme_render_surface_tracker_finish(tracker);
      return;
    }
  }
}

static void leme_render_view_tracking_finish(struct leme_view *view) {
  struct leme_render_surface_tracker *tracker;
  struct leme_render_surface_tracker *tmp;

  if (view == NULL || view->render_state == NULL) {
    return;
  }
  wl_list_for_each_safe(tracker, tmp, &view->render_state->roots, link) {
    leme_render_surface_tracker_finish(tracker);
  }
  free(view->render_state);
  view->render_state = NULL;
}

static float leme_render_view_opacity(const struct leme_config *config,
                                      bool activated, bool fullscreen,
                                      const struct leme_view_rules *rules);

void leme_render_view_set_activated(struct leme_view *view, bool activated) {
  const struct leme_config *config;
  const float *color;
  struct leme_view_rules rules;
  size_t index;

  if (view == NULL || view->render_tree == NULL || view->server == NULL ||
      view->server->config == NULL) {
    return;
  }
  config = view->server->config;
  rules = leme_view_rules_match(config, leme_view_identity(view),
                                leme_view_title(view));
  view->render_opacity =
      leme_render_view_opacity(config, activated, view->fullscreen, &rules);
  color = activated ? config->border_active : config->border_inactive;
  for (index = 0; index < LEME_ARRAY_LENGTH(view->border); index++) {
    if (view->border[index] != NULL) {
      wlr_scene_rect_set_color(view->border[index], color);
    }
  }
  leme_render_view_sync_presentation(view);
}

static float leme_render_view_opacity(const struct leme_config *config,
                                      bool activated, bool fullscreen,
                                      const struct leme_view_rules *rules) {
  if (config == NULL || fullscreen) {
    return 1.0f;
  }
  if (rules != NULL && (rules->fields & LEME_WINDOW_RULE_OPACITY) != 0) {
    return (float)rules->opacity;
  }
  return (float)(activated ? config->opacity_active : config->opacity_inactive);
}

struct leme_render_view_stack_entry {
  struct leme_view *view;
  struct leme_render_view_stack_entry *parent;
  struct leme_render_view_stack_entry *preferred;
  struct leme_render_view_stack_entry *previous;
  size_t cursor;
};

static bool leme_render_view_transient_of(const struct leme_view *child,
                                          const struct leme_view *parent) {
  if (child == parent || child->kind != parent->kind) {
    return false;
  }
  if (child->kind == LEME_VIEW_XDG) {
    return child->xdg_toplevel != NULL && parent->xdg_toplevel != NULL &&
           child->xdg_toplevel->parent == parent->xdg_toplevel;
  }
  return child->xwayland_surface != NULL && parent->xwayland_surface != NULL &&
         child->xwayland_surface->parent == parent->xwayland_surface;
}

static bool leme_render_view_stack_eligible(const struct leme_view *candidate,
                                            const struct leme_view *view) {
  return candidate->mapped && !candidate->unmanaged &&
         candidate->render_tree != NULL &&
         leme_ownership_tag(candidate) == leme_ownership_tag(view) &&
         candidate->render_tree->node.parent == view->render_tree->node.parent;
}

static void leme_render_view_stack_node(struct wlr_scene_node *node,
                                        struct wlr_scene_node **above) {
  if (*above == NULL) {
    wlr_scene_node_raise_to_top(node);
  } else {
    wlr_scene_node_place_below(node, *above);
  }
  *above = node;
}

static void leme_render_view_stack_place(struct leme_view *view,
                                         struct wlr_scene_node **above) {
  const struct leme_render_view_animation *animation = view->animation_state;
  struct wlr_scene_tree *layer = view->render_tree->node.parent;
  if (animation != NULL) {
    if (animation->redrawn.root != NULL &&
        animation->redrawn.root->node.parent == layer) {
      leme_render_view_stack_node(&animation->redrawn.root->node, above);
    }
    if (animation->root != NULL && animation->root->node.parent == layer) {
      leme_render_view_stack_node(&animation->root->node, above);
    }
  }
  leme_render_view_stack_node(&view->render_tree->node, above);
}

struct wlr_scene_node *
leme_render_view_stack_above(struct leme_view *view,
                             struct wlr_scene_node *anchor) {
  const struct leme_render_view_animation *animation = view->animation_state;
  wlr_scene_node_place_above(&view->render_tree->node, anchor);
  anchor = &view->render_tree->node;
  if (animation != NULL) {
    struct wlr_scene_tree *copies[] = {animation->root,
                                       animation->redrawn.root};
    for (size_t i = 0; i < LEME_ARRAY_LENGTH(copies); i++) {
      if (copies[i] != NULL && copies[i]->node.parent == anchor->parent) {
        wlr_scene_node_place_above(&copies[i]->node, anchor);
        anchor = &copies[i]->node;
      }
    }
  }
  return anchor;
}

bool leme_render_view_owns_node(const struct leme_view *view,
                                const struct wlr_scene_node *node) {
  const struct leme_render_view_animation *animation = view->animation_state;
  return node == &view->render_tree->node ||
         (animation != NULL &&
          ((animation->root != NULL && node == &animation->root->node) ||
           (animation->redrawn.root != NULL &&
            node == &animation->redrawn.root->node)));
}

static void leme_render_view_stack(struct leme_view *view, bool raise) {
  if (view == NULL || view->render_tree == NULL) {
    return;
  }
  struct leme_container *container = leme_container_for_view(view);
  if (container != NULL) {
    leme_render_container_restack(container, raise);
    return;
  }
  struct wlr_scene_node *above = NULL;
  if (view->server == NULL || view->server->views.next == NULL ||
      !view->mapped || view->unmanaged || leme_ownership_tag(view) == NULL) {
    if (raise) {
      leme_render_view_stack_place(view, &above);
    }
    return;
  }
  size_t capacity = 0;
  bool related = false;
  struct leme_view *candidate;
  wl_list_for_each(candidate, &view->server->views, link) {
    if (leme_render_view_stack_eligible(candidate, view)) {
      capacity++;
      related = related || leme_render_view_transient_of(candidate, view) ||
                leme_render_view_transient_of(view, candidate);
    }
  }
  if (!related) {
    if (raise) {
      leme_render_view_stack_place(view, &above);
    }
    return;
  }
  if (capacity > SIZE_MAX / sizeof(struct leme_render_view_stack_entry)) {
    return;
  }
  struct leme_render_view_stack_entry *entries =
      calloc(capacity, sizeof(*entries));
  if (entries == NULL) {
    wlr_log(WLR_ERROR, "%s",
            "leme: could not allocate transient stacking order");
    return;
  }
  struct leme_render_view_stack_entry *root = NULL;
  struct leme_render_view_stack_entry *focused = NULL;
  struct wlr_scene_node *node;
  size_t count = 0;
  wl_list_for_each(node, &view->render_tree->node.parent->children, link) {
    wl_list_for_each(candidate, &view->server->views, link) {
      if (leme_render_view_stack_eligible(candidate, view) &&
          node == &candidate->render_tree->node && count < capacity) {
        entries[count].view = candidate;
        if (candidate == view) {
          root = &entries[count];
        }
        if (candidate == view->server->focused_view) {
          focused = &entries[count];
        }
        count++;
        break;
      }
    }
  }
  if (root == NULL) {
    free(entries);
    return;
  }
  for (size_t i = 0; i < count; i++) {
    for (size_t j = 0; j < count; j++) {
      if (leme_render_view_transient_of(entries[i].view, entries[j].view)) {
        entries[i].parent = &entries[j];
        break;
      }
    }
  }
  for (size_t depth = 0; root->parent != NULL && depth < count; depth++) {
    if (raise) {
      root->parent->preferred = root;
    }
    root = root->parent;
  }
  if (root->parent != NULL) {
    free(entries);
    return;
  }
  if (!raise && focused != NULL) {
    struct leme_render_view_stack_entry *ancestor = focused;
    for (size_t depth = 0; ancestor->parent != NULL && depth < count; depth++) {
      ancestor = ancestor->parent;
    }
    if (ancestor == root) {
      while (focused->parent != NULL) {
        focused->parent->preferred = focused;
        focused = focused->parent;
      }
    }
  }
  struct leme_render_view_stack_entry *current = root;
  struct leme_render_view_stack_entry *last = root;
  while (current != NULL) {
    struct leme_render_view_stack_entry *child = NULL;
    while (current->cursor < count) {
      struct leme_render_view_stack_entry *next = &entries[current->cursor++];
      if (next->parent == current && next != current->preferred) {
        child = next;
        break;
      }
    }
    if (child == NULL && current->preferred != NULL) {
      child = current->preferred;
      current->preferred = NULL;
    }
    if (child == NULL) {
      current = current->parent;
    } else {
      child->previous = last;
      last = child;
      current = child;
    }
  }
  if (!raise) {
    wl_list_for_each_reverse(node, &view->render_tree->node.parent->children,
                             link) {
      bool member = false;
      for (current = last; current != NULL; current = current->previous) {
        if (leme_render_view_owns_node(current->view, node)) {
          member = true;
          break;
        }
      }
      if (member) {
        break;
      }
      above = node;
    }
  }
  for (current = last; current != NULL; current = current->previous) {
    leme_render_view_stack_place(current->view, &above);
  }
  free(entries);
}

void leme_render_view_focus(struct leme_view *view) {
  leme_render_view_stack(view, true);
}

void leme_render_view_restack(struct leme_view *view) {
  leme_render_view_stack(view, false);
}

void leme_render_view_update_layer(struct leme_view *view) {
  struct wlr_scene_tree *parent = leme_render_view_parent(view);

  if (view->render_tree != NULL && view->render_tree->node.parent != parent) {
    wlr_scene_node_reparent(&view->render_tree->node, parent);
    leme_render_view_focus(view);
  }
  leme_render_sync_durable(view->server);
}

/*
 * A árvore do ecrã inteiro nasce entre `top` e `overlay`; só a cobertura
 * `overlay` a sobe. `drag` e `lock` ficam sempre acima.
 */
void leme_render_apply_fullscreen_coverage(struct leme_server *server) {
  struct leme_view *view;

  if (server == NULL || server->scene_fullscreen == NULL ||
      server->scene_overlay == NULL) {
    return;
  }
  if (server->config != NULL &&
      server->config->fullscreen_covers == LEME_FULLSCREEN_COVERS_OVERLAY) {
    wlr_scene_node_place_above(&server->scene_fullscreen->node,
                               &server->scene_overlay->node);
  } else {
    wlr_scene_node_place_below(&server->scene_fullscreen->node,
                               &server->scene_overlay->node);
  }
  if (server->views.next == NULL) {
    return;
  }
  wl_list_for_each(view, &server->views, link) {
    if (view->fullscreen) {
      leme_render_view_update_layer(view);
    }
  }
  leme_render_sync_durable(server);
}

/*
 * O raio nunca passa de metade do lado mais curto: acima disso a caixa com
 * sinal inverte-se e o canto deixa de fechar.
 */
static const struct leme_titlebar_style *
leme_render_view_titlebar(const struct leme_view *view,
                          struct leme_titlebar_style *style) {
  if (view == NULL || view->server == NULL || view->server->config == NULL) {
    return NULL;
  }
  leme_desk_style_titlebar(view->server->config, style);
  return style;
}

static const struct leme_titlebar_style *leme_render_view_animation_titlebar(
    const struct leme_render_view_animation *state,
    struct leme_titlebar_style *style) {
  const struct leme_server *server = state->view != NULL ? state->view->server
                                     : state->output != NULL
                                         ? state->output->server
                                         : NULL;
  if (server == NULL || server->config == NULL) {
    return NULL;
  }
  leme_desk_style_titlebar(server->config, style);
  return style;
}

int leme_render_view_corner_radius(const struct leme_view *view,
                                   struct leme_box frame) {
  int limit;

  /* A animação de fecho sobrevive à vista e continua a pedir o raio. */
  if (view == NULL || view->fullscreen || view->unmanaged ||
      view->server == NULL || view->server->config == NULL ||
      view->container != NULL || leme_view_is_maximized(view)) {
    return 0;
  }
  limit = frame.width < frame.height ? frame.width : frame.height;
  limit /= 2;
  return view->server->config->corner_radius < limit
             ? view->server->config->corner_radius
             : limit;
}

/*
 * Só decora janelas geridas, tal como o raio. Se o desfoque chega a ver-se
 * decide-se na cena, a partir da região opaca do buffer: uma janela a toda a
 * opacidade cujo cliente desenhe a sua própria transparência ainda o quer, e
 * daqui não se sabe isso.
 */
static int leme_render_view_blur(const struct leme_view *view) {
  /* A animação de fecho sobrevive à vista e continua a pedir o desfoque. */
  if (view == NULL || view->fullscreen || view->unmanaged ||
      view->server == NULL || view->server->config == NULL) {
    return 0;
  }
  return view->server->config->blur;
}

static struct leme_box leme_render_view_inset(struct leme_box box,
                                              struct leme_gaps margins) {
  const int width = box.width - margins.left - margins.right;
  const int height = box.height - margins.top - margins.bottom;

  if (width < 1 || height < 1) {
    return box;
  }
  return (struct leme_box){
      .x = leme_render_saturate_coordinate((int64_t)box.x + margins.left),
      .y = leme_render_saturate_coordinate((int64_t)box.y + margins.top),
      .width = width,
      .height = height,
  };
}

static struct leme_gaps
leme_render_view_decoration_insets(const struct leme_view *view,
                                   struct leme_box frame) {
  struct leme_gaps insets =
      leme_decoration_protocol_insets(view, view != NULL && view->fullscreen);

  insets.top += leme_desk_decoration_height(view, frame);
  return insets;
}

struct leme_box
leme_render_view_content_box_for_state(const struct leme_view *view,
                                       struct leme_box frame, bool fullscreen) {
  if (view == NULL || view->server == NULL || view->server->config == NULL ||
      view->unmanaged || frame.width <= 0 || frame.height <= 0) {
    return frame;
  }
  const struct leme_gaps margins =
      leme_decoration_protocol_insets(view, fullscreen);
  if (fullscreen) {
    return leme_render_view_inset(frame, margins);
  }
  const int border = leme_decoration_protocol_replaces_border(view, false)
                         ? 0
                         : leme_render_view_clamp_border(
                               view->server->config->border_width, frame);
  struct leme_desk_decoration_boxes boxes = {0};
  if (view->server->desk_decorations_ready && view->desk_decoration != NULL &&
      leme_desk_decoration_eligible_for_state(view, fullscreen) &&
      leme_desk_decoration_geometry(
          frame, border, leme_desk_style_titlebar_height(view->server->config),
          leme_desk_style_button_width(view->server->config), &boxes)) {
    return leme_render_view_inset(boxes.content, margins);
  }
  frame.x = leme_render_saturate_coordinate((int64_t)frame.x + border);
  frame.y = leme_render_saturate_coordinate((int64_t)frame.y + border);
  frame.width -= border * 2;
  frame.height -= border * 2;
  return leme_render_view_inset(frame, margins);
}

struct leme_box leme_render_view_content_box(const struct leme_view *view,
                                             struct leme_box frame) {
  if (frame.width < 1 || frame.height < 1) {
    frame.width = 0;
    frame.height = 0;
    return frame;
  }
  const int border_width = leme_render_view_border_width(view, frame);
  const int titlebar_height = leme_desk_decoration_height(view, frame);
  frame.x = leme_render_saturate_coordinate((int64_t)frame.x + border_width);
  frame.y = leme_render_saturate_coordinate((int64_t)frame.y + border_width +
                                            titlebar_height);
  frame.width -= border_width * 2;
  frame.height -= border_width * 2 + titlebar_height;
  return leme_render_view_inset(
      frame,
      leme_decoration_protocol_insets(view, view != NULL && view->fullscreen));
}

/*
 * Cada nó do conteúdo guarda a sua medida final: o rácio de cada frame é
 * aplicado sempre à base e nunca ao que sobrou do frame anterior.
 */
static struct leme_animation_buffer_geometry
leme_render_view_buffer_geometry(const struct wlr_scene_buffer *buffer) {
  struct leme_animation_buffer_geometry geometry = {
      .width = buffer->dst_width,
      .height = buffer->dst_height,
      .src_box = buffer->src_box,
      .transform = buffer->transform,
  };

  if (buffer->buffer != NULL) {
    geometry.buffer_width = buffer->buffer->width;
    geometry.buffer_height = buffer->buffer->height;
    if (geometry.width == 0 && geometry.height == 0) {
      geometry.width = buffer->buffer->width;
      geometry.height = buffer->buffer->height;
      wlr_output_transform_coords(buffer->transform, &geometry.width,
                                  &geometry.height);
    }
  }
  return geometry;
}

bool leme_render_view_collect_content(struct wl_array *bases,
                                      struct wlr_scene_tree *tree, int origin_x,
                                      int origin_y) {
  struct wlr_scene_node *node;

  if (tree == NULL) {
    return true;
  }
  wl_list_for_each(node, &tree->children, link) {
    struct leme_render_view_content_base *base =
        wl_array_add(bases, sizeof(*base));
    const int root_x = origin_x + node->x;
    const int root_y = origin_y + node->y;

    if (base == NULL) {
      return false;
    }
    *base = (struct leme_render_view_content_base){
        .node = node,
        .x = node->x,
        .y = node->y,
        .opacity = 1.0f,
        .enabled = node->enabled,
        .root_x = root_x,
        .root_y = root_y,
    };
    if (node->type == WLR_SCENE_NODE_RECT) {
      struct wlr_scene_rect *rect = wl_container_of(node, rect, node);

      base->width = rect->width;
      base->height = rect->height;
      for (size_t component = 0; component < LEME_ARRAY_LENGTH(base->color);
           component++) {
        base->color[component] = rect->color[component];
      }
    } else if (node->type == WLR_SCENE_NODE_BUFFER) {
      struct wlr_scene_buffer *buffer = wl_container_of(node, buffer, node);

      base->width = buffer->dst_width;
      base->height = buffer->dst_height;
      base->opacity = buffer->opacity;
      base->geometry = leme_render_view_buffer_geometry(buffer);
    }
    /* A recursão fica no fim: wl_array_add pode realocar o vector. */
    if (node->type == WLR_SCENE_NODE_TREE) {
      struct wlr_scene_tree *branch = wl_container_of(node, branch, node);

      if (!leme_render_view_collect_content(bases, branch, root_x, root_y)) {
        return false;
      }
    }
  }
  return true;
}

static int leme_render_view_scaled_length(int base, double ratio) {
  int scaled;

  if (base <= 0) {
    return base;
  }
  scaled = (int)((double)base * ratio);
  return scaled < 1 ? 1 : scaled;
}

/*
 * Cada eixo leva o seu rácio: a moldura é uma constante subtraída às duas
 * dimensões, não uma proporção, por isso os rácios não coincidem.
 */
void leme_render_view_scale_nodes(struct wl_array *bases, double ratio_x,
                                  double ratio_y) {
  struct leme_render_view_content_base *base;

  wl_array_for_each(base, bases) {
    wlr_scene_node_set_position(base->node, (int)((double)base->x * ratio_x),
                                (int)((double)base->y * ratio_y));
    if (base->node->type == WLR_SCENE_NODE_RECT) {
      struct wlr_scene_rect *rect = wl_container_of(base->node, rect, node);

      wlr_scene_rect_set_size(
          rect, leme_render_view_scaled_length(base->width, ratio_x),
          leme_render_view_scaled_length(base->height, ratio_y));
    } else if (base->node->type == WLR_SCENE_NODE_BUFFER) {
      struct wlr_scene_buffer *buffer =
          wl_container_of(base->node, buffer, node);

      wlr_scene_buffer_set_dest_size(
          buffer, leme_render_view_scaled_length(base->width, ratio_x),
          leme_render_view_scaled_length(base->height, ratio_y));
    }
  }
}

/* wlr_render_color é pré-multiplicada: o alfa entra nos quatro canais. */
void leme_render_view_fade_nodes(struct wl_array *bases, float alpha) {
  struct leme_render_view_content_base *base;

  wl_array_for_each(base, bases) {
    if (base->node->type == WLR_SCENE_NODE_BUFFER) {
      struct wlr_scene_buffer *buffer =
          wl_container_of(base->node, buffer, node);

      wlr_scene_buffer_set_opacity(buffer, base->opacity * alpha);
#ifdef LEME_HAVE_EFFECTS
      wlr_scene_buffer_set_backdrop_blur_alpha(
          buffer, alpha > 0.0f ? (alpha < 1.0f ? alpha : 1.0f) : 0.0f);
#endif
    } else if (base->node->type == WLR_SCENE_NODE_RECT) {
      struct wlr_scene_rect *rect = wl_container_of(base->node, rect, node);
      float color[4] = {0};
      for (size_t component = 0; component < LEME_ARRAY_LENGTH(color);
           component++) {
        color[component] = base->color[component] * alpha;
      }
      wlr_scene_rect_set_color(rect, color);
    }
  }
}

static void
leme_render_view_animation_fade(struct leme_render_view_animation *state,
                                float alpha) {
  leme_render_view_fade_nodes(&state->content_bases, alpha);
  leme_render_view_fade_nodes(&state->decoration_bases, alpha);
  leme_render_view_fade_nodes(&state->shell_bases, alpha);
  for (size_t index = 0; index < LEME_ARRAY_LENGTH(state->nodes.border);
       index++) {
    const float color[4] = {
        state->border_color[index][0] * alpha,
        state->border_color[index][1] * alpha,
        state->border_color[index][2] * alpha,
        state->border_color[index][3] * alpha,
    };

    if (state->nodes.border[index] == NULL) {
      break;
    }
    wlr_scene_rect_set_color(state->nodes.border[index], color);
  }
}

static int leme_render_view_frame_titlebar(
    const struct leme_render_view_frame_nodes *nodes, struct leme_box box,
    int border_width) {
  struct leme_desk_decoration_boxes boxes = {0};
  return nodes->decoration != NULL && nodes->titlebar_height > 0 &&
                 leme_desk_decoration_geometry(box, border_width,
                                               nodes->titlebar_height,
                                               nodes->button_width, &boxes)
             ? nodes->titlebar_height
             : 0;
}

#ifdef LEME_HAVE_EFFECTS
static bool leme_render_view_has_insets(struct leme_gaps insets) {
  return insets.top > 0 || insets.right > 0 || insets.bottom > 0 ||
         insets.left > 0;
}
#endif

static void leme_render_view_round_content(struct wl_array *bases,
                                           double ratio_x, double ratio_y,
                                           struct leme_box content,
                                           struct leme_gaps insets) {
#ifdef LEME_HAVE_EFFECTS
  struct leme_render_view_content_base *base;

  wl_array_for_each(base, bases) {
    if (base->node->type != WLR_SCENE_NODE_BUFFER) {
      continue;
    }
    struct wlr_scene_buffer *buffer = wl_container_of(base->node, buffer, node);
    if (buffer->corner_radius <= 0.0f) {
      continue;
    }
    const struct wlr_box rounded = {
        .x = -(int)((double)base->root_x * ratio_x) - insets.left,
        .y = -(int)((double)base->root_y * ratio_y) - insets.top,
        .width = content.width + insets.left + insets.right,
        .height = content.height + insets.top + insets.bottom,
    };
    wlr_scene_buffer_set_corner_box(
        buffer, leme_render_view_has_insets(insets) ? &rounded : NULL);
  }
#else
  (void)bases;
  (void)ratio_x;
  (void)ratio_y;
  (void)content;
  (void)insets;
#endif
}

static bool
leme_render_view_collect_shells(struct leme_render_view_animation *state) {
  const struct wlr_scene_tree *content = state->nodes.content;

  for (size_t index = 0; index < LEME_ARRAY_LENGTH(state->nodes.shell);
       index++) {
    struct wlr_scene_tree *shell = state->nodes.shell[index];
    if (shell == NULL || content == NULL) {
      continue;
    }
    state->shell_x[index] = shell->node.x - content->node.x;
    state->shell_y[index] = shell->node.y - content->node.y;
    if (!leme_render_view_collect_content(&state->shell_bases, shell, 0, 0)) {
      return false;
    }
  }
  return true;
}

static void
leme_render_view_place_shells(struct leme_render_view_animation *state,
                              double ratio_x, double ratio_y) {
  const struct wlr_scene_tree *content = state->nodes.content;

  if (content == NULL) {
    return;
  }
  for (size_t index = 0; index < LEME_ARRAY_LENGTH(state->nodes.shell);
       index++) {
    struct wlr_scene_tree *shell = state->nodes.shell[index];
    if (shell != NULL) {
      wlr_scene_node_set_position(
          &shell->node,
          content->node.x + (int)((double)state->shell_x[index] * ratio_x),
          content->node.y + (int)((double)state->shell_y[index] * ratio_y));
    }
  }
  leme_render_view_scale_nodes(&state->shell_bases, ratio_x, ratio_y);
}

static void
leme_render_view_animation_apply(void *data,
                                 const struct leme_animation_frame *frame) {
  struct leme_render_view_animation *state = data;
  struct leme_box box = frame->box;
  int border_width = leme_render_view_clamp_border(state->border_width, box);
  double ratio_x = 1.0;
  double ratio_y = 1.0;

  state->current = box;

  wlr_scene_node_set_position(&state->root->node, box.x, box.y);
  struct leme_titlebar_style titlebar_style = {0};
  leme_render_view_layout_frame(
      &state->nodes, box, border_width,
      leme_render_view_corner_radius(state->view, box),
      leme_render_view_animation_titlebar(state, &titlebar_style));
  const int titlebar =
      leme_render_view_frame_titlebar(&state->nodes, box, border_width);
  struct leme_gaps insets = state->nodes.margins;
  insets.top += titlebar;
  const struct leme_box content = {
      .width = box.width - border_width * 2 - insets.left - insets.right,
      .height = box.height - border_width * 2 - insets.top - insets.bottom,
  };
  if (state->view != NULL) {
    const struct leme_box live = state->view->box;
    leme_render_view_apply_surface_effects(
        state->view, leme_render_view_corner_radius(state->view, box),
        border_width, leme_render_view_blur(state->view),
        leme_render_view_content_box(state->view, live),
        leme_render_view_decoration_insets(state->view, live));
  }
  if (state->final_content_width > 0) {
    ratio_x = (double)content.width / (double)state->final_content_width;
  }
  if (state->final_content_height > 0) {
    ratio_y = (double)content.height / (double)state->final_content_height;
  }
  leme_render_view_scale_nodes(&state->content_bases, ratio_x, ratio_y);
  leme_render_view_place_shells(state, ratio_x, ratio_y);
  leme_render_view_round_content(&state->content_bases, ratio_x, ratio_y,
                                 content, insets);
  leme_render_view_animation_fade(state, (float)frame->opacity);
}

static void leme_render_view_animation_done(void *data) {
  struct leme_render_view_animation *state = data;

  wl_array_release(&state->content_bases);
  wl_array_release(&state->decoration_bases);
  wl_array_release(&state->shell_bases);
  if (state->view != NULL) {
    if (state->restore_on_done && state->view->render_tree != NULL) {
      wlr_scene_node_set_enabled(&state->view->render_tree->node,
                                 !leme_view_is_minimized(state->view));
    }
    state->view->animation_state = NULL;
    leme_input_pointer_rebase(state->view->server);
  }
  free(state);
}

struct wlr_scene_tree *
leme_render_view_snapshot(const struct leme_view *view,
                          struct wlr_scene_tree *parent,
                          struct leme_render_view_frame_nodes *nodes) {
  enum {
    LEME_RENDER_VIEW_PART_CONTENT = LEME_ARRAY_LENGTH(view->border),
    LEME_RENDER_VIEW_PART_TITLEBAR,
    LEME_RENDER_VIEW_PART_SHELL,
    LEME_RENDER_VIEW_PART_COUNT = LEME_RENDER_VIEW_PART_SHELL +
        LEME_RENDER_VIEW_SHELL_MAX,
  };
  struct leme_animation_part parts[LEME_RENDER_VIEW_PART_COUNT] = {0};
  const struct wlr_scene_tree *titlebar = leme_desk_decoration_tree(view);

  for (size_t index = 0; index < LEME_ARRAY_LENGTH(view->border); index++) {
    if (view->border[index] != NULL) {
      parts[index].source = &view->border[index]->node;
    }
  }
  if (view->scene_tree != NULL) {
    parts[LEME_RENDER_VIEW_PART_CONTENT].source = &view->scene_tree->node;
  }
  if (titlebar != NULL) {
    parts[LEME_RENDER_VIEW_PART_TITLEBAR].source = &titlebar->node;
  }
  struct wlr_scene_tree *shell[LEME_RENDER_VIEW_SHELL_MAX] = {0};
  const size_t shells =
      leme_decoration_protocol_trees(view, shell, LEME_ARRAY_LENGTH(shell));
  for (size_t index = 0; index < shells; index++) {
    parts[LEME_RENDER_VIEW_PART_SHELL + index].source = &shell[index]->node;
  }
  struct wlr_scene_tree *snapshot = leme_animation_snapshot_parts(
      view->render_tree, parent, parts, LEME_ARRAY_LENGTH(parts));
  if (snapshot == NULL) {
    return NULL;
  }
  *nodes = (struct leme_render_view_frame_nodes){
      .margins = leme_decoration_protocol_insets(view, view->fullscreen),
      .titlebar_height = leme_desk_decoration_height(view, view->box),
      .button_width = leme_desk_style_button_width(view->server->config),
  };
  for (size_t index = 0; index < LEME_ARRAY_LENGTH(nodes->border); index++) {
    if (parts[index].copy != NULL) {
      nodes->border[index] = wlr_scene_rect_from_node(parts[index].copy);
    }
  }
  if (parts[LEME_RENDER_VIEW_PART_CONTENT].copy != NULL) {
    nodes->content =
        wlr_scene_tree_from_node(parts[LEME_RENDER_VIEW_PART_CONTENT].copy);
  }
  for (size_t index = 0; index < shells; index++) {
    const struct leme_animation_part *part =
        &parts[LEME_RENDER_VIEW_PART_SHELL + index];
    if (part->copy != NULL) {
      nodes->shell[index] = wlr_scene_tree_from_node(part->copy);
    }
  }
  if (parts[LEME_RENDER_VIEW_PART_TITLEBAR].copy != NULL &&
      nodes->titlebar_height > 0) {
    nodes->decoration =
        wlr_scene_tree_from_node(parts[LEME_RENDER_VIEW_PART_TITLEBAR].copy);
    struct wlr_scene_node *child = NULL;
    wl_list_for_each(child, &nodes->decoration->children, link) {
      if (child->type == WLR_SCENE_NODE_RECT) {
        nodes->titlebar_width = wlr_scene_rect_from_node(child)->width;
        break;
      }
    }
  }
  return snapshot;
}

void leme_render_view_apply_snapshot(
    const struct leme_view *view,
    const struct leme_render_view_frame_nodes *nodes, bool activated) {
  const struct leme_config *config = NULL;
  struct leme_view_rules rules;
  float opacity;
  size_t index;

  if (view == NULL || nodes == NULL) {
    return;
  }
  if (view->server != NULL) {
    config = view->server->config;
  }
  if (activated && config != NULL) {
    rules = leme_view_rules_match(config, leme_view_identity(view),
                                  leme_view_title(view));
    opacity = leme_render_view_opacity(config, true, view->fullscreen, &rules);
  } else {
    opacity = view->render_opacity;
  }
  if (nodes->content != NULL) {
    wlr_scene_node_for_each_buffer(&nodes->content->node,
                                   leme_render_view_apply_opacity, &opacity);
  }
  for (index = 0; index < LEME_ARRAY_LENGTH(nodes->border); index++) {
    if (nodes->border[index] != NULL && view->border[index] != NULL) {
      bool shown = leme_render_view_border_shown(view, index);

      wlr_scene_node_set_enabled(&nodes->border[index]->node, shown);
      if (activated && config != NULL) {
        wlr_scene_rect_set_color(nodes->border[index], config->border_active);
      }
    }
  }
  for (index = 0; index < LEME_ARRAY_LENGTH(nodes->shell); index++) {
    if (nodes->shell[index] != NULL) {
      wlr_scene_node_set_enabled(&nodes->shell[index]->node, true);
    }
  }
  if (nodes->decoration != NULL) {
    wlr_scene_node_set_enabled(&nodes->decoration->node,
                               leme_desk_decoration_eligible(view));
  }
}

static struct leme_box leme_render_view_shrunk(struct leme_box box,
                                               double scale_from) {
  struct leme_box small = box;

  small.width = (int)((double)box.width * scale_from);
  small.height = (int)((double)box.height * scale_from);
  small.x = box.x + (box.width - small.width) / 2;
  small.y = box.y + (box.height - small.height) / 2;
  return small;
}

/*
 * Abrir e fechar são a mesma animação com os extremos trocados; o efeito
 * escolhe que campos do frame se movem e os outros ficam no valor final.
 */
static struct leme_animation_spec
leme_render_view_animation_spec(struct leme_box final,
                                const struct leme_animation_settings *settings,
                                bool opening) {
  bool scale = (settings->effects & (uint32_t)LEME_ANIMATION_EFFECT_SCALE) != 0;
  bool fade = (settings->effects & (uint32_t)LEME_ANIMATION_EFFECT_FADE) != 0;
  struct leme_box small =
      scale ? leme_render_view_shrunk(final, settings->scale_from) : final;

  return (struct leme_animation_spec){
      .from = opening ? small : final,
      .to = opening ? final : small,
      .from_opacity = fade && opening ? 0.0 : 1.0,
      .to_opacity = fade && !opening ? 0.0 : 1.0,
      .duration_ms = settings->duration_ms,
      .curve = settings->curve,
      .opacity_curve = settings->opacity_curve,
      .kind = settings->kind,
      .spring = settings->spring,
  };
}

static struct leme_render_view_animation *
leme_render_view_animation_capture(struct leme_view *view) {
  struct leme_render_view_animation *state = calloc(1, sizeof(*state));
  if (state == NULL) {
    return NULL;
  }
  const struct leme_box content = leme_render_view_content_box(view, view->box);
  size_t index;
  wl_array_init(&state->content_bases);
  wl_array_init(&state->decoration_bases);
  wl_array_init(&state->shell_bases);
  state->view = view;
  state->output = leme_view_is_shown_scratchpad(view) &&
                          view->server->scratchpads.shown == view
                      ? leme_ownership_effective_output(view)
                      : (leme_ownership_tag(view) == NULL ||
                                 leme_ownership_tag(view)->owner == NULL
                             ? NULL
                             : leme_ownership_tag(view)->owner->output);
  state->border_width = leme_render_view_border_width(view, view->box);
  state->final_content_width = content.width;
  state->final_content_height = content.height;
  state->root = leme_render_view_snapshot(view, leme_render_view_parent(view),
                                          &state->nodes);
  if (state->root != NULL) {
    if (leme_render_view_collect_content(&state->content_bases,
                                         state->nodes.content, 0, 0) &&
        leme_render_view_collect_content(&state->decoration_bases,
                                         state->nodes.decoration, 0, 0) &&
        leme_render_view_collect_shells(state)) {
      for (index = 0; index < LEME_ARRAY_LENGTH(state->nodes.border); index++) {
        if (state->nodes.border[index] != NULL) {
          for (size_t component = 0;
               component < LEME_ARRAY_LENGTH(state->border_color[index]);
               component++) {
            state->border_color[index][component] =
                state->nodes.border[index]->color[component];
          }
        }
      }
    } else {
      leme_animation_snapshot_destroy(state->root);
      state->root = NULL;
    }
  }
  return state;
}

static void
leme_render_view_animation_begin(struct leme_view *view,
                                 const struct leme_animation_settings *settings,
                                 bool opening) {
  if (settings->effects == 0) {
    goto abandon;
  }
  struct leme_render_view_animation *state =
      leme_render_view_animation_capture(view);
  if (state == NULL) {
    goto abandon;
  }
  state->restore_on_done = opening;
  view->animation_state = state;
  const struct leme_animation_spec spec =
      leme_render_view_animation_spec(view->box, settings, opening);
  const struct leme_animation_subject subject = {
      .data = state,
      .owner = state->output,
      .apply = leme_render_view_animation_apply,
      .done = leme_render_view_animation_done,
  };
  /* Um instantâneo nulo não impede nada: run() abandona e corre done(). */
  leme_animation_run(&view->server->animations, state->root, &spec, &subject);
  return;

abandon:
  if (opening && view->render_tree != NULL) {
    wlr_scene_node_set_enabled(&view->render_tree->node,
                               !leme_view_is_minimized(view));
  }
}

static struct leme_box
leme_render_view_committed_content(const struct leme_view *view,
                                   struct leme_box fallback) {
  struct leme_box content = leme_render_view_content_box(view, fallback);

  if (view->kind == LEME_VIEW_XDG && view->xdg_toplevel != NULL &&
      view->xdg_toplevel->base != NULL &&
      view->xdg_toplevel->base->geometry.width > 0 &&
      view->xdg_toplevel->base->geometry.height > 0) {
    content.width = view->xdg_toplevel->base->geometry.width;
    content.height = view->xdg_toplevel->base->geometry.height;
  } else if (view->kind == LEME_VIEW_XWAYLAND &&
             view->xwayland_surface != NULL &&
             view->xwayland_surface->surface != NULL &&
             view->xwayland_surface->surface->current.width > 0 &&
             view->xwayland_surface->surface->current.height > 0) {
    content.width = view->xwayland_surface->surface->current.width;
    content.height = view->xwayland_surface->surface->current.height;
  }
  return content;
}

bool leme_render_view_move_settled(const struct leme_view *view) {
  const struct wlr_xdg_surface *base;

  if (view != NULL && view->configure_dirty) {
    return false;
  }
  if (view == NULL || view->kind != LEME_VIEW_XDG ||
      view->xdg_toplevel == NULL || view->xdg_toplevel->base == NULL) {
    return true;
  }
  base = view->xdg_toplevel->base;
  return base->current.configure_serial >= base->scheduled_serial;
}

static bool leme_render_view_move_ready(void *data) {
  const struct leme_render_view_animation *state = data;
  return state->view != NULL && state->view->container != NULL
             ? leme_render_container_ready(state->view->container)
             : leme_render_view_move_settled(state->view);
}

static void leme_render_view_copy_crop(struct leme_render_view_copy *copy,
                                       int width, int height) {
  struct leme_render_view_content_base *base;

  wl_array_for_each(base, &copy->content_bases) {
    const int right = base->root_x + base->width;
    const int bottom = base->root_y + base->height;
    const int left = base->root_x > 0 ? base->root_x : 0;
    const int top = base->root_y > 0 ? base->root_y : 0;
    const int visible_width = (right < width ? right : width) - left;
    const int visible_height = (bottom < height ? bottom : height) - top;

    if (base->node->type == WLR_SCENE_NODE_TREE) {
      continue;
    }
    if (!base->enabled || visible_width <= 0 || visible_height <= 0) {
      wlr_scene_node_set_enabled(base->node, false);
      continue;
    }
    wlr_scene_node_set_enabled(base->node, true);
    wlr_scene_node_set_position(base->node, base->x + left - base->root_x,
                                base->y + top - base->root_y);
    if (base->node->type == WLR_SCENE_NODE_RECT) {
      struct wlr_scene_rect *rect = wl_container_of(base->node, rect, node);

      wlr_scene_rect_set_size(rect, visible_width, visible_height);
    } else if (base->node->type == WLR_SCENE_NODE_BUFFER) {
      struct wlr_scene_buffer *buffer =
          wl_container_of(base->node, buffer, node);

      leme_animation_crop_buffer(buffer, &base->geometry, left - base->root_x,
                                 top - base->root_y, visible_width,
                                 visible_height);
#ifdef LEME_HAVE_EFFECTS
      struct leme_gaps insets = copy->nodes.margins;
      insets.top += copy->nodes.titlebar_height;
      wlr_scene_buffer_set_corner_box(
          buffer, &(struct wlr_box){
                      .x = -left - insets.left,
                      .y = -top - insets.top,
                      .width = width + insets.left + insets.right,
                      .height = height + insets.top + insets.bottom,
                  });
#endif
    }
  }
}

static int64_t leme_render_view_uncovered(int content_width, int content_height,
                                          int width, int height) {
  const int covered_width = width < content_width ? width : content_width;
  const int covered_height = height < content_height ? height : content_height;

  return (int64_t)width * height - (int64_t)covered_width * covered_height;
}

static void
leme_render_view_move_capture(struct leme_render_view_animation *state) {
  struct leme_view *view = state->view;
  struct leme_render_view_copy *copy = &state->redrawn;
  const struct leme_box natural =
      leme_render_view_committed_content(view, view->box);

  copy->root = leme_render_view_snapshot(view, leme_render_view_parent(view),
                                         &copy->nodes);
  if (copy->root == NULL) {
    return;
  }
  leme_render_view_apply_snapshot(view, &copy->nodes,
                                  view == view->server->focused_view);
  if (!leme_render_view_collect_content(&copy->content_bases,
                                        copy->nodes.content, 0, 0)) {
    leme_animation_snapshot_destroy(copy->root);
    copy->root = NULL;
    return;
  }
  copy->content_width = natural.width;
  copy->content_height = natural.height;
}

static void
leme_render_view_move_apply(void *data,
                            const struct leme_animation_frame *frame) {
  struct leme_render_view_animation *state = data;
  const struct leme_box box = frame->box;
  const int border_width =
      leme_render_view_clamp_border(state->border_width, box);
  const struct leme_gaps margins = state->nodes.margins;
  const int width = box.width - border_width * 2 - margins.left - margins.right;
  const int height =
      box.height - border_width * 2 - margins.top - margins.bottom -
      leme_render_view_frame_titlebar(&state->nodes, box, border_width);
  struct leme_render_view_copy original = {
      .root = state->root,
      .nodes = state->nodes,
      .content_bases = state->content_bases,
      .content_width = state->final_content_width,
      .content_height = state->final_content_height,
  };
  struct leme_render_view_copy *shown = &original;
  struct wlr_scene_tree *hidden = state->redrawn.root;

  state->current = box;
  if (state->view == NULL) {
    return;
  }
  if (state->redrawn.root == NULL && leme_render_view_move_ready(state)) {
    wl_array_init(&state->redrawn.content_bases);
    leme_render_view_move_capture(state);
    hidden = state->redrawn.root;
  }
  if (state->redrawn.root != NULL &&
      leme_render_view_uncovered(state->redrawn.content_width,
                                 state->redrawn.content_height, width,
                                 height) <=
          leme_render_view_uncovered(original.content_width,
                                     original.content_height, width, height)) {
    shown = &state->redrawn;
    hidden = state->root;
  }
  if (hidden != NULL) {
    wlr_scene_node_set_enabled(&hidden->node, false);
  }
  if (shown->root == NULL) {
    return;
  }
  wlr_scene_node_set_enabled(&shown->root->node, true);
  wlr_scene_node_set_position(&shown->root->node, box.x, box.y);
  struct leme_titlebar_style titlebar_style = {0};
  leme_render_view_layout_frame(
      &shown->nodes, box, border_width,
      leme_render_view_corner_radius(state->view, box),
      leme_render_view_animation_titlebar(state, &titlebar_style));
  leme_render_view_copy_crop(shown, width, height);
}

static void leme_render_view_move_done(void *data) {
  struct leme_render_view_animation *state = data;
  struct leme_view *view = state->view;

  if (state->redrawn.root != NULL) {
    leme_animation_snapshot_destroy(state->redrawn.root);
    wl_array_release(&state->redrawn.content_bases);
  }
  wl_array_release(&state->content_bases);
  wl_array_release(&state->decoration_bases);
  wl_array_release(&state->shell_bases);
  if (view != NULL) {
    view->animation_state = NULL;
    leme_render_view_sync_presentation(view);
    const struct leme_box content =
        leme_render_view_content_box(view, view->box);
    leme_render_view_apply_surface_effects(
        view, leme_render_view_corner_radius(view, view->box),
        leme_render_view_border_width(view, view->box),
        leme_render_view_blur(view), content,
        leme_render_view_decoration_insets(view, view->box));
    leme_input_pointer_rebase(view->server);
  }
  free(state);
}

static bool leme_render_view_move_allowed(const struct leme_view *view,
                                          struct leme_box from) {
  const enum leme_view_geometry_owner owner = leme_view_geometry_owner(view);
  const struct leme_tag *tag = leme_ownership_tag(view);
  return view->server->config != NULL &&
         view->server->config->animation[LEME_ANIMATION_MOVE].configured &&
         view->mapped && view->render_tree != NULL &&
         view->render_tree->node.enabled &&
         (owner == LEME_VIEW_GEOMETRY_TAG_LAYOUT ||
          owner == LEME_VIEW_GEOMETRY_CONTAINER ||
          (owner == LEME_VIEW_GEOMETRY_FLOATING && tag != NULL &&
           tag->layout.kind == LEME_LAYOUT_DESKTOP)) &&
         !view->detached && from.width > 0 && from.height > 0 &&
         leme_render_view_animation_allowed(view);
}

bool leme_render_view_animation_box(const struct leme_view *view,
                                    struct leme_box *out) {
  const struct leme_render_view_animation *state =
      view == NULL ? NULL : view->animation_state;
  if (state == NULL || !state->moving || out == NULL) {
    return false;
  }
  *out = state->current;
  return true;
}

void leme_render_view_finish_move(struct leme_view *view) {
  const struct leme_render_view_animation *state =
      view == NULL ? NULL : view->animation_state;

  if (state != NULL && state->moving) {
    leme_render_view_finish_animation(view);
  }
}

void leme_render_view_move(struct leme_view *view, struct leme_box from,
                           struct leme_box to) {
  const struct leme_render_view_animation *running;
  struct leme_render_view_animation *state;
  struct leme_box start = from;
  size_t index;

  if (view == NULL || view->server == NULL || view->open_animation_pending) {
    return;
  }
  running = view->animation_state;
  if (running != NULL && running->moving) {
    start = running->current;
  }
  if (view->animation_state != NULL) {
    leme_render_view_finish_animation(view);
  }
  if (!leme_render_view_move_allowed(view, from)) {
    return;
  }
  state = calloc(1, sizeof(*state));
  if (state == NULL) {
    return;
  }
  wl_array_init(&state->content_bases);
  state->view = view;
  state->output = leme_ownership_tag(view) == NULL ||
                          leme_ownership_tag(view)->owner == NULL
                      ? NULL
                      : leme_ownership_tag(view)->owner->output;
  state->moving = true;
  state->current = start;
  state->border_width = leme_render_view_border_width(view, to);
  const struct leme_box natural =
      leme_render_view_committed_content(view, start);
  state->final_content_width = natural.width;
  state->final_content_height = natural.height;
  state->root = leme_render_view_snapshot(view, leme_render_view_parent(view),
                                          &state->nodes);
  if (state->root != NULL) {
    leme_render_view_apply_snapshot(view, &state->nodes,
                                    view == view->server->focused_view);
    if (leme_render_view_collect_content(&state->content_bases,
                                         state->nodes.content, 0, 0)) {
      for (index = 0; index < LEME_ARRAY_LENGTH(state->nodes.border); index++) {
        if (state->nodes.border[index] != NULL) {
          for (size_t component = 0;
               component < LEME_ARRAY_LENGTH(state->border_color[index]);
               component++) {
            state->border_color[index][component] =
                state->nodes.border[index]->color[component];
          }
        }
      }
    } else {
      leme_animation_snapshot_destroy(state->root);
      state->root = NULL;
    }
  }
  view->animation_state = state;
  leme_render_view_sync_presentation(view);

  const struct leme_animation_settings *settings =
      &view->server->config->animation[LEME_ANIMATION_MOVE];
  struct leme_animation_spec spec =
      leme_animation_move_spec(settings, start, to, 1.0, 1.0);
  spec.hold_max_ms = LEME_ANIMATION_OPEN_TIMEOUT_MS;
  const struct leme_animation_subject subject = {
      .gate = view->container == NULL ? NULL : view->container->animation_gate,
      .data = state,
      .owner = state->output,
      .apply = leme_render_view_move_apply,
      .done = leme_render_view_move_done,
      .ready = leme_render_view_move_ready,
  };
  leme_animation_run(&view->server->animations, state->root, &spec, &subject);
}

bool leme_render_view_flying(const struct leme_view *view) {
  const struct leme_render_view_animation *state =
      view == NULL ? NULL : view->animation_state;
  return state != NULL && state->flying;
}

void leme_render_view_fly(struct leme_view *view, struct leme_box from,
                          struct leme_box to, bool appearing) {
  if (view == NULL || view->server == NULL || view->render_tree == NULL ||
      !view->mapped || view->fullscreen || view->unmanaged ||
      view->server->config == NULL ||
      !view->server->config->animation[LEME_ANIMATION_MOVE].configured ||
      leme_render_workspace_transition_hides_view(view) ||
      (view->server->session_protocols != NULL &&
       view->server->session_protocols->locked)) {
    return;
  }
  const struct leme_tag *tag = leme_ownership_tag(view);
  if (tag == NULL || tag->owner == NULL || tag->owner->focused_is_candidate ||
      tag->id != tag->owner->focused_id) {
    return;
  }
  leme_render_view_finish_animation(view);
  struct leme_animation_gate *gate = view->container == NULL
                                         ? leme_animation_gate_create(200)
                                         : view->container->animation_gate;
  if (gate == NULL) {
    return;
  }
  struct leme_render_view_animation *state =
      leme_render_view_animation_capture(view);
  if (state == NULL) {
    if (view->container == NULL) {
      leme_animation_gate_destroy(gate);
    }
    return;
  }
  state->moving = true;
  state->flying = true;
  state->current = from;
  view->animation_state = state;
  leme_render_view_sync_presentation(view);
  const struct leme_animation_settings *settings =
      &view->server->config->animation[LEME_ANIMATION_MOVE];
  const struct leme_animation_spec spec = leme_animation_move_spec(
      settings, from, to, appearing ? 0.0 : 1.0, appearing ? 1.0 : 0.0);
  const struct leme_animation_subject subject = {
      .gate = gate,
      .data = state,
      .owner = state->output,
      .apply = leme_render_view_animation_apply,
      .done = leme_render_view_move_done,
      .ready = appearing ? leme_render_view_move_ready : NULL};
  if (state->root != NULL) {
    const struct leme_animation_frame initial =
        leme_animation_frame_at(&spec, 0.0, 0.0);
    leme_render_view_animation_apply(state, &initial);
  }
  leme_animation_run(&view->server->animations, state->root, &spec, &subject);
  if (view->container == NULL) {
    leme_animation_gate_destroy(gate);
  }
}

struct leme_box leme_render_view_frame_box(const struct leme_view *view,
                                           struct leme_box content) {
  return leme_render_view_frame_box_for_state(view, content,
                                              view != NULL && view->fullscreen);
}

struct leme_box
leme_render_view_frame_box_for_state(const struct leme_view *view,
                                     struct leme_box content, bool fullscreen) {
  int border_width;

  if (view == NULL || view->server == NULL || fullscreen || view->unmanaged ||
      view->server->config == NULL) {
    return content;
  }
  border_width = leme_decoration_protocol_replaces_border(view, fullscreen)
                     ? 0
                     : view->server->config->border_width;
  const bool decorated =
      view->server->desk_decorations_ready && view->desk_decoration != NULL &&
      leme_desk_decoration_eligible_for_state(view, fullscreen);
  const struct leme_gaps margins =
      leme_decoration_protocol_insets(view, fullscreen);
  content.x =
      leme_render_saturate_coordinate((int64_t)content.x - margins.left);
  content.y = leme_render_saturate_coordinate((int64_t)content.y - margins.top);
  content.width = leme_render_saturate_coordinate((int64_t)content.width +
                                                  margins.left + margins.right);
  content.height = leme_render_saturate_coordinate(
      (int64_t)content.height + margins.top + margins.bottom);
  content.x =
      leme_render_saturate_coordinate((int64_t)content.x - border_width);
  content.y =
      leme_render_saturate_coordinate((int64_t)content.y - border_width);
  content.width = leme_render_saturate_coordinate((int64_t)content.width +
                                                  (int64_t)border_width * 2);
  content.height = leme_render_saturate_coordinate((int64_t)content.height +
                                                   (int64_t)border_width * 2);
  const int titlebar = leme_desk_style_titlebar_height(view->server->config);
  if (decorated && content.height <= INT_MAX - titlebar &&
      content.y >= INT_MIN + titlebar) {
    struct leme_box frame = content;
    frame.y -= titlebar;
    frame.height += titlebar;
    struct leme_desk_decoration_boxes boxes = {0};
    if (leme_desk_decoration_geometry(
            frame, border_width, titlebar,
            leme_desk_style_button_width(view->server->config), &boxes)) {
      return frame;
    }
  }
  return content;
}

static struct leme_box leme_render_view_local_box(const struct leme_view *view,
                                                  struct leme_box global) {
  if (view == NULL) {
    return (struct leme_box){0};
  }
  global.x -= view->box.x;
  global.y -= view->box.y;
  return global;
}

bool leme_render_view_show_drop_preview(struct leme_view *view,
                                        struct leme_box global) {
  static const float color[4] = {0.16f, 0.42f, 0.72f, 0.30f};
  struct leme_box local;

  if (view == NULL || view->render_tree == NULL || global.width <= 0 ||
      global.height <= 0) {
    return false;
  }
  local = leme_render_view_local_box(view, global);
  if (view->drop_preview == NULL) {
    view->drop_preview = wlr_scene_rect_create(view->render_tree, local.width,
                                               local.height, color);
    if (view->drop_preview == NULL) {
      return false;
    }
  } else {
    wlr_scene_rect_set_size(view->drop_preview, local.width, local.height);
  }
  wlr_scene_node_set_position(&view->drop_preview->node, local.x, local.y);
  wlr_scene_node_raise_to_top(&view->drop_preview->node);
  wlr_scene_node_set_enabled(&view->drop_preview->node, true);
  return true;
}

void leme_render_view_hide_drop_preview(struct leme_view *view) {
  if (view != NULL && view->drop_preview != NULL) {
    wlr_scene_node_set_enabled(&view->drop_preview->node, false);
  }
}

#ifdef LEME_HAVE_EFFECTS
struct leme_render_surface_effects {
  struct wlr_surface *surface;
  float radius;
  float blur;
  int origin_x;
  int origin_y;
  int width;
  int height;
  struct leme_gaps insets;
};

static void leme_render_view_apply_surface_effects_to_buffer(
    struct wlr_scene_buffer *buffer, int sx, int sy, void *data) {
  const struct leme_render_surface_effects *target = data;
  struct wlr_scene_surface *scene_surface =
      wlr_scene_surface_try_from_buffer(buffer);

  /*
   * Só a superfície de topo. Arredondar cada subsuperfície recortava-as
   * uma a uma, em vez de arredondar a janela. O desfoque segue a mesma
   * regra: cada subsuperfície desfocaria o que a de topo já desenhou.
   */
  if (scene_surface != NULL && scene_surface->surface == target->surface) {
    wlr_scene_buffer_set_corner_radius(buffer, target->radius);
    wlr_scene_buffer_set_backdrop_blur(buffer, target->blur);
    const struct leme_gaps insets = target->insets;
    const struct wlr_box rounded = {
        .x = target->origin_x - sx - insets.left,
        .y = target->origin_y - sy - insets.top,
        .width = target->width + insets.left + insets.right,
        .height = target->height + insets.top + insets.bottom,
    };
    wlr_scene_buffer_set_corner_box(
        buffer, leme_render_view_has_insets(insets) ? &rounded : NULL);
  }
}
#endif

/*
 * O conteúdo segue a curva interior do anel, para que as duas se encontrem
 * sem costura. O desfoque assenta na mesma superfície, porque é atrás dela
 * que o fundo tem de ser lido.
 */
static void leme_render_view_apply_surface_effects(
    struct leme_view *view,
    int corner_radius, // NOLINT(bugprone-easily-swappable-parameters)
    int border_width, int blur, struct leme_box content,
    struct leme_gaps insets) {
#ifdef LEME_HAVE_EFFECTS
  int inner = corner_radius - border_width;

  if (view == NULL) {
    return;
  }
  /*
   * A superfície é lida aqui em vez de vir de leme_view_surface: os testes
   * de geometria ligam só este ficheiro, e chamar a camada shell
   * obrigava-os a arrastar o resto.
   */
  struct wlr_surface *surface =
      view->kind == LEME_VIEW_XDG
          ? (view->xdg_toplevel != NULL && view->xdg_toplevel->base != NULL
                 ? view->xdg_toplevel->base->surface
                 : NULL)
          : (view->xwayland_surface != NULL ? view->xwayland_surface->surface
                                            : NULL);
  struct leme_render_surface_effects target = {
      .surface = surface,
      .radius = (float)(inner > 0 ? inner : 0),
      .blur = (float)(blur > 0 ? blur : 0),
      .origin_x = view->scene_tree == NULL ? 0 : view->scene_tree->node.x,
      .origin_y = view->scene_tree == NULL ? 0 : view->scene_tree->node.y,
      .width = content.width,
      .height = content.height,
      .insets = insets,
  };

  if (view->scene_tree != NULL && target.surface != NULL) {
    wlr_scene_node_for_each_buffer(
        &view->scene_tree->node,
        leme_render_view_apply_surface_effects_to_buffer, &target);
  }
#else
  (void)view;
  (void)corner_radius;
  (void)border_width;
  (void)blur;
  (void)content;
  (void)insets;
#endif
}

/* A moldura é desenhada a partir de uma caixa, seja ela a viva ou a animada. */
void leme_render_view_layout_frame(
    const struct leme_render_view_frame_nodes *nodes, struct leme_box box,
    int border_width, // NOLINT(bugprone-easily-swappable-parameters)
    int corner_radius, const struct leme_titlebar_style *titlebar) {
  size_t index;

#ifndef LEME_HAVE_EFFECTS
  (void)corner_radius;
#endif

  const int titlebar_height =
      leme_render_view_frame_titlebar(nodes, box, border_width);
  leme_desk_frame_layout(nodes->decoration, box, border_width, titlebar_height,
                         nodes->titlebar_width, titlebar, corner_radius);
  if (nodes->content != NULL) {
    wlr_scene_node_set_position(
        &nodes->content->node, border_width + nodes->margins.left,
        border_width + titlebar_height + nodes->margins.top);
  }
  for (index = 0; index < LEME_ARRAY_LENGTH(nodes->border); index++) {
    if (nodes->border[index] == NULL) {
      return;
    }
  }
  if (border_width > 0) {
#ifdef LEME_HAVE_EFFECTS
    /*
     * Um só rectângulo em anel cobre a moldura toda. O meio fica vago,
     * para que uma janela translúcida continue a mostrar o que está por
     * trás e não a cor da moldura.
     */
    wlr_scene_rect_set_size(nodes->border[0], box.width, box.height);
    wlr_scene_node_set_position(&nodes->border[0]->node, 0, 0);
    wlr_scene_rect_set_corner_radius(nodes->border[0], (float)corner_radius,
                                     (float)border_width);
#else
    int content_height = box.height - border_width * 2;

    wlr_scene_rect_set_size(nodes->border[0], box.width, border_width);
    wlr_scene_rect_set_size(nodes->border[1], box.width, border_width);
    wlr_scene_rect_set_size(nodes->border[2], border_width, content_height);
    wlr_scene_rect_set_size(nodes->border[3], border_width, content_height);
    wlr_scene_node_set_position(&nodes->border[0]->node, 0, 0);
    wlr_scene_node_set_position(&nodes->border[1]->node, 0,
                                box.height - border_width);
    wlr_scene_node_set_position(&nodes->border[2]->node, 0, border_width);
    wlr_scene_node_set_position(&nodes->border[3]->node,
                                box.width - border_width, border_width);
#endif
  }
  for (index = 0; index < LEME_ARRAY_LENGTH(nodes->border); index++) {
#ifdef LEME_HAVE_EFFECTS
    bool shown = index == 0 && border_width > 0;
#else
    bool shown = border_width > 0;
#endif

    wlr_scene_node_set_enabled(&nodes->border[index]->node, shown);
  }
}

static struct wlr_box leme_render_view_slot_clip(const struct leme_view *view,
                                                 struct wlr_box clip,
                                                 struct wlr_box geometry) {
  if (leme_view_geometry_owner(view) == LEME_VIEW_GEOMETRY_FLOATING) {
    return clip;
  }
  const struct leme_box slot = leme_render_view_content_box(view, view->box);
  if (slot.width < 1 || slot.height < 1 ||
      (geometry.width <= slot.width && geometry.height <= slot.height)) {
    return clip;
  }
  return (struct wlr_box){
      .x = geometry.x,
      .y = geometry.y,
      .width = geometry.width < slot.width ? geometry.width : slot.width,
      .height = geometry.height < slot.height ? geometry.height : slot.height,
  };
}

/*
 * Uma janela com margens invisíveis compromete uma superfície maior do que a
 * sua geometria, e é essa superfície que leva o desfoque e os cantos. Sem este
 * recorte os efeitos passam para fora da moldura, e até para a saída ao lado.
 */
void leme_render_view_clip_to_geometry(struct leme_view *view) {
  struct wlr_box geometry = {0};
  bool needs_clip = false;

  if (view == NULL || view->scene_tree == NULL) {
    return;
  }
  const bool tiled =
      leme_view_geometry_owner(view) != LEME_VIEW_GEOMETRY_FLOATING &&
      !view->fullscreen && !view->unmanaged;
  const struct leme_box content = leme_render_view_content_box(view, view->box);
  struct leme_render_view_state *state = view->render_state;

  if (view->kind == LEME_VIEW_XDG) {
    if (view->xdg_toplevel == NULL || view->xdg_toplevel->base == NULL ||
        view->xdg_toplevel->base->surface == NULL) {
      return;
    }
    const struct wlr_surface *surface = view->xdg_toplevel->base->surface;
    const struct wlr_xdg_toplevel_state *limits = &view->xdg_toplevel->current;

    geometry = view->xdg_toplevel->base->geometry;
    if (tiled) {
      const int left = content.x - view->box.x;
      const int top = content.y - view->box.y;
      const int dx = limits->max_width > 0 &&
                             limits->max_width < content.width &&
                             geometry.width < content.width
                         ? (content.width - geometry.width) / 2
                         : 0;
      const int dy = limits->max_height > 0 &&
                             limits->max_height < content.height &&
                             geometry.height < content.height
                         ? (content.height - geometry.height) / 2
                         : 0;

      wlr_scene_node_set_position(&view->scene_tree->node, left + dx, top + dy);
    }
    if (tiled && geometry.width > content.width) {
      geometry.width = content.width;
    }
    if (tiled && geometry.height > content.height) {
      geometry.height = content.height;
    }
    needs_clip = geometry.width > 0 && geometry.height > 0 &&
                 !(geometry.x <= 0 && geometry.y <= 0 &&
                   geometry.width >= surface->current.width &&
                   geometry.height >= surface->current.height);
  } else if (tiled) {
    geometry.width = content.width;
    geometry.height = content.height;
    needs_clip = geometry.width > 0 && geometry.height > 0;
  } else if (state == NULL || !state->clip_applied) {
    return;
  }

  const struct wlr_box clip = needs_clip ? geometry : (struct wlr_box){0};
  const struct wlr_box surface_clip =
      leme_render_view_slot_clip(view, clip, geometry);

  if (state != NULL && state->clip_applied &&
      wlr_box_equal(&state->clip, &clip) &&
      wlr_box_equal(&state->surface_clip, &surface_clip)) {
    return;
  }
  wlr_scene_subsurface_tree_set_clip(&view->scene_tree->node,
                                     needs_clip ? &geometry : NULL);
  if (!wlr_box_equal(&surface_clip, &clip) &&
      !wl_list_empty(&view->scene_tree->children)) {
    struct wlr_scene_node *surface_node =
        wl_container_of(view->scene_tree->children.next, surface_node, link);
    wlr_scene_subsurface_tree_set_clip(surface_node, &surface_clip);
  }
  leme_render_view_apply_cached_opacity(view);
  if (state != NULL) {
    state->clip = clip;
    state->surface_clip = surface_clip;
    state->clip_applied = true;
  }
}

void leme_render_view_set_box(struct leme_view *view, struct leme_box box) {
  struct leme_render_view_frame_nodes nodes = {0};
  struct leme_box content;
  int border_width;
  size_t index;

  const struct leme_box previous_content =
      leme_render_view_content_box(view, box);
  const bool current = box.x == view->box.x && box.y == view->box.y &&
                       box.width == view->box.width &&
                       box.height == view->box.height;
  (void)leme_desk_decoration_sync(view);
  if (current) {
    box = view->box;
  }
  leme_view_remember_desktop_box(view, box);
  leme_public_server_invalidate(view->server);
  if (view->render_tree == NULL || view->scene_tree == NULL) {
    return;
  }
  content = leme_render_view_content_box(view, box);
  border_width = leme_render_view_border_width(view, box);
  wlr_scene_node_set_position(&view->render_tree->node, box.x, box.y);
  for (index = 0; index < LEME_ARRAY_LENGTH(view->border); index++) {
    nodes.border[index] = view->border[index];
  }
  nodes.content = view->scene_tree;
  nodes.decoration = leme_desk_decoration_tree(view);
  nodes.margins = leme_decoration_protocol_insets(view, view->fullscreen);
  nodes.titlebar_height = leme_desk_decoration_height(view, box);
  nodes.button_width = leme_desk_style_button_width(view->server->config);
  nodes.titlebar_width = content.width;
  struct leme_titlebar_style titlebar_style = {0};
  leme_render_view_layout_frame(
      &nodes, box, border_width, leme_render_view_corner_radius(view, box),
      leme_render_view_titlebar(view, &titlebar_style));
  leme_decoration_protocol_view_arrange(view, content.x - box.x,
                                        content.y - box.y);
  leme_render_view_clip_to_geometry(view);
  leme_render_view_sync_presentation(view);
  leme_render_view_apply_surface_effects(
      view, leme_render_view_corner_radius(view, box), border_width,
      leme_render_view_blur(view), content,
      leme_render_view_decoration_insets(view, box));
  if (view->kind == LEME_VIEW_XWAYLAND) {
    leme_render_xwayland_set_geometry(view);
  }
  if ((leme_view_geometry_owner(view) != LEME_VIEW_GEOMETRY_FLOATING &&
       !view->detached) ||
      view->fullscreen || view->kind == LEME_VIEW_XWAYLAND ||
      content.width != previous_content.width ||
      content.height != previous_content.height) {
    if (view->configure_deferred) {
      view->deferred_configure_box = content;
      view->configure_dirty = true;
      return;
    }
    leme_view_configure(view, content);
  }
}

void leme_render_refresh_views(struct leme_server *server) {
  struct leme_view *view;

  wl_list_for_each(view, &server->views, link) {
    if (view->mapped) {
      leme_render_view_set_box(view, view->box);
      leme_render_view_set_activated(view, view == server->focused_view);
    }
  }
}
