#include "wifi_manager.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"
#include "dhcpserver/dhcpserver.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "relay_control.h"

#if CONFIG_DOORLOCK_AUTH_AUTO
#define RED(m) relay_control_red_status(m)
#else
#define RED(m) ((void)0)
#endif

#define NVS_NS        "wifi_cfg"
#define HOSTNAME      CONFIG_DOORLOCK_WIFI_HOSTNAME
#define PORTAL_IP     "192.168.4.1"
#define MAX_SCAN      20
#define RETRY_MAX_MS  30000

static const char *TAG = "wifi_mgr";

static SemaphoreHandle_t s_lock;
static volatile wifi_mgr_state_t s_state = WIFI_MGR_OFFLINE;
static volatile bool s_want_sta;
static bool s_inited, s_wifi_started, s_mdns_up;
static char s_ip[16];
static uint32_t s_retry_ms = 1000;
static esp_netif_t *s_sta_netif, *s_ap_netif;
static esp_timer_handle_t s_retry_timer, s_timeout_timer, s_finish_timer;
static httpd_handle_t s_httpd;
static TaskHandle_t s_dns_task;
static volatile bool s_dns_run;
static wifi_manager_link_cb_t s_link_cb;

void wifi_manager_set_link_cb(wifi_manager_link_cb_t cb)
{
    s_link_cb = cb;
}

static void link_notify(bool online, const char *ip)
{
    if (s_link_cb) {
        s_link_cb(online, ip);
    }
}

static struct {
    char ssid[33];
    int8_t rssi;
} s_scan[MAX_SCAN];
static int s_scan_n;

/* ---------- credentials (NVS) ---------- */

static bool creds_load(char *ssid, size_t ssz, char *pass, size_t psz)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t l = ssz;
    bool ok = (nvs_get_str(h, "ssid", ssid, &l) == ESP_OK) && ssid[0];
    if (ok) {
        l = psz;
        if (nvs_get_str(h, "pass", pass, &l) != ESP_OK) {
            pass[0] = 0;
        }
    }
    nvs_close(h);
    return ok;
}

static esp_err_t creds_save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) {
        return e;
    }
    e = nvs_set_str(h, "ssid", ssid);
    if (e == ESP_OK) {
        e = nvs_set_str(h, "pass", pass);
    }
    if (e == ESP_OK) {
        e = nvs_commit(h);
    }
    nvs_close(h);
    return e;
}

/* ---------- station ---------- */

/* Applies the stored credentials and starts connecting. ESP_ERR_NOT_FOUND if nothing is stored. */
static esp_err_t sta_connect_stored(void)
{
    char ssid[33] = {0}, pass[65] = {0};
    if (!creds_load(ssid, sizeof(ssid), pass, sizeof(pass))) {
        return ESP_ERR_NOT_FOUND;
    }
    wifi_config_t c = {0};
    strlcpy((char *)c.sta.ssid, ssid, sizeof(c.sta.ssid));
    strlcpy((char *)c.sta.password, pass, sizeof(c.sta.password));
    c.sta.pmf_cfg.capable = true;
    c.sta.pmf_cfg.required = false;

    s_want_sta = false; /* so the disconnect we trigger below doesn't schedule a retry */
    if (s_wifi_started) {
        esp_wifi_disconnect();
    }
    if (esp_timer_is_active(s_retry_timer)) {
        esp_timer_stop(s_retry_timer);
    }
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &c);
    s_retry_ms = 1000;
    s_state = WIFI_MGR_CONNECTING;
    s_want_sta = true;
    RED(RED_STATUS_OFFLINE);
    printf("WIFI,state=connecting,ssid=%s\n", ssid);
    if (!s_wifi_started) {
        esp_wifi_start(); /* WIFI_EVENT_STA_START handler issues the connect */
        s_wifi_started = true;
    } else {
        esp_wifi_connect();
    }
    return ESP_OK;
}

static void retry_cb(void *arg)
{
    (void)arg;
    if (s_want_sta) {
        esp_wifi_connect();
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            if (s_want_sta) {
                esp_wifi_connect();
            }
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            if (!s_want_sta) {
                break;
            }
            const wifi_event_sta_disconnected_t *d = (const wifi_event_sta_disconnected_t *)data;
            bool was_online = (s_state == WIFI_MGR_ONLINE);
            s_ip[0] = 0;
            s_state = WIFI_MGR_CONNECTING;
            RED(RED_STATUS_OFFLINE);
            if (was_online) {
                link_notify(false, NULL);
            }
            printf("WIFI,state=offline,reason=%d,retry_ms=%u\n", d->reason, (unsigned)s_retry_ms);
            if (esp_timer_is_active(s_retry_timer)) {
                esp_timer_stop(s_retry_timer);
            }
            esp_timer_start_once(s_retry_timer, (uint64_t)s_retry_ms * 1000ULL);
            s_retry_ms = (s_retry_ms * 2 > RETRY_MAX_MS) ? RETRY_MAX_MS : s_retry_ms * 2;
            break;
        }
        case WIFI_EVENT_AP_STACONNECTED:
            printf("WIFI,prov=client_joined\n");
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_state = WIFI_MGR_ONLINE;
        s_retry_ms = 1000;
        RED(RED_STATUS_OFF);
        if (!s_mdns_up && mdns_init() == ESP_OK) {
            mdns_hostname_set(HOSTNAME);
            mdns_instance_name_set("Door lock");
            s_mdns_up = true;
        }
#if CONFIG_DOORLOCK_WIFI_PS_NONE
        esp_wifi_set_ps(WIFI_PS_NONE); /* modem-sleep makes mDNS/broadcast/HTTP flaky; the lock is mains powered */
#endif
        printf("WIFI,state=online,ip=%s,host=%s.local\n", s_ip, HOSTNAME);
        link_notify(true, s_ip);
    }
}

/* ---------- captive-portal DNS: answers every A query with the portal IP ---------- */

static void dns_task(void *arg)
{
    (void)arg;
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s >= 0) {
        struct sockaddr_in a = {0};
        a.sin_family = AF_INET;
        a.sin_port = htons(53);
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        if (bind(s, (struct sockaddr *)&a, sizeof(a)) == 0) {
            uint8_t buf[300];
            static const uint8_t ans[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 168, 4, 1};
            while (s_dns_run) {
                struct sockaddr_in from;
                socklen_t fl = sizeof(from);
                int n = recvfrom(s, buf, sizeof(buf) - sizeof(ans), 0, (struct sockaddr *)&from, &fl);
                if (n < 17) {
                    continue;
                }
                int i = 12;
                while (i < n && buf[i]) {
                    i += buf[i] + 1;
                }
                i += 5; /* terminating zero + QTYPE(2) + QCLASS(2) */
                if (i > n) {
                    continue;
                }
                int qtype = (buf[i - 4] << 8) | buf[i - 3];
                buf[2] = 0x81;
                buf[3] = 0x80;
                buf[4] = 0; buf[5] = 1; /* QDCOUNT */
                buf[8] = buf[9] = buf[10] = buf[11] = 0;
                int out = i;
                if (qtype == 1) {
                    buf[6] = 0; buf[7] = 1;
                    memcpy(buf + i, ans, sizeof(ans));
                    out = i + (int)sizeof(ans);
                } else {
                    buf[6] = 0; buf[7] = 0; /* e.g. AAAA: empty answer so the phone falls back to A */
                }
                sendto(s, buf, out, 0, (struct sockaddr *)&from, fl);
            }
        } else {
            ESP_LOGW(TAG, "DNS bind failed (portal still reachable at http://" PORTAL_IP ")");
        }
        close(s);
    }
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

static void dns_start(void)
{
    s_dns_run = true;
    xTaskCreate(dns_task, "prov_dns", 3072, NULL, 4, &s_dns_task);
}

static void dns_stop(void)
{
    s_dns_run = false;
    for (int i = 0; i < 40 && s_dns_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ---------- scan cache ---------- */

static void scan_refresh(void)
{
    wifi_scan_config_t sc = {0};
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        ESP_LOGW(TAG, "scan failed");
        return;
    }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    uint16_t cap = n > 30 ? 30 : n;
    wifi_ap_record_t *r = calloc(cap ? cap : 1, sizeof(*r));
    if (!r) {
        esp_wifi_clear_ap_list();
        return;
    }
    esp_wifi_scan_get_ap_records(&cap, r); /* also frees the driver's internal list */
    s_scan_n = 0;
    for (int i = 0; i < cap && s_scan_n < MAX_SCAN; i++) {
        const char *ssid = (const char *)r[i].ssid;
        if (!ssid[0]) {
            continue;
        }
        bool dup = false;
        for (int j = 0; j < s_scan_n; j++) {
            if (strcmp(s_scan[j].ssid, ssid) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        strlcpy(s_scan[s_scan_n].ssid, ssid, sizeof(s_scan[0].ssid));
        s_scan[s_scan_n].rssi = r[i].rssi;
        s_scan_n++;
    }
    free(r);
}

/* ---------- portal HTTP ---------- */

static const char PAGE[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\"><title>Door lock Wi-Fi</title>"
    "<style>body{font-family:sans-serif;max-width:420px;margin:2em auto;padding:0 1em}"
    "input,select,button{width:100%;box-sizing:border-box;padding:.7em;margin:.4em 0;font-size:1em}</style></head>"
    "<body><h2>Door lock Wi-Fi setup</h2>"
    "<form method=POST action=/save>"
    "<select id=n onchange=\"document.getElementById('s').value=this.value\"><option value=\"\">Scanning...</option></select>"
    "<input id=s name=ssid placeholder=\"Network name (SSID)\" maxlength=32 required>"
    "<input name=pass type=password placeholder=\"Password (leave empty if open)\" maxlength=63>"
    "<button>Save and connect</button></form>"
    "<button type=button onclick=\"scan(1)\">Rescan</button>"
    "<script>function scan(r){var n=document.getElementById('n');n.innerHTML='<option>Scanning...</option>';"
    "fetch(r?'/scan?refresh=1':'/scan').then(function(x){return x.json()}).then(function(l){"
    "n.innerHTML='<option value=\"\">Select a network</option>';"
    "l.forEach(function(x){var o=document.createElement('option');o.value=x.ssid;"
    "o.textContent=x.ssid+' ('+x.rssi+' dBm)';n.appendChild(o)})}).catch(function(){"
    "n.innerHTML='<option value=\"\">Scan failed - type the name below</option>'})}scan(0)</script>"
    "</body></html>";

static const char PAGE_SAVED[] =
    "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>Saved</title></head><body style=\"font-family:sans-serif;max-width:420px;margin:2em auto;padding:0 1em\">"
    "<h2>Saved</h2><p>The door lock is now joining your network and this setup network will close.</p>"
    "<p>Red LED off = connected. Red LED still blinking = wrong name or password (or out of range): "
    "hold the button for 5 seconds and try again.</p></body></html>";

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t scan_get(httpd_req_t *req)
{
    if (httpd_req_get_url_query_len(req) > 0) {
        scan_refresh();
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");
    for (int i = 0; i < s_scan_n; i++) {
        char buf[140];
        size_t p = (size_t)snprintf(buf, sizeof(buf), "%s{\"ssid\":\"", i ? "," : "");
        for (const char *c = s_scan[i].ssid; *c && p < sizeof(buf) - 40; c++) {
            if (*c == '"' || *c == '\\') {
                buf[p++] = '\\';
                buf[p++] = *c;
            } else if ((unsigned char)*c >= 0x20) {
                buf[p++] = *c;
            }
        }
        snprintf(buf + p, sizeof(buf) - p, "\",\"rssi\":%d}", s_scan[i].rssi);
        httpd_resp_sendstr_chunk(req, buf);
    }
    httpd_resp_sendstr_chunk(req, "]");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static void url_decode(char *s)
{
    char *o = s;
    while (*s) {
        if (*s == '+') {
            *o++ = ' ';
            s++;
        } else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char h[3] = {s[1], s[2], 0};
            *o++ = (char)strtol(h, NULL, 16);
            s += 3;
        } else {
            *o++ = *s++;
        }
    }
    *o = 0;
}

static void finish_provisioning(void);

static void finish_cb(void *arg)
{
    (void)arg;
    finish_provisioning();
}

static esp_err_t save_post(httpd_req_t *req)
{
    char body[420];
    int len = req->content_len;
    if (len <= 0 || len >= (int)sizeof(body)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
    }
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, body + got, len - got);
        if (r <= 0) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv");
        }
        got += r;
    }
    body[len] = 0;

    char ssid[128] = {0}, pass[200] = {0};
    if (httpd_query_key_value(body, "ssid", ssid, sizeof(ssid)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid");
    }
    httpd_query_key_value(body, "pass", pass, sizeof(pass)); /* absent/empty = open network */
    url_decode(ssid);
    url_decode(pass);
    size_t sl = strlen(ssid), pl = strlen(pass);
    if (sl < 1 || sl > 32 || (pl != 0 && (pl < 8 || pl > 63))) {
        httpd_resp_set_type(req, "text/html");
        return httpd_resp_send(req,
            "<p style=\"font-family:sans-serif\">Name must be 1-32 characters; password empty or 8-63 characters. "
            "<a href=/>Back</a></p>", HTTPD_RESP_USE_STRLEN);
    }
    if (creds_save(ssid, pass) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs");
    }
    printf("WIFI,prov=saved,ssid=%s\n", ssid);
    httpd_resp_set_type(req, "text/html");
    esp_err_t e = httpd_resp_send(req, PAGE_SAVED, HTTPD_RESP_USE_STRLEN);
    esp_timer_start_once(s_finish_timer, 1500 * 1000ULL); /* let the page reach the phone before the AP drops */
    return e;
}

static esp_err_t redirect_404(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" PORTAL_IP "/");
    return httpd_resp_send(req, "Redirecting", HTTPD_RESP_USE_STRLEN);
}

static void portal_start(void)
{
    /* Offer the SoftAP's own IP as DNS so the phone's captive-portal check lands on us. */
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(s_ap_netif, &ip_info) == ESP_OK) {
        esp_netif_dns_info_t dns = {0};
        dns.ip.u_addr.ip4.addr = ip_info.ip.addr;
        dns.ip.type = IPADDR_TYPE_V4;
        dhcps_offer_t offer = OFFER_DNS;
        esp_netif_dhcps_stop(s_ap_netif);
        esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof(offer));
        esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns);
        esp_netif_dhcps_start(s_ap_netif);
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 6;
    cfg.lru_purge_enable = true;
    if (httpd_start(&s_httpd, &cfg) == ESP_OK) {
        static const httpd_uri_t u_root = {.uri = "/", .method = HTTP_GET, .handler = root_get};
        static const httpd_uri_t u_scan = {.uri = "/scan", .method = HTTP_GET, .handler = scan_get};
        static const httpd_uri_t u_save = {.uri = "/save", .method = HTTP_POST, .handler = save_post};
        httpd_register_uri_handler(s_httpd, &u_root);
        httpd_register_uri_handler(s_httpd, &u_scan);
        httpd_register_uri_handler(s_httpd, &u_save);
        httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, redirect_404);
    } else {
        ESP_LOGE(TAG, "portal httpd_start failed");
    }
    dns_start();
}

static void portal_stop(void)
{
    dns_stop();
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
}

/* ---------- provisioning mode ---------- */

static void timeout_cb(void *arg)
{
    (void)arg;
    printf("WIFI,prov=timeout\n");
    finish_provisioning();
}

static void finish_provisioning(void)
{
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    if (s_state == WIFI_MGR_PROVISIONING) {
        if (esp_timer_is_active(s_timeout_timer)) {
            esp_timer_stop(s_timeout_timer);
        }
        portal_stop();
        if (sta_connect_stored() != ESP_OK) { /* nothing stored (timed out on first setup) */
            esp_wifi_set_mode(WIFI_MODE_STA);
            s_state = WIFI_MGR_OFFLINE;
            RED(RED_STATUS_OFFLINE);
            printf("WIFI,state=offline,reason=no_credentials\n");
        }
    }
    xSemaphoreGiveRecursive(s_lock);
}

void wifi_manager_enter_provisioning(void)
{
    if (!s_inited) {
        return;
    }
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    if (s_state == WIFI_MGR_PROVISIONING) {
        esp_timer_stop(s_timeout_timer); /* restart the window */
        esp_timer_start_once(s_timeout_timer, (uint64_t)CONFIG_DOORLOCK_WIFI_PROV_TIMEOUT_S * 1000000ULL);
        xSemaphoreGiveRecursive(s_lock);
        return;
    }
    s_want_sta = false;
    if (esp_timer_is_active(s_retry_timer)) {
        esp_timer_stop(s_retry_timer);
    }
    if (s_wifi_started) {
        esp_wifi_disconnect();
    }
    s_ip[0] = 0;
    link_notify(false, NULL); /* frees port 80 for the portal */

    wifi_config_t ap = {0};
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "DoorLock-%02X%02X", mac[4], mac[5]);
    ap.ap.ssid_len = (uint8_t)strlen((char *)ap.ap.ssid);
    strlcpy((char *)ap.ap.password, CONFIG_DOORLOCK_WIFI_AP_PASS, sizeof(ap.ap.password));
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.channel = 1;
    ap.ap.max_connection = 2;

    esp_wifi_set_mode(WIFI_MODE_APSTA); /* STA half is only for scanning */
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (!s_wifi_started) {
        esp_wifi_start();
        s_wifi_started = true;
    }
    vTaskDelay(pdMS_TO_TICKS(300)); /* let the disconnect settle before scanning */
    scan_refresh();
    portal_start();

    s_state = WIFI_MGR_PROVISIONING;
    RED(RED_STATUS_PROVISIONING);
    esp_timer_start_once(s_timeout_timer, (uint64_t)CONFIG_DOORLOCK_WIFI_PROV_TIMEOUT_S * 1000000ULL);
    printf("WIFI,state=provisioning,ap=%s,portal=http://" PORTAL_IP ",timeout_s=%d\n", (char *)ap.ap.ssid,
           CONFIG_DOORLOCK_WIFI_PROV_TIMEOUT_S);
    xSemaphoreGiveRecursive(s_lock);
}

/* ---------- public ---------- */

wifi_mgr_state_t wifi_manager_state(void)
{
    return s_state;
}

bool wifi_manager_get_ip(char *buf, size_t len)
{
    if (s_state != WIFI_MGR_ONLINE || !s_ip[0] || !buf || !len) {
        return false;
    }
    strlcpy(buf, s_ip, len);
    return true;
}

void wifi_manager_start(void)
{
    if (s_inited) {
        return;
    }
    s_lock = xSemaphoreCreateRecursiveMutex();

    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        e = nvs_flash_init();
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "nvs init failed: %s", esp_err_to_name(e));
        return;
    }
    esp_netif_init();
    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) { /* INVALID_STATE = something else already created it */
        ESP_LOGE(TAG, "event loop: %s", esp_err_to_name(e));
        return;
    }
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta_netif, HOSTNAME);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed");
        return;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM); /* our own NVS namespace is the only credential store */
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);

    const esp_timer_create_args_t t1 = {.callback = retry_cb, .name = "wifi_retry"};
    const esp_timer_create_args_t t2 = {.callback = timeout_cb, .name = "prov_timeout"};
    const esp_timer_create_args_t t3 = {.callback = finish_cb, .name = "prov_finish"};
    esp_timer_create(&t1, &s_retry_timer);
    esp_timer_create(&t2, &s_timeout_timer);
    esp_timer_create(&t3, &s_finish_timer);
    s_inited = true;

    if (sta_connect_stored() == ESP_OK) {
        return;
    }
    s_state = WIFI_MGR_OFFLINE;
    RED(RED_STATUS_OFFLINE);
    printf("WIFI,state=offline,reason=no_credentials\n");
#if CONFIG_DOORLOCK_WIFI_AUTO_PROV
    wifi_manager_enter_provisioning();
#endif
}
