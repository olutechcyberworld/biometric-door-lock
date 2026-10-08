#include "device_link.h"

#include <stdio.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "wifi_manager.h"

#define FW_TAG "rev2-dev"

static const char *TAG = "dev_link";
static httpd_handle_t s_srv;
static bool s_svc_added;
static TaskHandle_t s_beacon;
static char s_id[13];
#define MAX_REGISTRARS 4
static device_link_register_fn s_registrars[MAX_REGISTRARS];
static int s_registrar_n;

static device_link_close_fn s_close_fn;

httpd_handle_t device_link_server(void)
{
    return s_srv;
}

void device_link_set_close_fn(device_link_close_fn fn)
{
    s_close_fn = fn;
}

/* A custom close_fn replaces the server's own, so it must close the socket itself. */
static void on_sock_close(httpd_handle_t hd, int sockfd)
{
    (void)hd;
    if (s_close_fn) {
        s_close_fn(sockfd);
    }
    close(sockfd);
}

static esp_err_t whoami_get(httpd_req_t *req)
{
    char ip[16] = "";
    wifi_manager_get_ip(ip, sizeof(ip));
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"device\":\"doorlock\",\"id\":\"%s\",\"fw\":\"%s\",\"ip\":\"%s\"}", s_id, FW_TAG, ip);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static void server_up(void)
{
    if (s_srv) {
        return;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 12; /* room for /challenge /auth /override /session next */
    cfg.lru_purge_enable = true;
    cfg.close_fn = on_sock_close;
    if (httpd_start(&s_srv, &cfg) != ESP_OK) {
        s_srv = NULL;
        ESP_LOGE(TAG, "httpd_start failed");
        return;
    }
    static const httpd_uri_t u_whoami = {.uri = "/whoami", .method = HTTP_GET, .handler = whoami_get};
    httpd_register_uri_handler(s_srv, &u_whoami);
    for (int i = 0; i < s_registrar_n; i++) {
        s_registrars[i](s_srv);
    }
    printf("LINK,server=up,registrars=%d\n", s_registrar_n);
}

void device_link_add_registrar(device_link_register_fn fn)
{
    if (!fn || s_registrar_n >= MAX_REGISTRARS) {
        return;
    }
    s_registrars[s_registrar_n++] = fn;
    if (s_srv) {
        fn(s_srv);
    }
}

static void server_down(void)
{
    if (s_srv) {
        httpd_stop(s_srv); /* frees port 80 for the provisioning portal */
        s_srv = NULL;
        printf("LINK,server=down\n");
    }
}

static void on_link(bool online, const char *ip)
{
    (void)ip;
    if (!online) {
        server_down();
        return;
    }
    server_up();
    if (!s_svc_added) {
        mdns_txt_item_t txt[] = {{"id", s_id}, {"fw", FW_TAG}};
        if (mdns_service_add("Door lock", "_doorlock", "_tcp", 80, txt, 2) == ESP_OK) {
            s_svc_added = true;
        }
    }
    if (s_beacon) {
        xTaskNotifyGive(s_beacon); /* announce right away; IP may have changed with the network */
    }
}

static void beacon_task(void *arg)
{
    (void)arg;
    int s = -1;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(3000));
        char ip[16];
        if (!wifi_manager_get_ip(ip, sizeof(ip))) {
            if (s >= 0) {
                close(s);
                s = -1;
            }
            continue;
        }
        if (s < 0) {
            s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (s < 0) {
                continue;
            }
            int one = 1;
            setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
        }
        esp_netif_ip_info_t info;
        esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (!n || esp_netif_get_ip_info(n, &info) != ESP_OK) {
            continue;
        }
        char msg[128];
        int len = snprintf(msg, sizeof(msg), "{\"t\":\"doorlock\",\"id\":\"%s\",\"ip\":\"%s\",\"port\":80}", s_id, ip);
        struct sockaddr_in to = {0};
        to.sin_family = AF_INET;
        to.sin_port = htons(DEVICE_LINK_BEACON_PORT);
        to.sin_addr.s_addr = (info.ip.addr & info.netmask.addr) | ~info.netmask.addr; /* directed broadcast */
        sendto(s, msg, len, 0, (struct sockaddr *)&to, sizeof(to));
        to.sin_addr.s_addr = 0xFFFFFFFFu; /* limited broadcast: some phones only answer one of the two */
        sendto(s, msg, len, 0, (struct sockaddr *)&to, sizeof(to));
    }
}

void device_link_start(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_id, sizeof(s_id), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    xTaskCreate(beacon_task, "beacon", 3072, NULL, 3, &s_beacon);
    wifi_manager_set_link_cb(on_link);
}
