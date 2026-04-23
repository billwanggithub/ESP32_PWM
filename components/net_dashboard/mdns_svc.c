#include "mdns_svc.h"

#include "mdns.h"
#include "esp_log.h"

static const char *TAG = "mdns_svc";

esp_err_t mdns_svc_start(void)
{
    esp_err_t e = mdns_init();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init: %s", esp_err_to_name(e));
        return e;
    }
    ESP_ERROR_CHECK(mdns_hostname_set("fan-testkit"));
    ESP_ERROR_CHECK(mdns_instance_name_set("Fan-TestKit Dashboard"));
    ESP_ERROR_CHECK(mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0));
    ESP_LOGI(TAG, "mdns up: fan-testkit.local");
    return ESP_OK;
}

void mdns_svc_stop(void)
{
    mdns_free();
}
