#include "timao/request.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static yyjson_mut_val *call(yyjson_mut_doc *doc, const char *name,
                            yyjson_mut_val **args) {
  yyjson_mut_val *node = yyjson_mut_obj(doc);
  *args = yyjson_mut_arr(doc);
  if (!yyjson_mut_obj_add_strcpy(doc, node, "call", name) ||
      !yyjson_mut_obj_add_val(doc, node, "args", *args))
    return NULL;
  return node;
}

static yyjson_mut_val *literal(yyjson_mut_doc *doc, yyjson_mut_val *value) {
  yyjson_mut_val *node = yyjson_mut_obj(doc);
  if (!yyjson_mut_obj_add_val(doc, node, "literal", value))
    return NULL;
  return node;
}

static yyjson_mut_val *path(yyjson_mut_doc *doc, int count, char **words) {
  yyjson_mut_val *args = NULL;
  yyjson_mut_val *node = call(doc, words[0], &args);
  for (int i = 1; node != NULL && i < count; ++i) {
    yyjson_mut_val *outer = call(doc, "get", &args);
    if (!yyjson_mut_arr_append(args, node) ||
        !yyjson_mut_arr_append(args,
                               literal(doc, yyjson_mut_strcpy(doc, words[i]))))
      return NULL;
    node = outer;
  }
  return node;
}

static yyjson_mut_val *command(yyjson_mut_doc *doc, const char *name, int count,
                               char **words) {
  yyjson_mut_val *strings = yyjson_mut_arr(doc);
  for (int i = 0; i < count; ++i)
    if (!yyjson_mut_arr_add_strcpy(doc, strings, words[i]))
      return NULL;
  yyjson_mut_val *args = NULL;
  yyjson_mut_val *node = call(doc, "command", &args);
  if (node == NULL ||
      !yyjson_mut_arr_append(args,
                             literal(doc, yyjson_mut_strcpy(doc, name))) ||
      !yyjson_mut_arr_append(args, literal(doc, strings)))
    return NULL;
  return node;
}

static bool source_form(const char *text) {
  while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r')
    ++text;
  return *text == '(';
}

static bool refuse(struct timao_request *out, const char **usage,
                   const char *message) {
  yyjson_mut_doc_free(out->body);
  out->body = NULL;
  *usage = message;
  return false;
}

bool timao_request_parse(int argc, char **argv, struct timao_request *out,
                         const char **usage) {
  *out = (struct timao_request){0};
  *usage = NULL;
  int i = 1;
  for (; i < argc && strncmp(argv[i], "--", 2) == 0; ++i) {
    if (strcmp(argv[i], "--help") == 0) {
      out->action = TIMAO_ACTION_HELP;
      return true;
    }
    if (strcmp(argv[i], "--version") == 0) {
      out->action = TIMAO_ACTION_VERSION;
      return true;
    }
    if (strcmp(argv[i], "--json") == 0)
      out->format = TIMAO_FORMAT_JSON;
    else if (strcmp(argv[i], "--raw") == 0)
      out->format = TIMAO_FORMAT_RAW;
    else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc)
      out->socket = argv[++i];
    else if (strcmp(argv[i], "--socket") == 0)
      return refuse(out, usage, "--socket needs a path");
    else
      return refuse(out, usage, "unknown option");
  }
  if (i == argc)
    return refuse(out, usage, "missing command");
  const char *form = argv[i];
  char **words = argv + i + 1;
  const int count = argc - i - 1;

  out->body = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *root = yyjson_mut_obj(out->body);
  if (root == NULL || !yyjson_mut_obj_add_int(out->body, root, "version", 1))
    return refuse(out, usage, "out of memory");
  yyjson_mut_doc_set_root(out->body, root);

  const char *op = "act";
  const char *source = NULL;
  yyjson_mut_val *expr = NULL;
  if (strcmp(form, "query") == 0 || strcmp(form, "act") == 0) {
    if (count != 1)
      return refuse(out, usage, "query and act take one expression");
    op = form;
    source = words[0];
  } else if (strcmp(form, "watch") == 0) {
    op = form;
    out->watch = true;
    if (count == 0)
      return refuse(out, usage, "watch needs an expression or a root");
    if (source_form(words[0]) && count != 1)
      return refuse(out, usage, "watch takes one expression");
    if (source_form(words[0]))
      source = words[0];
    else
      expr = path(out->body, count, words);
  } else if (strcmp(form, "get") == 0) {
    if (count == 0)
      return refuse(out, usage, "get needs a root");
    op = "query";
    expr = path(out->body, count, words);
  } else
    expr = command(out->body, form, count, words);

  const bool built =
      yyjson_mut_obj_add_strcpy(out->body, root, "op", op) &&
      (source != NULL
           ? yyjson_mut_obj_add_strcpy(out->body, root, "source", source)
           : yyjson_mut_obj_add_val(out->body, root, "expr", expr));
  if (!built)
    return refuse(out, usage, "out of memory");
  return true;
}

char *timao_request_line(struct timao_request *request, const char *id,
                         const char *instance, size_t *length) {
  *length = 0;
  yyjson_mut_doc *doc = request->body;
  yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
  if (!yyjson_mut_obj_put(root, yyjson_mut_str(doc, "id"),
                          yyjson_mut_strcpy(doc, id)) ||
      !yyjson_mut_obj_put(root, yyjson_mut_str(doc, "instance"),
                          yyjson_mut_strcpy(doc, instance)))
    return NULL;
  size_t written = 0;
  char *text = yyjson_mut_write(doc, YYJSON_WRITE_NOFLAG, &written);
  if (text == NULL || written > SIZE_MAX - 2) {
    free(text);
    return NULL;
  }
  char *line = realloc(text, written + 2);
  if (line == NULL) {
    free(text);
    return NULL;
  }
  line[written] = '\n';
  line[written + 1] = '\0';
  *length = written + 1;
  return line;
}

void timao_request_finish(struct timao_request *request) {
  yyjson_mut_doc_free(request->body);
  request->body = NULL;
}
