#ifndef LEME_CONFIG_OPTION_H
#define LEME_CONFIG_OPTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct leme_config;
struct leme_reject_extra;
struct leme_scfg_directive;

enum leme_option_kind {
  LEME_OPTION_BOOL,
  LEME_OPTION_INT,
  LEME_OPTION_U16,
  LEME_OPTION_U32,
  LEME_OPTION_DECIMAL,
  LEME_OPTION_CHOICE,
  LEME_OPTION_COLOR,
  LEME_OPTION_STRING,
  LEME_OPTION_CUSTOM,
};

enum leme_option_flag {
  LEME_OPTION_MIN_OPEN = 1 << 0,
  LEME_OPTION_MAX_OPEN = 1 << 1,
  LEME_OPTION_REPEAT = 1 << 2,
  LEME_OPTION_MULTI = 1 << 3,
};

enum leme_option_block_flag {
  LEME_OPTION_BLOCK_FIRST_WINS = 1 << 0,
};

enum leme_option_effect {
  LEME_OPTION_EFFECT_NONE,
  LEME_OPTION_EFFECT_STORE,
  LEME_OPTION_EFFECT_LAYOUT,
  LEME_OPTION_EFFECT_ARRANGE,
  LEME_OPTION_EFFECT_REDRAW,
  LEME_OPTION_EFFECT_EFFECTS,
  LEME_OPTION_EFFECT_FULLSCREEN,
  LEME_OPTION_EFFECT_POINTERS,
  LEME_OPTION_EFFECT_SWIPE,
  LEME_OPTION_EFFECT_FINGERS,
  LEME_OPTION_EFFECT_CURSOR,
};

union leme_option_value {
  bool boolean;
  int64_t integer;
  double decimal;
  unsigned choice;
  float color[4];
  char *string;
};

enum leme_option_result {
  LEME_OPTION_ACCEPTED,
  LEME_OPTION_REJECTED,
  LEME_OPTION_FATAL,
};

struct leme_option;
struct leme_option_block;

struct leme_option_parse {
  struct leme_config *config;
  const struct leme_option_block *block;
  const struct leme_option *option;
  const struct leme_scfg_directive *entry;
  void *target;
  void *data;
};

struct leme_option {
  const char *name;
  enum leme_option_kind kind;
  size_t offset;
  double min;
  double max;
  uint32_t flags;
  const char *const *choices;
  size_t choice_count;
  const char *unit;
  uint32_t field;
  enum leme_option_result (*parse)(const struct leme_option_parse *parse);
  enum leme_option_effect effect;
};

struct leme_option_block {
  const char *name;
  const struct leme_option *options;
  size_t count;
  uint32_t flags;
};

#define LEME_OPTION_BLOCK_MAX 32
#define LEME_OPTION_COUNT(array) (sizeof(array) / sizeof(*(array)))
#define LEME_OPTION_CHOICES(array)                                             \
  .choices = (array), .choice_count = LEME_OPTION_COUNT(array)

enum leme_option_result
leme_option_reject(const struct leme_option_parse *parse, int param,
                   const struct leme_reject_extra *extra, const char *format,
                   ...);
enum leme_option_result
leme_option_parse_block(struct leme_config *config,
                        const struct leme_scfg_directive *directive,
                        const struct leme_option_block *block, void *target,
                        uint32_t *fields, void *data);

bool leme_option_in_range(const struct leme_option *option,
                          const union leme_option_value *value);
bool leme_option_equal(const struct leme_option *option, const void *target,
                       const union leme_option_value *value);
void leme_option_store(const struct leme_option *option, void *target,
                       union leme_option_value *value);
bool leme_option_block_valid(const struct leme_option_block *block,
                             const void *target, uint32_t fields);

#endif
