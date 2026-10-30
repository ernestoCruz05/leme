#ifndef LEME_DESK_CATALOG_H
#define LEME_DESK_CATALOG_H

#include <stdbool.h>
#include <stddef.h>

struct leme_desk_catalog;

struct leme_desk_entry {
  char *path;
  char *label;
  char *icon;
  bool directory;
};

char *leme_desk_catalog_directory(void);
char *leme_desk_catalog_launcher_icon(const char *path);
struct leme_desk_catalog *leme_desk_catalog_create(void);
void leme_desk_catalog_destroy(struct leme_desk_catalog *catalog);
bool leme_desk_catalog_scan(struct leme_desk_catalog *catalog,
                            const char *directory);
size_t leme_desk_catalog_count(const struct leme_desk_catalog *catalog);
const struct leme_desk_entry *
leme_desk_catalog_entry(const struct leme_desk_catalog *catalog, size_t index);

#endif
