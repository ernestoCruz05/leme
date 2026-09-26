#ifndef LEME_INPUT_PUBLIC_H
#define LEME_INPUT_PUBLIC_H

#include "public/identity.h"
#include "public/value.h"

struct leme_server;
struct leme_config;
struct leme_public_input_info {
  struct leme_public_id id;
  const char *name;
  const char *seat;
  bool keyboard;
  bool libinput;
  bool has_ids;
  uint32_t vendor;
  uint32_t product;
  bool has_accel;
  bool adaptive;
  bool flat;
  const char *accel_profile;
  double accel_speed;
  bool has_natural_scroll;
  bool natural_scroll;
  bool has_left_handed;
  bool left_handed;
  bool has_tap;
  bool tap;
  const char *keyboard_active;
  const char *keyboard_variant;
};
typedef enum leme_public_status (*leme_public_input_visitor)(
    void *context, const struct leme_public_input_info *info);
void leme_input_public_keymap_committed(struct leme_server *server);
enum leme_public_status
leme_input_public_keyboards(const struct leme_server *server,
                            leme_public_input_visitor visit, void *context);
enum leme_public_status
leme_input_public_pointers(const struct leme_server *server,
                           leme_public_input_visitor visit, void *context);
enum leme_public_status leme_input_public_record(
    struct leme_public_builder *b, const struct leme_server *server,
    const struct leme_public_input_info *info, struct leme_public_value **out);
enum leme_public_status
leme_inputs_public_capture(struct leme_public_builder *b,
                           const struct leme_server *server,
                           struct leme_public_value **out);
enum leme_public_status
leme_input_public_layout(struct leme_public_builder *b,
                         const struct leme_config *config, const char *active,
                         const char *variant, struct leme_public_value **out);

#endif
