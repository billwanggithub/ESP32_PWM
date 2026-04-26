#include "ip_announcer.h"
#include "ip_announcer_priv.h"

#include <string.h>
#include <stdatomic.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "ip_announcer";

#define NVS_NAMESPACE       "ip_announcer"
#define NVS_KEY_ENABLE      "enable"
#define NVS_KEY_TOPIC       "topic"
#define NVS_KEY_SERVER      "server"
#define NVS_KEY_PRIORITY    "priority"
#define NVS_KEY_LAST_IP     "last_ip"

#define DEFAULT_PRIORITY    3

// Mutex protecting s_settings + s_telemetry.
static SemaphoreHandle_t s_lock;
static ip_announcer_settings_t  s_settings;
static ip_announcer_telemetry_t s_telemetry;

static void generate_random_topic(char *out, size_t out_size)
{
    static const char alphabet[] =
        "abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "0123456789";  // 62 chars
    char tok[33];
    for (int i = 0; i < 32; i++) {
        tok[i] = alphabet[esp_random() % 62];
    }
    tok[32] = '\0';
    snprintf(out, out_size, "fan-testkit-%s", tok);
}

static esp_err_t persist_settings_unlocked(const ip_announcer_settings_t *s)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    esp_err_t e1 = nvs_set_u8 (h, NVS_KEY_ENABLE,   s->enable ? 1 : 0);
    esp_err_t e2 = nvs_set_str(h, NVS_KEY_TOPIC,    s->topic);
    esp_err_t e3 = nvs_set_str(h, NVS_KEY_SERVER,   s->server);
    esp_err_t e4 = nvs_set_u8 (h, NVS_KEY_PRIORITY, s->priority);
    esp_err_t ec = (e1 == ESP_OK && e2 == ESP_OK && e3 == ESP_OK && e4 == ESP_OK)
                   ? nvs_commit(h) : ESP_OK;
    nvs_close(h);
    if (e1 != ESP_OK) return e1;
    if (e2 != ESP_OK) return e2;
    if (e3 != ESP_OK) return e3;
    if (e4 != ESP_OK) return e4;
    return ec;
}

void ip_announcer_on_ip_event(void *arg, esp_event_base_t base,
                              int32_t id, void *data)
{
    (void)arg;
    if (base != IP_EVENT || id != IP_EVENT_STA_GOT_IP) return;

    ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
    char ip[16];
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&evt->ip_info.ip));

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool enable = s_settings.enable;
    bool same   = (strcmp(ip, s_telemetry.last_pushed_ip) == 0)
                  && (s_telemetry.status == IP_ANN_STATUS_OK);
    xSemaphoreGive(s_lock);

    if (!enable) {
        ESP_LOGI(TAG, "IP %s — push disabled", ip);
        return;
    }
    if (same) {
        ESP_LOGI(TAG, "IP %s — already pushed; skipping (dedupe)", ip);
        return;
    }
    ESP_LOGI(TAG, "IP %s — enqueueing push", ip);
    esp_err_t e = ip_announcer_priv_enqueue_push(ip);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "enqueue failed: %s", esp_err_to_name(e));
    }
}

esp_err_t ip_announcer_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    memset(&s_settings,  0, sizeof(s_settings));
    memset(&s_telemetry, 0, sizeof(s_telemetry));
    s_settings.priority  = DEFAULT_PRIORITY;
    s_telemetry.status   = IP_ANN_STATUS_NEVER;

    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    bool nvs_has_topic = false;
    if (e == ESP_OK) {
        uint8_t en = 0;
        if (nvs_get_u8(h, NVS_KEY_ENABLE, &en) == ESP_OK) {
            s_settings.enable = (en != 0);
        }
        size_t sz = sizeof(s_settings.topic);
        if (nvs_get_str(h, NVS_KEY_TOPIC, s_settings.topic, &sz) == ESP_OK
            && s_settings.topic[0] != '\0') {
            nvs_has_topic = true;
        }
        sz = sizeof(s_settings.server);
        nvs_get_str(h, NVS_KEY_SERVER, s_settings.server, &sz);
        uint8_t pr = DEFAULT_PRIORITY;
        if (nvs_get_u8(h, NVS_KEY_PRIORITY, &pr) == ESP_OK) {
            if (pr < 1) pr = 1;
            if (pr > 5) pr = 5;
            s_settings.priority = pr;
        }
        sz = sizeof(s_telemetry.last_pushed_ip);
        nvs_get_str(h, NVS_KEY_LAST_IP, s_telemetry.last_pushed_ip, &sz);
        nvs_close(h);
    }

    // 3-tier topic resolution.
    if (!nvs_has_topic) {
        const char *kcfg = CONFIG_APP_IP_ANNOUNCER_TOPIC_DEFAULT;
        if (kcfg && kcfg[0] != '\0') {
            strncpy(s_settings.topic, kcfg, sizeof(s_settings.topic) - 1);
            s_settings.topic[sizeof(s_settings.topic) - 1] = '\0';
            ESP_LOGI(TAG, "topic from Kconfig: %s", s_settings.topic);
        } else {
            generate_random_topic(s_settings.topic, sizeof(s_settings.topic));
            ESP_LOGI(TAG, "topic random-generated: %s", s_settings.topic);
        }
    } else {
        ESP_LOGI(TAG, "topic from NVS: %s", s_settings.topic);
    }

    // Server fallback to Kconfig if not in NVS.
    if (s_settings.server[0] == '\0') {
        const char *kcfg = CONFIG_APP_IP_ANNOUNCER_SERVER_DEFAULT;
        const char *src = (kcfg && kcfg[0] != '\0') ? kcfg : "ntfy.sh";
        strncpy(s_settings.server, src, sizeof(s_settings.server) - 1);
        s_settings.server[sizeof(s_settings.server) - 1] = '\0';
    }

    // Persist resolved topic + server back so subsequent boots take
    // the fast path.
    esp_err_t pe = persist_settings_unlocked(&s_settings);
    if (pe != ESP_OK) {
        ESP_LOGW(TAG, "persist on init failed: %s", esp_err_to_name(pe));
    }

    ESP_LOGI(TAG, "init: enable=%d topic=%s server=%s priority=%u",
             s_settings.enable, s_settings.topic, s_settings.server,
             s_settings.priority);

    e = ip_announcer_push_init();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "push init failed: %s", esp_err_to_name(e));
        return e;
    }

    extern void ip_announcer_on_ip_event(void *arg, esp_event_base_t base,
                                         int32_t id, void *data);
    esp_err_t reg_e = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                 ip_announcer_on_ip_event, NULL);
    if (reg_e == ESP_ERR_INVALID_STATE) {
        // Default loop already created by provisioning. Some IDF versions
        // surface this; ignore — handler still registers via the existing loop.
        ESP_LOGD(TAG, "esp_event_handler_register: default loop already exists");
    } else if (reg_e != ESP_OK) {
        ESP_LOGE(TAG, "event handler register failed: %s",
                 esp_err_to_name(reg_e));
        return reg_e;
    }
    return ESP_OK;
}

esp_err_t ip_announcer_get_settings(ip_announcer_settings_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_settings;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t ip_announcer_set_settings(const ip_announcer_settings_t *in)
{
    if (!in) return ESP_ERR_INVALID_ARG;
    if (in->topic[0] == '\0' || strlen(in->topic) < 8 || strlen(in->topic) > 64) {
        return ESP_ERR_INVALID_ARG;
    }
    if (in->server[0] == '\0') return ESP_ERR_INVALID_ARG;
    uint8_t pr = in->priority;
    if (pr < 1) pr = 1;
    if (pr > 5) pr = 5;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_settings.enable   = in->enable;
    strncpy(s_settings.topic,  in->topic,  sizeof(s_settings.topic) - 1);
    s_settings.topic[sizeof(s_settings.topic) - 1] = '\0';
    strncpy(s_settings.server, in->server, sizeof(s_settings.server) - 1);
    s_settings.server[sizeof(s_settings.server) - 1] = '\0';
    s_settings.priority = pr;
    esp_err_t e = persist_settings_unlocked(&s_settings);
    xSemaphoreGive(s_lock);
    return e;
}

esp_err_t ip_announcer_set_enable(bool enable)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_settings.enable = enable;
    esp_err_t e = persist_settings_unlocked(&s_settings);
    xSemaphoreGive(s_lock);
    return e;
}

void ip_announcer_get_telemetry(ip_announcer_telemetry_t *out)
{
    if (!out) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_telemetry;
    xSemaphoreGive(s_lock);
}

esp_err_t ip_announcer_test_push(void)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta) return ESP_ERR_INVALID_STATE;
    esp_netif_ip_info_t info = {0};
    if (esp_netif_get_ip_info(sta, &info) != ESP_OK) return ESP_ERR_INVALID_STATE;
    if (info.ip.addr == 0) return ESP_ERR_INVALID_STATE;
    char ip[16];
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
    return ip_announcer_priv_enqueue_push(ip);
}

void ip_announcer_priv_get_settings(ip_announcer_settings_t *out)
{
    if (!out) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_settings;
    xSemaphoreGive(s_lock);
}

void ip_announcer_priv_set_telemetry(const ip_announcer_telemetry_t *t)
{
    if (!t) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_telemetry = *t;
    // Persist last_ip on success so dedupe survives reboot.
    if (t->status == IP_ANN_STATUS_OK && t->last_pushed_ip[0]) {
        nvs_handle_t h;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_str(h, NVS_KEY_LAST_IP, t->last_pushed_ip);
            nvs_commit(h);
            nvs_close(h);
        }
    }
    xSemaphoreGive(s_lock);
}
