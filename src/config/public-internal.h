#ifndef LEME_CONFIG_PUBLIC_INTERNAL_H
#define LEME_CONFIG_PUBLIC_INTERNAL_H

#include "config/public.h"
#include "config/config.h"

const char *leme_config_public_drop_mode(enum leme_drop_mode mode);
const char *leme_config_public_profile(enum leme_pointer_accel_profile profile);
enum leme_public_status leme_config_public_text(struct leme_public_builder *b,
                                                const char *text,
                                                struct leme_public_value **out);
enum leme_public_status
leme_config_public_redacted(struct leme_public_builder *b,
                            struct leme_public_value **out);
enum leme_public_status leme_config_public_merge(
    struct leme_public_builder *b, const struct leme_public_value *lhs,
    const struct leme_public_value *rhs, struct leme_public_value **out);
enum leme_public_status
leme_config_public_count(struct leme_public_builder *b,
                         struct leme_public_value *record,
                         struct leme_public_text key, size_t count);
enum leme_public_status
leme_config_public_scalars(struct leme_public_builder *b,
                           const struct leme_config *config,
                           struct leme_public_value **out);
enum leme_public_status
leme_config_public_modes(struct leme_public_builder *b,
                         const struct leme_config *config,
                         struct leme_public_value **out);
enum leme_public_status
leme_config_public_rules(struct leme_public_builder *b,
                         const struct leme_config *config,
                         struct leme_public_value **out);
enum leme_public_status
leme_config_public_animation(struct leme_public_builder *b,
                             const struct leme_config *config,
                             struct leme_public_value **out);
enum leme_public_status
leme_config_public_diagnostics(struct leme_public_builder *b,
                               const struct leme_config *config,
                               struct leme_public_value **out);

#endif
