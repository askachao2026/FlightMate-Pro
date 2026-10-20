#include "flightmate_store.h"

#include "nvs.h"
#include <string.h>

#define FM_NVS_NAMESPACE "flightmate"
#define FM_NVS_SETTINGS_KEY "settings"
#define FM_STORE_SCHEMA 3

typedef struct {
    uint16_t schema;
    fm_store_t data;
} fm_store_blob_t;

esp_err_t fm_store_load(fm_store_t *store)
{
    if (!store) return ESP_ERR_INVALID_ARG;
    memset(store, 0, sizeof(*store));
    store->brightness_percent = 75;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(FM_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;

    fm_store_blob_t blob = { 0 };
    size_t size = sizeof(blob);
    err = nvs_get_blob(handle, FM_NVS_SETTINGS_KEY, &blob, &size);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK || size < sizeof(uint16_t) || blob.schema != FM_STORE_SCHEMA) return ESP_OK;

    *store = blob.data;
    if (store->airport_index >= FM_AIRPORT_COUNT) store->airport_index = 0;
    if (store->brightness_percent < 25 || store->brightness_percent > 100) {
        store->brightness_percent = 75;
    }
    if (store->log_count > FM_MAX_LOGS) store->log_count = FM_MAX_LOGS;
    return ESP_OK;
}

esp_err_t fm_store_save(const fm_store_t *store)
{
    if (!store) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(FM_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    fm_store_blob_t blob = {
        .schema = FM_STORE_SCHEMA,
        .data = *store,
    };
    if (blob.data.airport_index >= FM_AIRPORT_COUNT) blob.data.airport_index = 0;
    if (blob.data.brightness_percent < 25 || blob.data.brightness_percent > 100) {
        blob.data.brightness_percent = 75;
    }
    if (blob.data.log_count > FM_MAX_LOGS) blob.data.log_count = FM_MAX_LOGS;

    err = nvs_set_blob(handle, FM_NVS_SETTINGS_KEY, &blob, sizeof(blob));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t fm_store_add_log(fm_store_t *store, const fm_log_record_t *record)
{
    if (!store || !record) return ESP_ERR_INVALID_ARG;
    if (store->log_count < FM_MAX_LOGS) ++store->log_count;
    for (size_t i = store->log_count - 1; i > 0; --i) {
        store->logs[i] = store->logs[i - 1];
    }
    store->logs[0] = *record;
    return fm_store_save(store);
}

esp_err_t fm_store_remove_log(fm_store_t *store, size_t index)
{
    if (!store || index >= store->log_count) return ESP_ERR_INVALID_ARG;
    for (size_t i = index; i + 1 < store->log_count; ++i) {
        store->logs[i] = store->logs[i + 1];
    }
    if (store->log_count > 0) --store->log_count;
    memset(&store->logs[store->log_count], 0, sizeof(store->logs[0]));
    return fm_store_save(store);
}
