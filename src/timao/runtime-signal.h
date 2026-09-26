#ifndef TIMAO_RUNTIME_SIGNAL_H
#define TIMAO_RUNTIME_SIGNAL_H
#include "public/budget.h"
#include <signal.h>
struct timao_runtime_signal;
enum leme_public_status
timao_runtime_signal_create(struct leme_public_budget *account,
                            struct timao_runtime_signal **out);
int timao_runtime_signal_destroy(struct timao_runtime_signal *signals);
int timao_runtime_signal_mask(const struct timao_runtime_signal *signals,
                              sigset_t *out);
int timao_runtime_signal_fd(const struct timao_runtime_signal *signals);
int timao_runtime_signal_read(struct timao_runtime_signal *signals);
void timao_runtime_signal_clear_interrupt(struct timao_runtime_signal *signals);
#endif
