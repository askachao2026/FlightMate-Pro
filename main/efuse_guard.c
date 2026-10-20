/*
 * FlightMate eFuse guard.
 *
 * The application never writes eFuse: nothing here burns keys, MACs or
 * calibration data, and secure boot / flash encryption are disabled in
 * sdkconfig. The ESP-IDF startup still calls esp_efuse_check_errors(), which
 * reads the BLOCK0 coding-error registers and can restart the chip. FlightMate
 * does not need that check, so the build wraps it with the no-op below. The
 * real implementation then has no caller and the linker keeps it (and its
 * register-level diagnostics) out of the image.
 *
 * See tools/check_no_efuse_write.py for the build-time verification.
 */
#include "esp_err.h"

esp_err_t __wrap_esp_efuse_check_errors(void);
esp_err_t __real_esp_efuse_check_errors(void);

esp_err_t __wrap_esp_efuse_check_errors(void)
{
    return ESP_OK;
}
