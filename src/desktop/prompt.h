#ifndef LEME_DESK_PROMPT_H
#define LEME_DESK_PROMPT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <xkbcommon/xkbcommon.h>

#define LEME_DESK_PROMPT_TEXT_MAX 1024

struct leme_server;
struct leme_output;

enum leme_desk_prompt_kind {
  LEME_DESK_PROMPT_CREATE,
  LEME_DESK_PROMPT_RENAME,
  LEME_DESK_PROMPT_MESSAGE,
};

struct leme_desk_prompt_text {
  char bytes[LEME_DESK_PROMPT_TEXT_MAX + 1];
  size_t cursor;
  bool select_all;
};

bool leme_desk_prompt_text_init(struct leme_desk_prompt_text *text,
                                const char *initial);
bool leme_desk_prompt_text_edit(struct leme_desk_prompt_text *text,
                                xkb_keysym_t keysym, uint32_t modifiers,
                                const char *utf8, size_t length);
bool leme_desk_prompt_open(struct leme_server *server,
                           struct leme_output *output,
                           enum leme_desk_prompt_kind kind, const char *initial,
                           void (*submit)(struct leme_server *server,
                                          const char *name));
void leme_desk_prompt_status(struct leme_server *server, const char *message);
void leme_desk_prompt_close(struct leme_server *server);
void leme_desk_prompt_finish(struct leme_server *server);
bool leme_desk_prompt_active(const struct leme_server *server);

#endif
