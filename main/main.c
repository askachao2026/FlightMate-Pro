#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "flightmate_net.h"
#include "flightmate_store.h"
#include "flightmate_ui.h"
#include "flightmate_wifi.h"
#include "airband_audio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_sleep.h"
#include "nvs_flash.h"

static const char *TAG = "flightmate";

static esp_err_t storage_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err == ESP_OK) err = nvs_flash_init();
    }
    return err;
}

void app_main(void)
{
    ESP_LOGI(TAG, "FlightMate boot, wakeup=%d", esp_sleep_get_wakeup_cause());

    esp_err_t err = storage_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
        return;
    }

    bsp_i2c_init();
    bsp_i2c_scan();
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "display/LVGL init failed: MOSI=%d SCLK=%d CS=%d DC=%d BL=%d",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }

    static fm_store_t store;
    fm_store_load(&store);
    bsp_display_backlight(store.brightness_percent);

    const esp_err_t battery_err = bsp_battery_init();
    if (battery_err != ESP_OK) {
        ESP_LOGW(TAG, "battery unavailable: %s", esp_err_to_name(battery_err));
    }

    /* 音频任务栈要 24KB 连续内存：开机先建，空闲后任务自毁把内存还给 HTTPS。 */
    if (airband_audio_init() != ESP_OK) {
        ESP_LOGW(TAG, "audio task prealloc failed (free heap=%u)", (unsigned)esp_get_free_heap_size());
    } else {
        ESP_LOGI(TAG, "audio task preallocated (free heap=%u)", (unsigned)esp_get_free_heap_size());
    }

    err = fm_wifi_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi init failed: %s", esp_err_to_name(err));
    } else {
        fm_net_start_time_sync();
    }

    err = fm_net_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "network worker init failed: %s", esp_err_to_name(err));
    }

    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "LVGL lock failed during UI init");
        return;
    }
    err = fm_ui_init(&store);
    if (err != ESP_OK) {
        bsp_lvgl_unlock();
        ESP_LOGE(TAG, "UI init failed: %s", esp_err_to_name(err));
        return;
    }
    bsp_lvgl_unlock();

    err = bsp_button_init(fm_ui_key_event, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "button init failed: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "heap after init: free=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    ESP_LOGI(TAG, "FlightMate ready: battery=%s wifi=%s",
             battery_err == ESP_OK ? "ok" : "unavailable",
             fm_wifi_has_credentials() ? "configured" : "unprovisioned");
}
