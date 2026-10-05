#ifndef LEME_OUTPUT_HOME_H
#define LEME_OUTPUT_HOME_H

#include <stdbool.h>

struct leme_output;
struct leme_server;
struct leme_view;

void leme_output_home_init(struct leme_server *server);
void leme_output_home_finish(struct leme_server *server);
bool leme_output_home_tracked(const struct leme_output *output);
void leme_output_home_evacuate(struct leme_output *from,
                               struct leme_output *to);
bool leme_output_home_restore(struct leme_output *output, const char *name);
void leme_output_home_forget_view(struct leme_view *view);

#endif
