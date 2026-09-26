#ifndef TIMAO_LEXER_H
#define TIMAO_LEXER_H

#include "timao/source.h"

enum timao_token_kind {
  TIMAO_TOKEN_END,
  TIMAO_TOKEN_OPEN,
  TIMAO_TOKEN_CLOSE,
  TIMAO_TOKEN_NAME,
  TIMAO_TOKEN_FIELD,
  TIMAO_TOKEN_STRING,
  TIMAO_TOKEN_NUMBER,
  TIMAO_TOKEN_TRUE,
  TIMAO_TOKEN_FALSE,
  TIMAO_TOKEN_NULL
};
struct timao_token {
  enum timao_token_kind kind;
  struct timao_span span;
  struct leme_public_text text;
  double number;
  void *owned;
};
struct timao_lexer {
  struct timao_source *source;
  size_t offset;
  struct timao_diagnostic *error;
  bool allocation_failed;
};

enum timao_status timao_lexer_next(struct timao_lexer *lexer,
                                   struct timao_token *token,
                                   struct timao_diagnostic *error);
void timao_token_finish(struct timao_token *token);
bool timao_name_valid(struct leme_public_text text);

#endif
