#ifndef TIMAO_TRANSPORT_H
#define TIMAO_TRANSPORT_H

#include "timao/client.h"
#include "timao/endpoint.h"

struct timao_transport;

enum leme_public_status timao_transport_create(
    struct leme_public_budget *account, const struct timao_endpoint *endpoint,
    struct timao_client *client, struct timao_transport **out);
void timao_transport_destroy(struct timao_transport *transport);
int timao_transport_fd(const struct timao_transport *transport);
short timao_transport_events(struct timao_transport *transport);
void timao_transport_step(struct timao_transport *transport, short revents);

#endif
