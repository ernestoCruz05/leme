#ifndef LEME_SESSION_ENVIRONMENT_H
#define LEME_SESSION_ENVIRONMENT_H

struct leme_server;

void leme_session_environment_publish(struct leme_server *server);
void leme_session_environment_cursor(const struct leme_server *server);

#endif
