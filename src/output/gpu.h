#ifndef LEME_OUTPUT_GPU_H
#define LEME_OUTPUT_GPU_H

#include <stdbool.h>
#include <stddef.h>

struct leme_server;

struct leme_gpu_candidate {
  const char *devnode;
  bool boot;
};

size_t leme_gpu_pick_primary(const struct leme_gpu_candidate *cards,
                             size_t count);
bool leme_gpu_is_card_name(const char *name);

bool leme_gpu_create_backend(struct leme_server *server);
void leme_gpu_apply_config(struct leme_server *server);
void leme_gpu_finish(struct leme_server *server);

#endif
