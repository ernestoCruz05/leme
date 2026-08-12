#ifndef LEME_SCRATCHPAD_H
#define LEME_SCRATCHPAD_H

#include "core/leme.h"

#include <stdbool.h>
#include <stddef.h>
#include <wayland-server-core.h>

struct leme_config;
struct leme_output;
struct leme_server;
struct leme_tags;
struct leme_view;

enum leme_scratchpad_state {
    LEME_SCRATCHPAD_NONE,
    LEME_SCRATCHPAD_HIDDEN,
    LEME_SCRATCHPAD_SHOWN,
};

enum leme_scratchpad_map_result {
    LEME_SCRATCHPAD_MAP_NO_MATCH,
    LEME_SCRATCHPAD_MAP_ADOPTED,
    LEME_SCRATCHPAD_MAP_FAILED,
};

struct leme_scratchpad_manager {
    struct leme_server *server;
    struct wl_list pool; /* struct leme_view::scratchpad_link, MRU first */
    struct wl_list pending; /* private owned named spawn requests */
    struct leme_view *shown;
    struct leme_output *shown_output;
    /*
     * Ganchos que um teste preenche para substituir o que toca no sistema:
     * lançar um processo, armar um temporizador, contar configurações. Em
     * produção ficam nulos e o compositor nunca lhes mexe.
     */
    bool commit_started;
    void (*commit_observer)(const struct leme_server *, void *);
    void *commit_observer_data;
    size_t *geometry_configure_count;
    size_t *render_configure_count;
    bool (*spawn_observer)(const struct leme_server *, char *const *, void *);
    void *spawn_observer_data;
    int (*timer_arm_observer)(unsigned int, void *);
    void *timer_arm_observer_data;
    bool (*direct_prepare_observer)(const struct leme_view *, void *);
    void *direct_prepare_observer_data;
};

/*
 * Consultas sobre os pedidos por atender. Vivem aqui porque a entrada da
 * lista só existe dentro de scratchpad.c.
 */
size_t leme_scratchpad_pending_count(const struct leme_server *server);
bool leme_scratchpad_pending_matches(const struct leme_server *server,
    const char *name, const char *identity);
struct leme_output *leme_scratchpad_pending_destination(
    const struct leme_server *server, const char *name);
bool leme_scratchpad_expire_pending(struct leme_server *server,
    const char *name);

bool leme_scratchpad_init(struct leme_server *server);
void leme_scratchpad_finish(struct leme_server *server);
bool leme_scratchpad_send(struct leme_server *server, struct leme_view *view);
bool leme_scratchpad_toggle_unnamed(struct leme_server *server,
    struct leme_output *output);
bool leme_scratchpad_toggle_named(struct leme_server *server,
    const char *name, struct leme_output *output);
bool leme_scratchpad_retrieve(struct leme_server *server,
    struct leme_tags *destination);
bool leme_view_is_scratchpad(const struct leme_view *view);
bool leme_view_is_shown_scratchpad(const struct leme_view *view);
/* Uma vista gerida e mapeada que pode estar exposta pelos protocolos. */
bool leme_view_protocol_eligible(const struct leme_view *view);
enum leme_scratchpad_map_result leme_scratchpad_try_adopt_map(
    struct leme_view *view);
void leme_scratchpad_handle_unmap(struct leme_view *view);
void leme_scratchpad_handle_identity_change(struct leme_view *view);
void leme_scratchpad_handle_output_destroy(struct leme_server *server,
    struct leme_output *output);
void leme_scratchpad_reconcile_outputs(struct leme_server *server);
void leme_scratchpad_handle_usable_area(struct leme_output *output);
bool leme_scratchpad_move_shown(struct leme_view *view,
    struct leme_output *output);
bool leme_scratchpad_apply_shown_box(struct leme_view *view,
    struct leme_box box, bool resizing);
void leme_scratchpad_reconcile_config(struct leme_server *server,
    const struct leme_config *previous, const struct leme_config *next);

#endif
