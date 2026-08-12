#ifndef LEME_REQUEST_H
#define LEME_REQUEST_H

#include <stddef.h>

#define LEME_REQUEST_MAX_TOKENS 16

size_t leme_request_tokenize(char *line, char **tokens, size_t capacity);
size_t leme_request_fields(char *token, char **fields, size_t capacity);

#endif
