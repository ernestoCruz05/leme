#ifndef TIMAO_LANGUAGE_INTERNAL_H
#define TIMAO_LANGUAGE_INTERNAL_H

#include "timao/heap.h"
#include "timao/host.h"
#include "timao/value.h"
#include "timao/environment.h"
#include "timao/parser.h"

struct timao_value {
  struct timao_heap_object *allocation;
  enum timao_value_kind kind;
  union {
    bool boolean;
    double number;
    struct leme_public_text text;
    struct {
      size_t count;
      const struct timao_value **items;
    } array;
    struct {
      size_t count;
      struct timao_member *members;
    } object;
    struct {
      struct timao_program *program;
      struct timao_environment *captured;
      uint32_t definition;
      size_t arity;
    } function;
    uint64_t token;
  } as;
};

enum timao_status timao_array_builder(struct timao_vm *vm, size_t count,
                                      struct timao_value **out,
                                      struct timao_diagnostic *error);

enum timao_status timao_object_builder(struct timao_vm *vm,
                                       const struct timao_member *members,
                                       size_t count, struct timao_value **out,
                                       struct timao_diagnostic *error);

struct timao_execution {
  struct timao_vm *vm;
  enum timao_context context;
  struct timao_environment *environment;
  size_t call_depth;
};

struct timao_pin;
struct timao_vm {
  struct timao_heap heap;
  struct timao_limits limits;
  struct timao_host host;
  struct timao_meter meter;
  struct timao_pin *pins;
  uint64_t next_pin;
  const struct timao_value *args;
  struct timao_heap_root args_root;
  struct timao_environment *globals;
  struct timao_heap_root globals_root;
  struct timao_heap_root result_root;
  struct timao_program *file_program;
  size_t file_next;
  bool active;
};

#endif
