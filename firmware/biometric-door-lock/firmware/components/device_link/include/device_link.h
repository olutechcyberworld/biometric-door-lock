#pragma once
/* Station-side presence: lets the owner app FIND the lock without knowing its IP.
 *
 *  1. HTTP server on port 80 while online, with an unauthenticated  GET /whoami
 *     -> {"device":"doorlock","id":"<12 hex mac>","fw":"...","ip":"a.b.c.d"}
 *  2. UDP broadcast beacon on DEVICE_LINK_BEACON_PORT every ~3 s while online (and immediately on join):
 *     {"t":"doorlock","id":"<12 hex mac>","ip":"a.b.c.d","port":80}
 *  3. mDNS service _doorlock._tcp (TXT id, fw) next to the <hostname>.local name.
 * Nothing here grants access: /whoami and the beacon only say "a lock is at this address". */
#define DEVICE_LINK_BEACON_PORT 4210

#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif
void device_link_start(void); /* call BEFORE wifi_manager_start() */

/* Other modules (PIN API, later the override/session endpoints) register their URI handlers here. The callback runs
 * every time the server (re)starts - it is stopped whenever the link drops - and immediately if it is already up. */
typedef void (*device_link_register_fn)(httpd_handle_t server);
void device_link_add_registrar(device_link_register_fn fn);

/* The running server, or NULL while the link is down. Long-lived users of a socket (the WebSocket session) compare
 * their saved handle with this before touching it, because the server is stopped and restarted with the Wi-Fi link. */
httpd_handle_t device_link_server(void);

/* Called with the socket number whenever the server closes a client socket (any client). */
typedef void (*device_link_close_fn)(int sockfd);
void device_link_set_close_fn(device_link_close_fn fn);
#ifdef __cplusplus
}
#endif
