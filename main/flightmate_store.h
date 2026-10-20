#pragma once

#include "esp_err.h"
#include <stdint.h>

#include "flightmate_logic.h"

typedef struct {
    uint8_t airport_index;
    uint8_t brightness_percent;
    uint16_t reserved;
    uint16_t log_count;
    fm_log_record_t logs[FM_MAX_LOGS];
} fm_store_t;

esp_err_t fm_store_load(fm_store_t *store);
esp_err_t fm_store_save(const fm_store_t *store);
esp_err_t fm_store_add_log(fm_store_t *store, const fm_log_record_t *record);
esp_err_t fm_store_remove_log(fm_store_t *store, size_t index);