#ifndef LEME_INPUT_INTERNAL_H
#define LEME_INPUT_INTERNAL_H

struct leme_server;
struct wlr_input_device;

void leme_input_update_capabilities(struct leme_server *server);
void leme_input_keyboard_add(struct leme_server *server,
    struct wlr_input_device *device);
void leme_input_keyboards_finish(struct leme_server *server);
void leme_input_pointer_add(struct leme_server *server,
    struct wlr_input_device *device);
void leme_input_pointer_events_init(struct leme_server *server);
void leme_input_pointers_finish(struct leme_server *server);

#endif
