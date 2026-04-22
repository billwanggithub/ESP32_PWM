#include <string.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"

// ESP-IDF v6.0 拔掉 built-in wifi_provisioning，搬到 component manager 上的
// espressif/network_provisioning。symbol prefix 從 wifi_prov_* 改成
// network_prov_*，部分 event 多了 WIFI infix（NETWORK_PROV_WIFI_CRED_*）因為
// component 同時支援 Wi-Fi 跟 Thread provisioning。
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_ble.h"

#include "prov_internal.h"

static const char *TAG = "prov";

static EventGroupHandle_t s_wifi_events;
#define WIFI_CONNECTED_BIT BIT0

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    } else if (base == NETWORK_PROV_EVENT) {
        switch (id) {
        case NETWORK_PROV_WIFI_CRED_SUCCESS: ESP_LOGI(TAG, "provisioning success"); break;
        case NETWORK_PROV_WIFI_CRED_FAIL:    ESP_LOGW(TAG, "provisioning failed");  break;
        case NETWORK_PROV_END:
            network_prov_mgr_deinit();
            break;
        default: break;
        }
    }
}

esp_err_t provisioning_run_and_connect(void)
{
    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,     ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,       IP_EVENT_STA_GOT_IP, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));

    network_prov_mgr_config_t prov_cfg = {
        .scheme               = network_prov_scheme_ble,
        .scheme_event_handler = NETWORK_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM,
    };
    ESP_ERROR_CHECK(network_prov_mgr_init(prov_cfg));

    bool provisioned = false;
    ESP_ERROR_CHECK(network_prov_mgr_is_wifi_provisioned(&provisioned));

    if (!provisioned) {
        ESP_LOGI(TAG, "no credentials → starting BLE provisioning");
        const char *service_name = "ESP32-PWM";
        const char *pop          = "abcd1234";
        network_prov_security_t security = NETWORK_PROV_SECURITY_1;
        ESP_ERROR_CHECK(network_prov_mgr_start_provisioning(security, pop, service_name, NULL));
        network_prov_mgr_wait();
    } else {
        ESP_LOGI(TAG, "already provisioned → connecting");
        network_prov_mgr_deinit();
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
    }

    xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "wifi connected");
    return ESP_OK;
}

// network_prov_mgr_reset_wifi_provisioning() works even after the manager has
// been deinit'd (see already-provisioned branch above) — it talks straight to
// the underlying wifi NVS entries.
esp_err_t prov_clear_credentials(void)
{
    ESP_LOGW(TAG, "clearing wifi credentials");
    return network_prov_mgr_reset_wifi_provisioning();
}
