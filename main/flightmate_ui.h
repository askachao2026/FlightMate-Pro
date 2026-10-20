#pragma once

#include "bsp_button.h"
#include "esp_err.h"

#include "flightmate_store.h"

esp_err_t fm_ui_init(fm_store_t *store);
void fm_ui_key_event(bsp_btn_t button, bsp_btn_ev_t event, void *user);