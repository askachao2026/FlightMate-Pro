#include "flightmate_wifi.h"

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "lwip/ip4_addr.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "fm_wifi";
#define FM_WIFI_RETRY_DELAY_US (8 * 1000 * 1000)
#define FM_WIFI_MAX_RETRIES 3
#define FM_AP_STOP_DELAY_US (4 * 1000 * 1000)
#define FM_AP_STOP_AFTER_SAVE_US (1500 * 1000)
#define FM_PROVISION_SSID "FlightMate-Setup"
#define FM_PROVISION_PASSWORD "flightmate"
#define FM_FORM_MAX 512

static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static esp_event_handler_instance_t s_wifi_handler;
static esp_event_handler_instance_t s_ip_handler;
static esp_timer_handle_t s_retry_timer;
static esp_timer_handle_t s_provision_stop_timer;
static httpd_handle_t s_http_server;
static wifi_config_t s_sta_config;
static char s_ip[16];
static char s_last_error[64];
static fm_wifi_state_t s_state;
static bool s_wifi_initialized;
static bool s_wifi_started;
static bool s_wifi_connected;
static bool s_wifi_connecting;
static bool s_ap_active;
static bool s_reconnect_after_disconnect;
static uint8_t s_retry_count;

static const char FORM_HTML[] =
    "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>FlightMate Setup</title>"
    "<style>body{font-family:system-ui;background:#06111c;color:#eaf7ff;margin:0;padding:24px}"
    "main{max-width:420px;margin:auto;background:#0d2236;border-radius:14px;padding:22px}"
    "h1{color:#31d7c5;font-size:24px}p{color:#83a3b8;line-height:1.5}"
    "label{display:block;margin-top:16px}input{box-sizing:border-box;width:100%;padding:12px;"
    "margin-top:6px;border:1px solid #1b425a;border-radius:8px;background:#06111c;color:#fff}"
    "button{width:100%;padding:13px;margin-top:20px;border:0;border-radius:8px;background:#31d7c5;"
    "color:#06111c;font-weight:700}</style><main><h1>FlightMate Setup</h1>"
    "<p>Enter the 2.4 GHz Wi-Fi network used by the device.</p>"
    "<form method=post action=/save><label>SSID<input name=ssid maxlength=32 required></label>"
    "<label>Password<input name=password type=password maxlength=64></label>"
    "<button type=submit>Connect</button></form></main>";

static const char SAVED_HTML[] =
    "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>FlightMate</title><body style='font-family:system-ui;background:#06111c;color:#eaf7ff;"
    "padding:32px;text-align:center'><h1 style='color:#31d7c5'>Saved</h1>"
    "<p>FlightMate is connecting. You can return to the device.</p></body>";

static void set_error(esp_err_t error)
{
    snprintf(s_last_error, sizeof(s_last_error), "%s", esp_err_to_name(error));
    s_state = FM_WIFI_FAILED;
}

static const char *disconnect_reason_text(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
        return "找不到路由器，请确认是 2.4GHz 且信号良好";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return "路由器加密方式不兼容";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "路由器信号太弱";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "密码错误或握手超时";
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_CONNECTION_FAIL:
        return "路由器拒绝连接";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "与本设备连接中断";
    default:
        return "连接失败";
    }
}

static bool have_ssid(void)
{
    return s_sta_config.sta.ssid[0] != '\0';
}

static void request_wifi_connect(void)
{
    if (!s_wifi_started) return;
    const bool was_connected = s_wifi_connected;
    s_wifi_connecting = true;
    s_wifi_connected = false;
    s_state = FM_WIFI_CONNECTING;
    s_last_error[0] = '\0';
    if (was_connected) {
        s_reconnect_after_disconnect = true;
        if (esp_wifi_disconnect() == ESP_OK) return;
        s_reconnect_after_disconnect = false;
    }
    ESP_LOGI(TAG, "Connecting to SSID \"%s\" pw_len=%u auth_thr=%d",
             (const char *)s_sta_config.sta.ssid,
             (unsigned)strlen((const char *)s_sta_config.sta.password),
             (int)s_sta_config.sta.threshold.authmode);
    const esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        s_wifi_connecting = false;
        ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
        set_error(err);
    }
}

static void retry_timer_cb(void *arg)
{
    (void)arg;
    if (s_wifi_started && have_ssid() && !s_wifi_connected) request_wifi_connect();
}

static void schedule_retry(void)
{
    if (!s_retry_timer) return;
    esp_timer_stop(s_retry_timer);
    const esp_err_t err = esp_timer_start_once(s_retry_timer, FM_WIFI_RETRY_DELAY_US);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "retry timer start failed: %s", esp_err_to_name(err));
    }
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_STA_START) {
        if (!s_wifi_initialized) return;
        esp_wifi_get_config(WIFI_IF_STA, &s_sta_config);
        if (have_ssid()) {
            if (!s_wifi_connected && !s_wifi_connecting) request_wifi_connect();
        } else if (!s_ap_active) {
            s_state = FM_WIFI_OFF;
        }
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = data;
        ESP_LOGW(TAG, "Wi-Fi disconnected, reason=%u", event->reason);
        s_wifi_connected = false;
        s_wifi_connecting = false;
        s_ip[0] = '\0';
        snprintf(s_last_error, sizeof(s_last_error), "%s（%u）",
                 disconnect_reason_text(event->reason), event->reason);
        if (s_reconnect_after_disconnect) {
            s_reconnect_after_disconnect = false;
            request_wifi_connect();
        } else if (have_ssid()) {
            if (s_retry_count < FM_WIFI_MAX_RETRIES) {
                ++s_retry_count;
                s_state = FM_WIFI_CONNECTING;
                ESP_LOGI(TAG, "Wi-Fi retry %u/%u", s_retry_count, FM_WIFI_MAX_RETRIES);
                schedule_retry();
            } else {
                ESP_LOGW(TAG, "Wi-Fi connect failed: %s", s_last_error);
                s_state = FM_WIFI_FAILED;
            }
        } else if (s_ap_active) {
            s_state = FM_WIFI_PROVISIONING;
        } else {
            s_state = FM_WIFI_OFF;
        }
    }
}

static void ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id != IP_EVENT_STA_GOT_IP) return;
    const ip_event_got_ip_t *event = data;
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&event->ip_info.ip));
    s_wifi_connecting = false;
    s_wifi_connected = true;
    s_retry_count = 0;
    s_last_error[0] = '\0';
    s_state = FM_WIFI_CONNECTED;
    ESP_LOGI(TAG, "Wi-Fi connected: ssid=%s ip=%s", s_sta_config.sta.ssid, s_ip);
    if (s_ap_active && s_provision_stop_timer) {
        esp_timer_stop(s_provision_stop_timer);
        if (esp_timer_start_once(s_provision_stop_timer, FM_AP_STOP_DELAY_US) != ESP_OK) {
            ESP_LOGW(TAG, "provision stop timer start failed");
        }
    }
}

static int hex_value(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static void url_decode(char *text)
{
    if (!text) return;
    char *read = text;
    char *write = text;
    while (*read) {
        if (*read == '+') {
            *write++ = ' ';
            ++read;
        } else if (*read == '%' && hex_value(read[1]) >= 0 && hex_value(read[2]) >= 0) {
            *write++ = (char)((hex_value(read[1]) << 4) | hex_value(read[2]));
            read += 3;
        } else {
            *write++ = *read++;
        }
    }
    *write = '\0';
}


static esp_err_t root_get_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    return httpd_resp_send(request, FORM_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t save_post_handler(httpd_req_t *request)
{
    if (request->content_len <= 0 || request->content_len >= FM_FORM_MAX) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid form");
    }
    char form[FM_FORM_MAX] = { 0 };
    int received = 0;
    while (received < request->content_len) {
        const int result = httpd_req_recv(request, form + received, request->content_len - received);
        if (result <= 0) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
        received += result;
    }
    form[received] = '\0';

    char ssid[33] = { 0 };
    char password[65] = { 0 };
    /* Parse the whole body in ONE pass. The earlier two-call version reused
       form_value(), which rewrites '&' and '=' into NUL terminators; the first
       (ssid) lookup therefore destroyed the body and the second (password)
       lookup could never match, leaving the stored password empty. That made
       the station hunt for an OPEN AP and fail with reason 210. */
    for (char *cursor = form; cursor && *cursor; ) {
        char *amp = strchr(cursor, '&');
        if (amp) *amp = '\0';
        char *eq = strchr(cursor, '=');
        if (eq) {
            *eq = '\0';
            char *value = eq + 1;
            url_decode(value);
            if (strcmp(cursor, "ssid") == 0) {
                strncpy(ssid, value, sizeof(ssid) - 1);
                ssid[sizeof(ssid) - 1] = '\0';
            } else if (strcmp(cursor, "password") == 0) {
                strncpy(password, value, sizeof(password) - 1);
                password[sizeof(password) - 1] = '\0';
            }
        }
        cursor = amp ? amp + 1 : NULL;
    }
    if (ssid[0] == '\0') {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "SSID is required");
    }
    ESP_LOGI(TAG, "form received: ssid=\"%s\" pw_len=%u", ssid, (unsigned)strlen(password));

    wifi_config_t config = { 0 };
    if (strlen(ssid) > sizeof(config.sta.ssid) - 1 ||
        strlen(password) > sizeof(config.sta.password) - 1) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Credential too long");
    }
    memcpy(config.sta.ssid, ssid, strlen(ssid) + 1);
    memcpy(config.sta.password, password, strlen(password) + 1);
    /* Do not over-restrict the security threshold: WIFI_AUTH_WPA_WPA2_PSK (4)
       rejects plain WPA2-PSK APs (authmode 3), which surfaced as disconnect
       reason 210 (NO_AP_FOUND_W_COMPATIBLE_SECURITY). Accept any security the
       driver supports and let it pick the best match. */
    config.sta.threshold.authmode = password[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    config.sta.pmf_cfg.capable = false;
    config.sta.pmf_cfg.required = false;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (err == ESP_OK) {
        s_sta_config = config;
        s_retry_count = 0;
        s_last_error[0] = '\0';
        s_wifi_connected = false;
        s_wifi_connecting = false;
        s_state = FM_WIFI_CONNECTING;
        /* The setup AP pins the radio to its own channel, so a station scan in
           AP+STA mode cannot see the target AP on another channel (observed as
           disconnect reason 210). Deliver the HTTP response first, then drop
           the setup AP and let the station scan every channel. */
        if (s_provision_stop_timer) {
            esp_timer_stop(s_provision_stop_timer);
            if (esp_timer_start_once(s_provision_stop_timer, FM_AP_STOP_AFTER_SAVE_US) != ESP_OK) {
                fm_wifi_provisioning_stop();
            }
        } else {
            fm_wifi_provisioning_stop();
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save Wi-Fi config failed: %s", esp_err_to_name(err));
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Save failed");
    }
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    return httpd_resp_send(request, SAVED_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t start_http_server(void)
{
    if (s_http_server) return ESP_OK;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 4;
    config.lru_purge_enable = true;
    config.stack_size = 6144;
    esp_err_t err = httpd_start(&s_http_server, &config);
    if (err != ESP_OK) return err;
    const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
    };
    const httpd_uri_t save = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = save_post_handler,
    };
    err = httpd_register_uri_handler(s_http_server, &root);
    if (err == ESP_OK) err = httpd_register_uri_handler(s_http_server, &save);
    if (err != ESP_OK) {
        httpd_stop(s_http_server);
        s_http_server = NULL;
    }
    return err;
}

static void provision_stop_timer_cb(void *arg)
{
    (void)arg;
    fm_wifi_provisioning_stop();
}

esp_err_t fm_wifi_init(void)
{
    if (s_wifi_initialized) return ESP_OK;

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (!s_sta_netif) return ESP_ERR_NO_MEM;
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_ap_netif) return ESP_ERR_NO_MEM;

    const wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&config);
    if (err != ESP_OK) return err;
    s_wifi_initialized = true;
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              wifi_event, NULL, &s_wifi_handler);
    if (err != ESP_OK) return err;
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              ip_event, NULL, &s_ip_handler);
    if (err != ESP_OK) return err;
    const esp_timer_create_args_t timer_args = {
        .callback = retry_timer_cb,
        .name = "fm_wifi_retry",
    };
    err = esp_timer_create(&timer_args, &s_retry_timer);
    if (err != ESP_OK) return err;
    const esp_timer_create_args_t stop_args = {
        .callback = provision_stop_timer_cb,
        .name = "fm_ap_stop",
    };
    err = esp_timer_create(&stop_args, &s_provision_stop_timer);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_STA);
    /* Mark the driver as started before esp_wifi_start() so the STA_START
       event handler (which runs on the higher-priority event loop task and
       can preempt this task) is allowed to kick off the connection. */
    s_wifi_started = true;
    err = esp_wifi_start();
    if (err != ESP_OK) {
        s_wifi_started = false;
        return err;
    }
    /* Also read the stored config synchronously and connect here, so a missed
       STA_START event can never leave the station idle. */
    if (esp_wifi_get_config(WIFI_IF_STA, &s_sta_config) == ESP_OK && have_ssid()) {
        ESP_LOGI(TAG, "Stored Wi-Fi credentials for SSID \"%s\"", (const char *)s_sta_config.sta.ssid);
        s_retry_count = 0;
        if (!s_wifi_connecting) request_wifi_connect();
    } else {
        ESP_LOGI(TAG, "No stored Wi-Fi credentials");
        s_state = FM_WIFI_OFF;
    }
    return ESP_OK;
}

esp_err_t fm_wifi_reconnect(void)
{
    if (!s_wifi_started) return ESP_ERR_INVALID_STATE;
    esp_wifi_get_config(WIFI_IF_STA, &s_sta_config);
    if (!have_ssid()) return ESP_ERR_INVALID_STATE;
    s_retry_count = 0;
    request_wifi_connect();
    return ESP_OK;
}

esp_err_t fm_wifi_provisioning_start(void)
{
    if (!s_wifi_started) return ESP_ERR_INVALID_STATE;
    if (s_ap_active) return ESP_OK;

    wifi_config_t ap_config = {
        .ap = {
            .channel = 1,
            .max_connection = 2,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    memcpy(ap_config.ap.ssid, FM_PROVISION_SSID, sizeof(FM_PROVISION_SSID));
    memcpy(ap_config.ap.password, FM_PROVISION_PASSWORD, sizeof(FM_PROVISION_PASSWORD));
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (err == ESP_OK) err = start_http_server();
    if (err != ESP_OK) {
        esp_wifi_set_mode(WIFI_MODE_STA);
        set_error(err);
        return err;
    }
    s_ap_active = true;
    s_retry_count = 0;
    s_state = FM_WIFI_PROVISIONING;
    return ESP_OK;
}

void fm_wifi_provisioning_stop(void)
{
    if (s_provision_stop_timer) esp_timer_stop(s_provision_stop_timer);
    if (!s_ap_active) return;
    if (s_http_server) {
        httpd_stop(s_http_server);
        s_http_server = NULL;
    }
    s_ap_active = false;
    esp_wifi_set_mode(WIFI_MODE_STA);
    if (s_wifi_connected) {
        s_state = FM_WIFI_CONNECTED;
    } else if (have_ssid()) {
        s_retry_count = 0;
        request_wifi_connect();
    } else {
        s_state = FM_WIFI_OFF;
    }
}

esp_err_t fm_wifi_forget(void)
{
    if (!s_wifi_started) return ESP_ERR_INVALID_STATE;
    if (s_provision_stop_timer) esp_timer_stop(s_provision_stop_timer);
    s_reconnect_after_disconnect = false;
    esp_wifi_disconnect();
    wifi_config_t empty = { 0 };
    const esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &empty);
    memset(&s_sta_config, 0, sizeof(s_sta_config));
    s_wifi_connected = false;
    s_wifi_connecting = false;
    s_retry_count = 0;
    s_ip[0] = '\0';
    s_last_error[0] = '\0';
    s_state = s_ap_active ? FM_WIFI_PROVISIONING : FM_WIFI_OFF;
    return err;
}

bool fm_wifi_connected(void) { return s_wifi_connected; }
bool fm_wifi_has_credentials(void) { return have_ssid(); }
bool fm_wifi_provisioning_active(void) { return s_ap_active; }
fm_wifi_state_t fm_wifi_state(void)
{
    if (s_wifi_connected) return FM_WIFI_CONNECTED;
    return s_state;
}
const char *fm_wifi_ip(void) { return s_ip; }
const char *fm_wifi_ssid(void) { return (const char *)s_sta_config.sta.ssid; }
const char *fm_wifi_last_error(void) { return s_last_error; }