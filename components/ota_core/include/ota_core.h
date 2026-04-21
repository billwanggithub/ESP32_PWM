#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OTA_STATE_IDLE,
    OTA_STATE_WRITING,
    OTA_STATE_DONE,
    OTA_STATE_ERROR,
} ota_state_t;

esp_err_t   ota_core_init(void);

// All three funnel into the same esp_ota_* sequence.
// Either transport (Wi-Fi POST /ota or CDC OTA frames) may call them, but not
// concurrently (single writer enforced by an internal mutex; second caller
// gets ESP_ERR_INVALID_STATE).
esp_err_t   ota_core_begin(uint32_t total_size);
esp_err_t   ota_core_write(const uint8_t *data, size_t len);
esp_err_t   ota_core_end_and_reboot(void);   // verifies signature, sets boot partition, reboots
esp_err_t   ota_core_abort(void);

ota_state_t ota_core_state(void);
uint32_t    ota_core_progress(void);
uint32_t    ota_core_total(void);

// Called from app_main after a successful boot-health check to cancel rollback.
void ota_core_mark_current_image_valid(void);

#ifdef __cplusplus
}
#endif
