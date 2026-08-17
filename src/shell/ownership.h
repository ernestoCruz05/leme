#ifndef LEME_OWNERSHIP_H
#define LEME_OWNERSHIP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct leme_output;
struct leme_tag;
struct leme_tags;
struct leme_view;

/* Reservado para os marcos que definirem restauração concreta. */
struct leme_view_restore;

enum leme_view_owner_kind {
    LEME_VIEW_OWNER_NONE,
    LEME_VIEW_OWNER_TAG,
    LEME_VIEW_OWNER_DURABLE,
};

enum leme_durable_reason {
    LEME_DURABLE_SCRATCHPAD,
    LEME_DURABLE_STICKY,
};

enum leme_durable_presentation {
    LEME_DURABLE_HIDDEN,
    LEME_DURABLE_OUTPUT,
};

struct leme_durable_owner {
    enum leme_durable_reason reason;
    enum leme_durable_presentation presentation;
    struct leme_output *output;
    struct leme_view_restore *restore;
};

struct leme_view_owner {
    enum leme_view_owner_kind kind;
    union {
        struct leme_tag *tag;
        struct leme_durable_owner durable;
    } value;
};

struct leme_ownership_transition;

void leme_ownership_init(struct leme_view *view);
enum leme_view_owner_kind leme_ownership_kind(const struct leme_view *view);
struct leme_tag *leme_ownership_tag(const struct leme_view *view);
bool leme_ownership_is_durable(const struct leme_view *view,
    enum leme_durable_reason reason);
const struct leme_durable_owner *leme_ownership_durable(
    const struct leme_view *view);
struct leme_output *leme_ownership_effective_output(
    const struct leme_view *view);
const struct leme_view_restore *leme_ownership_restore(
    const struct leme_view *view);

bool leme_ownership_scene_visible(const struct leme_view *view);
bool leme_ownership_hit_test_eligible(const struct leme_view *view);
bool leme_ownership_focus_eligible(const struct leme_view *view);
bool leme_ownership_activation_eligible(const struct leme_view *view);
bool leme_ownership_publication_eligible(const struct leme_view *view);
bool leme_ownership_direct_capture_eligible(const struct leme_view *view);

bool leme_ownership_prepare_none_to_durable(struct leme_view *view,
    enum leme_durable_reason reason,
    enum leme_durable_presentation presentation,
    struct leme_output *output,
    struct leme_ownership_transition **transition);
bool leme_ownership_prepare_tag_to_durable(struct leme_view *view,
    enum leme_durable_reason reason,
    enum leme_durable_presentation presentation,
    struct leme_output *output,
    struct leme_ownership_transition **transition);
bool leme_ownership_prepare_durable_group_to_tag(
    struct leme_view *const *views, size_t view_count,
    struct leme_view *focused, struct leme_tags *tags, uint16_t tag_id,
    struct leme_ownership_transition **transition);
bool leme_ownership_prepare_durable_group_to_durable(
    struct leme_view *const *views, size_t view_count,
    enum leme_durable_presentation presentation,
    struct leme_output *output,
    struct leme_ownership_transition **transition);
void leme_ownership_commit(struct leme_ownership_transition **transition);
void leme_ownership_discard(struct leme_ownership_transition **transition);
bool leme_ownership_present_durable(struct leme_view *view,
    enum leme_durable_presentation presentation,
    struct leme_output *output);
void leme_ownership_clear(struct leme_view *view);
void leme_ownership_set_unmanaged_output(struct leme_view *view,
    struct leme_output *output);

#endif
