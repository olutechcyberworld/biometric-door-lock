#pragma once
/* Wi-Fi station + WiFiManager-style provisioning.
 *
 *  - Credentials live in NVS (namespace "wifi_cfg"), never in source or sdkconfig.
 *  - Stored credentials -> join that network (retry forever with capped backoff), advertise <hostname>.local.
 *  - No credentials, or button held DOORLOCK_PROV_HOLD_MS -> SoftAP "DoorLock-XXXX" (WPA2) + captive portal where the
 *    user picks a network and types the password. Portal closes after DOORLOCK_WIFI_PROV_TIMEOUT_S or on save.
 *  - Red LED (via relay_control): slow blink = offline / connecting, fast blink = provisioning, off = online.
 *    A lockout (solid red) or a deny (2 flashes) always wins over the blink.
 * Serial lines for the dashboard:  WIFI,state=online,ip=...   WIFI,state=offline,reason=N   WIFI,state=provisioning,... */
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_MGR_OFFLINE = 0,   /* no credentials stored and not provisioning */
    WIFI_MGR_CONNECTING,    /* credentials stored, not (yet) associated / no IP */
    WIFI_MGR_ONLINE,
    WIFI_MGR_PROVISIONING,
} wifi_mgr_state_t;

/* Called with online=true (and the IP) after the station gets an address, online=false when the link is lost or the
 * setup portal opens. Runs on the event-loop / caller task: keep it short. */
typedef void (*wifi_manager_link_cb_t)(bool online, const char *ip);
void wifi_manager_set_link_cb(wifi_manager_link_cb_t cb);

void wifi_manager_start(void);               /* call once, after relay_control_init() */
void wifi_manager_enter_provisioning(void);  /* safe to call from any task; no-op if already provisioning */
wifi_mgr_state_t wifi_manager_state(void);
bool wifi_manager_get_ip(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
