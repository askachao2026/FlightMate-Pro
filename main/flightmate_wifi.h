#pragma once

#include "esp_err.h"
#include <stdbool.h>

typedef enum {
    FM_WIFI_OFF = 0,
    FM_WIFI_CONNECTING,
    FM_WIFI_CONNECTED,
    FM_WIFI_PROVISIONING,
    FM_WIFI_FAILED,
} fm_wifi_state_t;

esp_err_t fm_wifi_init(void);
esp_err_t fm_wifi_reconnect(void);
esp_err_t fm_wifi_provisioning_start(void);
void fm_wifi_provisioning_stop(void);
esp_err_t fm_wifi_forget(void);

bool fm_wifi_connected(void);
bool fm_wifi_has_credentials(void);
bool fm_wifi_provisioning_active(void);
fm_wifi_state_t fm_wifi_state(void);
const char *fm_wifi_ip(void);
const char *fm_wifi_ssid(void);
const char *fm_wifi_last_error(void);