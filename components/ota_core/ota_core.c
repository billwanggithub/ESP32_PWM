#include "ota_core.h"

#include <string.h>
#include <stdatomic.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "ota_core";

static struct {
    bool                 initialised;
    SemaphoreHandle_t    mtx;
    esp_ota_handle_t     handle;
    const esp_partition_t *target;
    _Atomic uint32_t     state;
    _Atomic uint32_t     progress;
    _Atomic uint32_t     total;
} s;

esp_err_t ota_core_init(void)
{
    if (s.initialised) return ESP_OK;
    s.mtx = xSemaphoreCreateMutex();
    if (!s.mtx) return ESP_ERR_NO_MEM;
    s.initialised = true;
    atomic_store(&s.state, OTA_STATE_IDLE);
    return ESP_OK;
}

esp_err_t ota_core_begin(uint32_t total_size)
{
    if (!s.initialised) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s.mtx, 0) != pdTRUE) return ESP_ERR_INVALID_STATE;

    s.target = esp_ota_get_next_update_partition(NULL);
    if (!s.target) { xSemaphoreGive(s.mtx); return ESP_ERR_NOT_FOUND; }

    esp_err_t e = esp_ota_begin(s.target, total_size ? total_size : OTA_SIZE_UNKNOWN, &s.handle);
    if (e != ESP_OK) { xSemaphoreGive(s.mtx); atomic_store(&s.state, OTA_STATE_ERROR); return e; }

    atomic_store(&s.progress, 0);
    atomic_store(&s.total, total_size);
    atomic_store(&s.state, OTA_STATE_WRITING);
    ESP_LOGI(TAG, "begin → partition %s, size %lu", s.target->label, (unsigned long)total_size);
    return ESP_OK;
}

esp_err_t ota_core_write(const uint8_t *data, size_t len)
{
    if (atomic_load(&s.state) != OTA_STATE_WRITING) return ESP_ERR_INVALID_STATE;
    esp_err_t e = esp_ota_write(s.handle, data, len);
    if (e != ESP_OK) {
        atomic_store(&s.state, OTA_STATE_ERROR);
        esp_ota_abort(s.handle);
        xSemaphoreGive(s.mtx);
        return e;
    }
    atomic_fetch_add(&s.progress, (uint32_t)len);
    return ESP_OK;
}

esp_err_t ota_core_end_and_reboot(void)
{
    if (atomic_load(&s.state) != OTA_STATE_WRITING) return ESP_ERR_INVALID_STATE;
    esp_err_t e = esp_ota_end(s.handle);   // verifies signature under Secure Boot V2
    if (e != ESP_OK) {
        atomic_store(&s.state, OTA_STATE_ERROR);
        xSemaphoreGive(s.mtx);
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(e));
        return e;
    }
    e = esp_ota_set_boot_partition(s.target);
    if (e != ESP_OK) {
        atomic_store(&s.state, OTA_STATE_ERROR);
        xSemaphoreGive(s.mtx);
        ESP_LOGE(TAG, "set_boot_partition failed: %s", esp_err_to_name(e));
        return e;
    }
    atomic_store(&s.state, OTA_STATE_DONE);
    ESP_LOGW(TAG, "OTA done, rebooting in 500 ms");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    // not reached
    return ESP_OK;
}

esp_err_t ota_core_abort(void)
{
    if (atomic_load(&s.state) == OTA_STATE_WRITING) {
        esp_ota_abort(s.handle);
    }
    atomic_store(&s.state, OTA_STATE_IDLE);
    xSemaphoreGive(s.mtx);
    return ESP_OK;
}

ota_state_t ota_core_state(void) { return (ota_state_t)atomic_load(&s.state); }
uint32_t    ota_core_progress(void) { return atomic_load(&s.progress); }
uint32_t    ota_core_total(void)    { return atomic_load(&s.total); }

void ota_core_mark_current_image_valid(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
        if (state == ESP_OTA_IMG_PENDING_VERIFY) {
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "marked current image valid (rollback cancelled)");
        }
    }
}
