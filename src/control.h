#pragma once

#include "server.h"

typedef struct control_t control_t;

/* Local, same-user JSON control socket. Requests and replies are one
   newline-terminated JSON object, bounded to 64 KiB. */
control_t *control_start(settings_t *settings, server_t *server, GError **error);
void control_poll(control_t *control);
void control_stop(control_t *control);
const char *control_path(const control_t *control);
