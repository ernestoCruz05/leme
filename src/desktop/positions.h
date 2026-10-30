#ifndef LEME_DESK_POSITIONS_H
#define LEME_DESK_POSITIONS_H

#include <stdbool.h>
#include <stdint.h>

struct leme_desk_positions;

char *leme_desk_positions_path(void);
struct leme_desk_positions *leme_desk_positions_load(const char *path,
                                                     const char *directory);
void leme_desk_positions_destroy(struct leme_desk_positions *positions);
bool leme_desk_positions_get(const struct leme_desk_positions *positions,
                             const char *name, uint32_t *column, uint32_t *row);
bool leme_desk_positions_set(struct leme_desk_positions *positions,
                             const char *name, uint32_t column, uint32_t row);
void leme_desk_positions_forget(struct leme_desk_positions *positions,
                                const char *name);
bool leme_desk_positions_rename(struct leme_desk_positions *positions,
                                const char *from, const char *to);
bool leme_desk_positions_save(const struct leme_desk_positions *positions,
                              const char *path);

#endif
