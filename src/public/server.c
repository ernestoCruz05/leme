#include "public/server.h"
#include "public/model-internal.h"
#include "public/value-internal.h"
#include "core/server.h"
#include "config/public.h"
#include "protocols/session.h"
#include "shell/public.h"
#include "shell/view.h"
#include "output/public.h"
#include "input/public.h"
#include "workspace/public.h"
#include "output/output.h"
#include "config/config.h"
#include "render/graphics.h"
#include <wlr/backend/session.h>

struct leme_public_features
leme_public_server_features(const struct leme_server *server) {
  struct leme_public_features result = {0};
#ifdef LEME_HAVE_EFFECTS
  result.effects_build = true;
#endif
  if (server != NULL) {
    result.effects_runtime_known = true;
    result.effects_runtime = leme_graphics_effects_supported(server);
    result.json_control = true;
    result.query = true;
    result.actions = true;
    result.config_writes = true;
    result.watch = true;
  }
  return result;
}

bool leme_public_server_init(struct leme_server *server) {
  if (server == NULL)
    return false;
  if (server->public_model != NULL)
    return leme_public_model_available(server->public_model);
  return leme_public_model_create(NULL, &server->public_model) ==
         LEME_PUBLIC_OK;
}

bool leme_public_server_prepare(struct leme_server *server) {
  struct leme_public_snapshot *snapshot = NULL;
  const enum leme_public_status status = leme_public_server_capture(
      server, LEME_PUBLIC_ROOT_BIT(LEME_PUBLIC_RUNTIME), &snapshot);
  leme_public_snapshot_unref(snapshot);
  return status == LEME_PUBLIC_OK;
}

void leme_public_server_finish(struct leme_server *server) {
  if (server == NULL)
    return;
  leme_public_model_destroy(server->public_model);
  server->public_model = NULL;
}

void leme_public_server_invalidate(struct leme_server *server) {
  if (server != NULL)
    leme_public_model_invalidate(server->public_model);
}

void leme_public_server_config_changed(struct leme_server *server) {
  if (server == NULL)
    return;
  if (server->public_config_generation == (UINT64_MAX >> 8)) {
    leme_public_model_disable(server->public_model);
    return;
  }
  ++server->public_config_generation;
  leme_public_server_invalidate(server);
}

void leme_public_server_lock_changed(struct leme_server *server, bool locked) {
  if (server != NULL)
    leme_public_model_lock_changed(server->public_model, locked);
}

void leme_public_server_view_mapped(struct leme_view *view) {
  if (view == NULL || view->server == NULL || !view->mapped ||
      view->unmanaged ||
      !leme_public_model_available(view->server->public_model))
    return;
  if (view->public_meta.id.serial == 0 &&
      leme_public_model_issue_id(view->server->public_model,
                                 &view->public_meta.id) != LEME_PUBLIC_OK)
    return;
  view->public_meta.ever_mapped = true;
  leme_public_server_invalidate(view->server);
}

void leme_public_server_view_focused(struct leme_view *view, bool changed) {
  if (view == NULL || view->server == NULL || !view->mapped ||
      view->unmanaged || !view->public_meta.ever_mapped ||
      !leme_public_model_available(view->server->public_model))
    return;
  view->public_meta.urgent = false;
  view->public_meta.urgent_since = 0;
  if (changed && leme_public_model_next_order(
                     view->server->public_model,
                     &view->public_meta.last_focused) != LEME_PUBLIC_OK)
    return;
  leme_public_server_invalidate(view->server);
}

void leme_public_server_view_urgent(struct leme_view *view) {
  if (view == NULL || view->server == NULL || !view->mapped ||
      view->unmanaged || !view->public_meta.ever_mapped ||
      view->public_meta.urgent ||
      !leme_public_model_available(view->server->public_model))
    return;
  if (leme_public_model_next_order(view->server->public_model,
                                   &view->public_meta.urgent_since) !=
      LEME_PUBLIC_OK)
    return;
  view->public_meta.urgent = true;
  leme_public_server_invalidate(view->server);
}

static enum leme_public_status
capabilities_value(struct leme_public_builder *b,
                   const struct leme_server *server,
                   struct leme_public_value **out) {
  *out = NULL;
  const struct leme_public_features facts = leme_public_server_features(server);
  const bool known = !facts.effects_build || facts.effects_runtime_known;
  struct leme_public_value *capabilities = NULL, *effects = NULL;
  if (leme_public_object(b, 7, &capabilities) != LEME_PUBLIC_OK ||
      leme_public_object(b, 3, &effects) != LEME_PUBLIC_OK ||
      leme_public_put_bool(b, effects, LEME_PUBLIC_TEXT("build"),
                           facts.effects_build) != LEME_PUBLIC_OK ||
      leme_public_put_bool(b, effects, LEME_PUBLIC_TEXT("known"), known) !=
          LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  const enum leme_public_status status =
      known ? leme_public_put_bool(b, effects, LEME_PUBLIC_TEXT("effective"),
                                   facts.effects_build && facts.effects_runtime)
            : leme_public_put_null(b, effects, LEME_PUBLIC_TEXT("effective"));
  if (status != LEME_PUBLIC_OK ||
      leme_public_object_set(b, capabilities, LEME_PUBLIC_TEXT("effects"),
                             effects) != LEME_PUBLIC_OK ||
      leme_public_put_bool(b, capabilities, LEME_PUBLIC_TEXT("public_model"),
                           true) != LEME_PUBLIC_OK ||
      leme_public_put_bool(b, capabilities, LEME_PUBLIC_TEXT("json_control"),
                           facts.json_control) != LEME_PUBLIC_OK ||
      leme_public_put_bool(b, capabilities, LEME_PUBLIC_TEXT("query"),
                           facts.query) != LEME_PUBLIC_OK ||
      leme_public_put_bool(b, capabilities, LEME_PUBLIC_TEXT("actions"),
                           facts.actions) != LEME_PUBLIC_OK ||
      leme_public_put_bool(b, capabilities, LEME_PUBLIC_TEXT("config_writes"),
                           facts.config_writes) != LEME_PUBLIC_OK ||
      leme_public_put_bool(b, capabilities, LEME_PUBLIC_TEXT("watch"),
                           facts.watch) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  *out = capabilities;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status
limits_value(struct leme_public_builder *b,
             const struct leme_public_model *model,
             struct leme_public_value **out) {
  *out = NULL;
  if (model->snapshot_limit > (uint64_t)LEME_PUBLIC_SAFE_INTEGER ||
      model->total_limit > (uint64_t)LEME_PUBLIC_SAFE_INTEGER)
    return leme_public_fail(b, LEME_PUBLIC_LIMIT);
  struct leme_public_value *limits = NULL;
  if (leme_public_object(b, 4, &limits) != LEME_PUBLIC_OK ||
      leme_public_put_int(b, limits, LEME_PUBLIC_TEXT("snapshot_bytes"),
                          (int64_t)model->snapshot_limit) != LEME_PUBLIC_OK ||
      leme_public_put_int(b, limits, LEME_PUBLIC_TEXT("total_bytes"),
                          (int64_t)model->total_limit) != LEME_PUBLIC_OK ||
      leme_public_put_int(b, limits, LEME_PUBLIC_TEXT("value_depth"),
                          LEME_PUBLIC_MAX_DEPTH) != LEME_PUBLIC_OK ||
      leme_public_put_int(b, limits, LEME_PUBLIC_TEXT("field_depth"),
                          LEME_PUBLIC_MAX_FIELD_DEPTH) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  *out = limits;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status status_value(struct leme_public_builder *b,
                                            const struct leme_server *server,
                                            struct leme_public_value **out) {
  *out = NULL;
  struct leme_public_value *status = NULL, *capabilities = NULL;
  if (leme_public_object(b, 4, &status) != LEME_PUBLIC_OK ||
      capabilities_value(b, server, &capabilities) != LEME_PUBLIC_OK ||
      leme_public_put_bool(b, status, LEME_PUBLIC_TEXT("locked"),
                           leme_session_locked(server)) != LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, status, LEME_PUBLIC_TEXT("version"),
                           LEME_PUBLIC_BUILD_VERSION) != LEME_PUBLIC_OK ||
      leme_public_put_int(b, status, LEME_PUBLIC_TEXT("api_version"),
                          LEME_PUBLIC_API_VERSION) != LEME_PUBLIC_OK ||
      leme_public_object_set(b, status, LEME_PUBLIC_TEXT("capabilities"),
                             capabilities) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  *out = status;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status
runtime_value(struct leme_public_builder *b, const struct leme_server *server,
              const struct leme_public_model *model,
              struct leme_public_value **out) {
  *out = NULL;
  const struct leme_public_features facts = leme_public_server_features(server);
  struct leme_public_value *runtime = NULL, *capabilities = NULL,
                           *limits = NULL, *schema = NULL;
  if (leme_public_object(b, 5, &runtime) != LEME_PUBLIC_OK ||
      capabilities_value(b, server, &capabilities) != LEME_PUBLIC_OK ||
      limits_value(b, model, &limits) != LEME_PUBLIC_OK ||
      leme_public_schema_value(b, &facts, NULL, 0, &schema) != LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, runtime, LEME_PUBLIC_TEXT("version"),
                           LEME_PUBLIC_BUILD_VERSION) != LEME_PUBLIC_OK ||
      leme_public_put_int(b, runtime, LEME_PUBLIC_TEXT("api_version"),
                          LEME_PUBLIC_API_VERSION) != LEME_PUBLIC_OK ||
      leme_public_object_set(b, runtime, LEME_PUBLIC_TEXT("capabilities"),
                             capabilities) != LEME_PUBLIC_OK ||
      leme_public_object_set(b, runtime, LEME_PUBLIC_TEXT("limits"), limits) !=
          LEME_PUBLIC_OK ||
      leme_public_object_set(b, runtime, LEME_PUBLIC_TEXT("schema"), schema) !=
          LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  *out = runtime;
  return LEME_PUBLIC_OK;
}

static bool source_locked(void *context) {
  const struct leme_server *server = context;
  return leme_session_locked(server);
}

static enum leme_public_status session_value(struct leme_public_builder *b,
                                             const struct leme_server *server,
                                             struct leme_public_value **out) {
  if (leme_session_locked(server))
    return leme_public_fail(b, LEME_PUBLIC_LOCKED);
  struct leme_public_value *record = NULL, *focused_view = NULL,
                           *focused_output = NULL, *layout = NULL;
  const struct leme_view *view =
      server->focused_layer == NULL ? server->focused_view : NULL;
  const bool managed = view != NULL && view->mapped && !view->unmanaged &&
                       view->public_meta.ever_mapped;
  const struct leme_output *output = server->focused_output;
  const struct leme_config *config = server->config;
  const char *active = NULL, *variant = NULL;
  if (config != NULL && config->keyboard_layouts != NULL &&
      server->keyboard_layout < config->keyboard_layout_count) {
    active = config->keyboard_layouts[server->keyboard_layout].name;
    variant = config->keyboard_layouts[server->keyboard_layout].variant;
  }
  if (leme_public_object(b, 6, &record) != LEME_PUBLIC_OK ||
      (managed
           ? leme_public_ref_value(b, server->public_model, LEME_PUBLIC_VIEW,
                                   view->public_meta.id, 0, &focused_view)
           : leme_public_null(b, &focused_view)) != LEME_PUBLIC_OK ||
      (output != NULL
           ? leme_public_ref_value(b, server->public_model, LEME_PUBLIC_OUTPUT,
                                   output->public_id, 0, &focused_output)
           : leme_public_null(b, &focused_output)) != LEME_PUBLIC_OK ||
      leme_input_public_layout(b, config, active, variant, &layout) !=
          LEME_PUBLIC_OK ||
      leme_public_put_bool(b, record, LEME_PUBLIC_TEXT("locked"), false) !=
          LEME_PUBLIC_OK ||
      (server->session != NULL
           ? leme_public_put_bool(b, record, LEME_PUBLIC_TEXT("active"),
                                  server->session->active)
           : leme_public_put_null(b, record, LEME_PUBLIC_TEXT("active"))) !=
          LEME_PUBLIC_OK ||
      leme_public_put_cstr(b, record, LEME_PUBLIC_TEXT("mode"),
                           server->active_mode == NULL
                               ? "common"
                               : server->active_mode->name) != LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("focused_view"),
                             focused_view) != LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("focused_output"),
                             focused_output) != LEME_PUBLIC_OK ||
      leme_public_object_set(b, record, LEME_PUBLIC_TEXT("keyboard_layout"),
                             layout) != LEME_PUBLIC_OK)
    return leme_public_builder_status(b);
  *out = record;
  return LEME_PUBLIC_OK;
}

static enum leme_public_status
source_root(void *context, const struct leme_public_model *model,
            struct leme_public_builder *b, enum leme_public_root root,
            struct leme_public_value **out) {
  *out = NULL;
  const struct leme_server *server = context;
  switch (root) {
  case LEME_PUBLIC_STATUS:
    return status_value(b, server, out);
  case LEME_PUBLIC_RUNTIME:
    return runtime_value(b, server, model, out);
  case LEME_PUBLIC_VIEWS:
    return leme_views_public_capture(b, server, out);
  case LEME_PUBLIC_TAGS:
    return leme_tags_public_capture(b, server, out);
  case LEME_PUBLIC_OUTPUTS:
    return leme_outputs_public_capture(b, server, out);
  case LEME_PUBLIC_INPUTS:
    return leme_inputs_public_capture(b, server, out);
  case LEME_PUBLIC_CONFIG:
    return leme_config_public_capture(b, server, out);
  case LEME_PUBLIC_SESSION:
    return session_value(b, server, out);
  case LEME_PUBLIC_ROOT_COUNT:
    return LEME_PUBLIC_INVALID;
  }
  return LEME_PUBLIC_INVALID;
}

enum leme_public_status leme_public_server_capture_work(
    struct leme_server *server, uint32_t requested_roots,
    const struct leme_public_work *work, struct leme_public_snapshot **out) {
  if (out == NULL)
    return LEME_PUBLIC_INVALID;
  *out = NULL;
  if (server == NULL)
    return LEME_PUBLIC_INVALID;
  const struct leme_public_source source = leme_public_server_source(server);
  return leme_public_model_capture_work(server->public_model, &source,
                                        requested_roots, work, out);
}

enum leme_public_status
leme_public_server_capture(struct leme_server *server, uint32_t requested_roots,
                           struct leme_public_snapshot **out) {
  return leme_public_server_capture_work(server, requested_roots, NULL, out);
}

static uint64_t source_generation(void *context, enum leme_public_root root) {
  const struct leme_server *server = context;
  const struct leme_public_features f = leme_public_server_features(server);
  const uint64_t features =
      (uint64_t)f.effects_build | ((uint64_t)f.effects_runtime_known << 1) |
      ((uint64_t)f.effects_runtime << 2) | ((uint64_t)f.json_control << 3) |
      ((uint64_t)f.query << 4) | ((uint64_t)f.actions << 5) |
      ((uint64_t)f.config_writes << 6) | ((uint64_t)f.watch << 7);
  if (root == LEME_PUBLIC_RUNTIME)
    return UINT64_C(256) | features;
  if (root == LEME_PUBLIC_CONFIG && server->config != NULL &&
      server->public_config_generation != 0)
    return (server->public_config_generation << 8) | features;
  return 0;
}

struct leme_public_source
leme_public_server_source(struct leme_server *server) {
  return (struct leme_public_source){
      .context = server,
      .cache_snapshots = true,
      .root_generation = source_generation,
      .locked = source_locked,
      .root = source_root,
  };
}
