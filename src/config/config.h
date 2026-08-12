#ifndef LEME_CONFIG_H
#define LEME_CONFIG_H

#include "config/diagnostics.h"
#include "input/input.h"
#include "output/placement.h"
#include "render/animation.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct leme_environment {
    char *name;
    char *value;
};

struct leme_startup {
    char **argv;
};

struct leme_scratchpad_config {
    char *name;
    char *identity;
    char **spawn;
    double width;
    double height;
    int line;
};

struct leme_keyboard_layout {
    char *name;
    char *variant;
};

enum leme_output_transform {
    LEME_OUTPUT_TRANSFORM_NORMAL,
    LEME_OUTPUT_TRANSFORM_90,
    LEME_OUTPUT_TRANSFORM_180,
    LEME_OUTPUT_TRANSFORM_270,
};

struct leme_output_config {
    char *name;
    int width;
    int height;
    int refresh_mhz;
    float scale;
    enum leme_output_transform transform;
    int x;
    int y;
    char *relative_to;
    enum leme_output_relation relation;
    bool configured;
    bool has_mode;
    bool has_refresh;
    bool has_position;
};

struct leme_output_policy {
    bool cross_output_focus;
    bool cross_output_move;
    bool cross_output_drag;
    bool warp_cursor;
};

enum leme_tag_field {
    LEME_TAG_FIELD_LAYOUT = 1 << 0,
    LEME_TAG_FIELD_DROP_MODE = 1 << 1,
    LEME_TAG_FIELD_MFACT = 1 << 2,
    LEME_TAG_FIELD_NMASTER = 1 << 3,
    LEME_TAG_FIELD_GAP = 1 << 4,
    LEME_TAG_FIELD_SPLIT_RATIO = 1 << 5,
    LEME_TAG_FIELD_COLLAPSE_WIDTH = 1 << 6,
};

struct leme_tag_settings {
    enum leme_layout_kind layout;
    enum leme_drop_mode drop_mode;
    double mfact;
    uint16_t nmaster;
    int gap;
    double split_ratio;
    int collapse_width;
    bool has_gap;
};

struct leme_tag_rule {
    uint16_t *ids;
    size_t id_count;
    uint32_t fields;
    struct leme_tag_settings settings;
};

enum leme_window_rule_field {
    LEME_WINDOW_RULE_TAG = 1 << 0,
    LEME_WINDOW_RULE_FLOATING = 1 << 1,
    LEME_WINDOW_RULE_FULLSCREEN = 1 << 2,
    LEME_WINDOW_RULE_OUTPUT = 1 << 3,
    LEME_WINDOW_RULE_OPACITY = 1 << 4,
};

struct leme_window_rule {
    char *identity;
    char *title;
    uint32_t fields;
    uint16_t tag_id;
    bool floating;
    bool fullscreen;
    char *output;
    double opacity;
};

enum leme_pointer_accel_profile {
    LEME_POINTER_ACCEL_ADAPTIVE,
    LEME_POINTER_ACCEL_FLAT,
};

enum leme_pointer_field {
    LEME_POINTER_PROFILE = 1 << 0,
    LEME_POINTER_SPEED = 1 << 1,
    LEME_POINTER_NATURAL_SCROLL = 1 << 2,
    LEME_POINTER_LEFT_HANDED = 1 << 3,
    LEME_POINTER_TAP = 1 << 4,
};

struct leme_pointer_settings {
    uint32_t fields;
    enum leme_pointer_accel_profile profile;
    double speed;
    bool natural_scroll;
    bool left_handed;
    bool tap;
};

struct leme_pointer_rule {
    char *name;
    struct leme_pointer_settings settings;
};

enum leme_activation_policy {
    LEME_ACTIVATION_FOLLOW,
    LEME_ACTIVATION_URGENT,
    LEME_ACTIVATION_IGNORE,
};

struct leme_publication_config {
    enum leme_activation_policy activation;
};

enum leme_banner_position {
    LEME_BANNER_TOP,
    LEME_BANNER_BOTTOM,
};

struct leme_config_errors {
    bool show;
    enum leme_banner_position position;
    int timeout;
};

enum leme_workspace_animation_style {
    LEME_WORKSPACE_ANIMATION_FULL_SLIDE,
    LEME_WORKSPACE_ANIMATION_GLIDE_FADE,
};

struct leme_workspace_animation_settings {
    bool configured;
    uint32_t duration_ms;
    enum leme_workspace_animation_style style;
    double distance;
    struct leme_animation_curve curve;
    struct leme_animation_curve opacity_curve;
};

struct leme_config {
    uint16_t initial_tags;
    uint16_t max_tags;
    enum leme_drop_mode drop_mode;
    int gap;
    int border_width;
    int corner_radius;
    int blur;
    float border_active[4];
    float border_inactive[4];
    double opacity_active;
    double opacity_inactive;
    struct leme_animation_settings animation[LEME_ANIMATION_EVENT_COUNT];
    struct leme_workspace_animation_settings workspace_animation;
    struct leme_tag_settings tag_defaults;
    struct leme_tag_rule *tag_rules;
    size_t tag_rule_count;
    struct leme_window_rule *window_rules;
    size_t window_rule_count;
    struct leme_keyboard_layout *keyboard_layouts;
    size_t keyboard_layout_count;
    struct leme_output_config *outputs;
    size_t output_count;
    struct leme_output_policy output_policy;
    struct leme_publication_config publication;
    struct leme_pointer_settings pointer_defaults;
    struct leme_pointer_rule *pointer_rules;
    size_t pointer_rule_count;
    struct leme_environment *environment;
    size_t environment_count;
    struct leme_startup *startup;
    size_t startup_count;
    struct leme_scratchpad_config *scratchpads;
    size_t scratchpad_count;
    struct leme_mode *modes;
    size_t mode_count;
    struct leme_config_errors config_errors;
    struct leme_diagnostics diagnostics;
    char *path;
};

struct leme_server;

struct leme_config *leme_config_defaults(void);
struct leme_config *leme_config_load(const char *path, char **error);
int leme_config_check(const char *path, FILE *stream);
bool leme_config_validate(const struct leme_config *config, char **error);
bool leme_config_apply(struct leme_server *server,
    struct leme_config *next, char **error);
void leme_config_destroy(struct leme_config *config);
void leme_config_tag_settings(const struct leme_config *config,
    uint16_t id, struct leme_tag_settings *settings);
const struct leme_scratchpad_config *leme_config_scratchpad(
    const struct leme_config *config, const char *name);
const char *leme_config_path(void);
void leme_config_launch_startup(struct leme_server *server);

#endif
