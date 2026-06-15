#include <errno.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"

#ifndef CONFIG_WIFI_RMT_STATIC_RX_BUFFER_NUM
#define CONFIG_WIFI_RMT_STATIC_RX_BUFFER_NUM 10
#endif
#ifndef CONFIG_WIFI_RMT_DYNAMIC_RX_BUFFER_NUM
#define CONFIG_WIFI_RMT_DYNAMIC_RX_BUFFER_NUM 32
#endif
#ifndef CONFIG_WIFI_RMT_TX_BUFFER_TYPE
#define CONFIG_WIFI_RMT_TX_BUFFER_TYPE 1
#endif
#ifndef CONFIG_WIFI_RMT_DYNAMIC_RX_MGMT_BUF
#define CONFIG_WIFI_RMT_DYNAMIC_RX_MGMT_BUF 0
#endif
#ifndef CONFIG_WIFI_RMT_ESPNOW_MAX_ENCRYPT_NUM
#define CONFIG_WIFI_RMT_ESPNOW_MAX_ENCRYPT_NUM 7
#endif

#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_eth_netif_glue.h"
#include "ethernet_init.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include "nvs_flash.h"
#include "usb_update_msc.h"

#if __has_include("local_wifi_config.h")
#include "local_wifi_config.h"
#endif

#define AP_WIFI_SSID "UC-PROXY-01"
#define AP_WIFI_PASS "ucproxy123"
#define AP_WIFI_CHANNEL 6
#define AP_WIFI_MAX_CONN 4

#ifndef STA_WIFI_SSID
#define STA_WIFI_SSID ""
#endif

#ifndef STA_WIFI_PASS
#define STA_WIFI_PASS ""
#endif

#ifndef STA_WIFI_NETWORKS
#define STA_WIFI_NETWORKS { { STA_WIFI_SSID, STA_WIFI_PASS } }
#endif

#define PROXY_LISTEN_PORT 2222
#define UC_SSH_HOST "192.168.1.100"
#define UC_SSH_PORT 22

#define ETH_IP "192.168.1.101"
#define ETH_GW "192.168.1.100"
#define ETH_NETMASK "255.255.255.0"

static const char *TAG = "uc_proxy";
#define MODE_NVS_NAMESPACE "ucprog"
#define MODE_NVS_KEY "usb_mode"

typedef struct {
    const char *ssid;
    const char *pass;
} wifi_credential_t;

static const wifi_credential_t s_wifi_networks[] = STA_WIFI_NETWORKS;
static const size_t s_wifi_network_count = sizeof(s_wifi_networks) / sizeof(s_wifi_networks[0]);
static size_t s_wifi_network_index = 0;
static int s_sta_retry_count = 0;
static uc_usb_mode_t s_usb_mode = UC_USB_MODE_OFF;

#define STA_WIFI_ENABLED (s_wifi_network_count > 0 && s_wifi_networks[0].ssid[0] != '\0')

static const wifi_credential_t *current_wifi_network(void) {
    return &s_wifi_networks[s_wifi_network_index];
}

static void configure_sta_network(void) {
    const wifi_credential_t *network = current_wifi_network();
    wifi_config_t sta_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strlcpy((char *)sta_config.sta.ssid, network->ssid, sizeof(sta_config.sta.ssid));
    strlcpy((char *)sta_config.sta.password, network->pass, sizeof(sta_config.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
}

static void select_next_wifi_network(void) {
    if (s_wifi_network_count <= 1) {
        return;
    }
    s_wifi_network_index = (s_wifi_network_index + 1) % s_wifi_network_count;
    configure_sta_network();
    ESP_LOGI(TAG, "Trying alternate site Wi-Fi '%s'", current_wifi_network()->ssid);
}

static void ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    (void)arg;
    (void)event_base;
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;

    if (event_id == IP_EVENT_STA_GOT_IP) {
        s_sta_retry_count = 0;
        ESP_LOGI(TAG, "Joined Wi-Fi '%s' with IP " IPSTR "; PuTTY target is " IPSTR ":%d",
                 current_wifi_network()->ssid,
                 IP2STR(&event->ip_info.ip),
                 IP2STR(&event->ip_info.ip),
                 PROXY_LISTEN_PORT);
    } else if (event_id == IP_EVENT_ETH_GOT_IP) {
        ESP_LOGI(TAG, "Ethernet IP " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    (void)arg;
    (void)event_base;
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "Wi-Fi client joined: " MACSTR, MAC2STR(event->mac));
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(TAG, "Wi-Fi client left: " MACSTR, MAC2STR(event->mac));
    } else if (event_id == WIFI_EVENT_STA_START && STA_WIFI_ENABLED) {
        ESP_LOGI(TAG, "Connecting to site Wi-Fi '%s'", current_wifi_network()->ssid);
        ESP_ERROR_CHECK(esp_wifi_connect());
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED && STA_WIFI_ENABLED) {
        s_sta_retry_count++;
        ESP_LOGW(TAG, "Site Wi-Fi disconnected; retry %d", s_sta_retry_count);
        if (s_sta_retry_count % 3 == 0) {
            select_next_wifi_network();
        }
        ESP_ERROR_CHECK(esp_wifi_connect());
    }
}

static void eth_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    (void)arg;
    (void)event_base;
    if (event_id == ETHERNET_EVENT_CONNECTED) {
        uint8_t mac[6];
        esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;
        esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac);
        ESP_LOGI(TAG, "Ethernet link up, MAC " MACSTR, MAC2STR(mac));
    } else if (event_id == ETHERNET_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "Ethernet link down");
    }
}

static void init_nvs(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(ret);
    }
}

static uc_usb_mode_t load_usb_mode(void) {
    nvs_handle_t nvs;
    uint8_t raw = UC_USB_MODE_OFF;
    esp_err_t ret = nvs_open(MODE_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret == ESP_OK) {
        nvs_get_u8(nvs, MODE_NVS_KEY, &raw);
        nvs_close(nvs);
    }
    if (raw == UC_USB_MODE_M0 || raw == UC_USB_MODE_M1) {
        return (uc_usb_mode_t)raw;
    }
    return UC_USB_MODE_OFF;
}

static esp_err_t save_usb_mode(uc_usb_mode_t mode) {
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(MODE_NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "open mode nvs");
    esp_err_t ret = nvs_set_u8(nvs, MODE_NVS_KEY, (uint8_t)mode);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

static void restart_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(750));
    esp_restart();
}

static esp_err_t send_text(httpd_req_t *req, const char *type, const char *body) {
    httpd_resp_set_type(req, type);
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t root_handler(httpd_req_t *req) {
    char body[1800];
    snprintf(body, sizeof(body),
             "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
             "<title>UC Proxy</title><style>"
             "body{font-family:Segoe UI,Arial,sans-serif;margin:24px;max-width:680px}"
             "button{font-size:18px;margin:8px 8px 8px 0;padding:12px 18px;border-radius:6px;border:1px solid #777}"
             ".danger{background:#7a1f1f;color:white}.safe{background:#1f6f43;color:white}"
             "code{background:#eee;padding:2px 5px;border-radius:4px}"
             "</style></head><body>"
             "<h1>UC Proxy</h1>"
             "<p>USB mode: <strong>%s</strong></p>"
             "<p>USB MSC active: <strong>%s</strong></p>"
             "<p>SSH proxy: <code>port %d</code>, UC target <code>%s:%d</code></p>"
             "<button class='safe' onclick=\"setMode('off')\">OFF</button>"
             "<button class='danger' onclick=\"setMode('m0')\">Expose M0</button>"
             "<button class='danger' onclick=\"setMode('m1')\">Expose M1</button>"
             "<script>"
             "async function setMode(m){"
             "let label=m.toUpperCase();"
             "if(!confirm(m==='off'?'Switch USB exposure OFF and reboot?':'Expose '+label+' over USB after reboot?'))return;"
             "let r=await fetch('/mode/'+m,{method:'POST'});"
             "alert(await r.text());"
             "}"
             "</script></body></html>",
             uc_usb_mode_name(s_usb_mode),
             uc_usb_update_active() ? "yes" : "no",
             PROXY_LISTEN_PORT,
             UC_SSH_HOST,
             UC_SSH_PORT);
    return send_text(req, "text/html", body);
}

static esp_err_t mode_status_handler(httpd_req_t *req) {
    char body[256];
    snprintf(body, sizeof(body),
             "{\"mode\":\"%s\",\"usb_active\":%s,\"ssh_port\":%d,\"uc_host\":\"%s\"}\n",
             uc_usb_mode_name(s_usb_mode),
             uc_usb_update_active() ? "true" : "false",
             PROXY_LISTEN_PORT,
             UC_SSH_HOST);
    return send_text(req, "application/json", body);
}

static esp_err_t set_mode_handler(httpd_req_t *req) {
    uc_usb_mode_t mode = UC_USB_MODE_OFF;
    if (strstr(req->uri, "/m0")) {
        mode = UC_USB_MODE_M0;
    } else if (strstr(req->uri, "/m1")) {
        mode = UC_USB_MODE_M1;
    }

    ESP_ERROR_CHECK(save_usb_mode(mode));
    char body[160];
    snprintf(body, sizeof(body), "Saved USB mode %s. Rebooting...\n", uc_usb_mode_name(mode));
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static void init_http_server(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_handler,
    };
    const httpd_uri_t mode_status = {
        .uri = "/mode",
        .method = HTTP_GET,
        .handler = mode_status_handler,
    };
    const httpd_uri_t mode_off = {
        .uri = "/mode/off",
        .method = HTTP_POST,
        .handler = set_mode_handler,
    };
    const httpd_uri_t mode_m0 = {
        .uri = "/mode/m0",
        .method = HTTP_POST,
        .handler = set_mode_handler,
    };
    const httpd_uri_t mode_m1 = {
        .uri = "/mode/m1",
        .method = HTTP_POST,
        .handler = set_mode_handler,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &mode_status));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &mode_off));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &mode_m0));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &mode_m1));
    ESP_LOGI(TAG, "HTTP mode control ready on /");
}

static void init_wifi(void) {
    ESP_ERROR_CHECK(esp_netif_create_default_wifi_ap() ? ESP_OK : ESP_FAIL);
    if (STA_WIFI_ENABLED) {
        ESP_ERROR_CHECK(esp_netif_create_default_wifi_sta() ? ESP_OK : ESP_FAIL);
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_handler, NULL));

    wifi_config_t ap_config = {
        .ap = {
            .ssid = AP_WIFI_SSID,
            .ssid_len = strlen(AP_WIFI_SSID),
            .password = AP_WIFI_PASS,
            .channel = AP_WIFI_CHANNEL,
            .max_connection = AP_WIFI_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .required = false,
            },
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(STA_WIFI_ENABLED ? WIFI_MODE_APSTA : WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

    if (STA_WIFI_ENABLED) {
        configure_sta_network();
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Fallback AP ready: SSID=%s password=%s IP=192.168.4.1", AP_WIFI_SSID, AP_WIFI_PASS);
    if (!STA_WIFI_ENABLED) {
        ESP_LOGW(TAG, "Site Wi-Fi is not configured; create src/local_wifi_config.h to enable STA mode");
    }
}

static void set_eth_static_ip(esp_netif_t *eth_netif) {
    esp_netif_ip_info_t ip_info;
    memset(&ip_info, 0, sizeof(ip_info));
    esp_err_t dhcp_stop = esp_netif_dhcpc_stop(eth_netif);
    if (dhcp_stop != ESP_OK && dhcp_stop != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_ERROR_CHECK(dhcp_stop);
    }
    ESP_ERROR_CHECK(ip4addr_aton(ETH_IP, (ip4_addr_t *)&ip_info.ip) ? ESP_OK : ESP_ERR_INVALID_ARG);
    ESP_ERROR_CHECK(ip4addr_aton(ETH_GW, (ip4_addr_t *)&ip_info.gw) ? ESP_OK : ESP_ERR_INVALID_ARG);
    ESP_ERROR_CHECK(ip4addr_aton(ETH_NETMASK, (ip4_addr_t *)&ip_info.netmask) ? ESP_OK : ESP_ERR_INVALID_ARG);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(eth_netif, &ip_info));
    ESP_LOGI(TAG, "Ethernet static IP=%s target UC=%s:%d", ETH_IP, UC_SSH_HOST, UC_SSH_PORT);
}

static void init_ethernet(void) {
    uint8_t eth_port_cnt = 0;
    esp_eth_handle_t *eth_handles = NULL;
    ESP_ERROR_CHECK(ethernet_init_all(&eth_handles, &eth_port_cnt));
    if (eth_port_cnt == 0) {
        ESP_LOGE(TAG, "No Ethernet ports detected");
        return;
    }
    if (eth_port_cnt > 1) {
        ESP_LOGW(TAG, "Multiple Ethernet ports detected; using first");
    }

    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&cfg);
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handles[0])));
    set_eth_static_ip(eth_netif);

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, ip_event_handler, NULL));
    ESP_ERROR_CHECK(esp_eth_start(eth_handles[0]));
}

static int connect_to_uc(void) {
    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port = htons(UC_SSH_PORT),
    };
    inet_pton(AF_INET, UC_SSH_HOST, &dest.sin_addr);

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed: errno=%d", errno);
        return -1;
    }

    ESP_LOGI(TAG, "Connecting to UC SSH at %s:%d", UC_SSH_HOST, UC_SSH_PORT);
    if (connect(sock, (struct sockaddr *)&dest, sizeof(dest)) != 0) {
        ESP_LOGE(TAG, "UC SSH connect failed: errno=%d", errno);
        close(sock);
        return -1;
    }
    ESP_LOGI(TAG, "UC SSH connected");
    return sock;
}

static void pipe_sockets(int a, int b) {
    uint8_t buf[1024];
    while (true) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(a, &rfds);
        FD_SET(b, &rfds);
        int maxfd = a > b ? a : b;
        int ready = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        if (ready <= 0) {
            break;
        }

        if (FD_ISSET(a, &rfds)) {
            int len = recv(a, buf, sizeof(buf), 0);
            if (len <= 0 || send(b, buf, len, 0) <= 0) break;
        }
        if (FD_ISSET(b, &rfds)) {
            int len = recv(b, buf, sizeof(buf), 0);
            if (len <= 0 || send(a, buf, len, 0) <= 0) break;
        }
    }
}

static void proxy_task(void *arg) {
    (void)arg;
    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "listen socket failed: errno=%d", errno);
        vTaskDelete(NULL);
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in listen_addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(PROXY_LISTEN_PORT),
    };
    if (bind(listen_sock, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) != 0) {
        ESP_LOGE(TAG, "bind failed: errno=%d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
    }
    if (listen(listen_sock, 2) != 0) {
        ESP_LOGE(TAG, "listen failed: errno=%d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "SSH proxy listening on port %d; AP address is 192.168.4.1", PROXY_LISTEN_PORT);

    while (true) {
        struct sockaddr_in source_addr;
        socklen_t addr_len = sizeof(source_addr);
        int client = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (client < 0) {
            ESP_LOGE(TAG, "accept failed: errno=%d", errno);
            continue;
        }

        ESP_LOGI(TAG, "PuTTY/client connected");
        int uc = connect_to_uc();
        if (uc >= 0) {
            pipe_sockets(client, uc);
            close(uc);
        }
        close(client);
        ESP_LOGI(TAG, "Proxy session closed");
    }
}

void app_main(void) {
    init_nvs();
    s_usb_mode = load_usb_mode();
    if (s_usb_mode == UC_USB_MODE_OFF) {
        ESP_LOGW(TAG, "SAFE OFF MODE: USB mass storage is disabled.");
    } else {
        ESP_LOGW(TAG, "USB update mode requested: %s", uc_usb_mode_name(s_usb_mode));
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    init_wifi();
    init_ethernet();
    init_http_server();
    esp_err_t usb_ret = uc_usb_update_start(s_usb_mode);
    if (usb_ret != ESP_OK) {
        ESP_LOGE(TAG, "USB update mode %s failed: %s; returning to OFF on next boot",
                 uc_usb_mode_name(s_usb_mode), esp_err_to_name(usb_ret));
        save_usb_mode(UC_USB_MODE_OFF);
        s_usb_mode = UC_USB_MODE_OFF;
    }
    xTaskCreate(proxy_task, "ssh_proxy", 8192, NULL, 5, NULL);
}
