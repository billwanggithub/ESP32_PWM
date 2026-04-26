#include "ip_announcer_priv.h"

#include <string.h>
#include <stdio.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "ip_ann_push";

#define PUSH_QUEUE_DEPTH    4
#define PUSH_RETRIES        3
#define PUSH_RETRY_GAP_MS   5000
#define PUSH_HTTP_TIMEOUT_MS 5000

typedef struct {
    char ip[16];
} push_job_t;

static QueueHandle_t s_push_q;
static TaskHandle_t  s_push_task;

// Mirror of announcerTopicLooksSafe() in net_dashboard/web/app.js — keep
// the rules in sync (len < 16, CHANGE-ME-*, fan-testkit-CHANGE*).
static bool topic_is_safe(const char *topic)
{
    if (!topic) return false;
    size_t n = strlen(topic);
    if (n < 16) return false;
    if (strncasecmp(topic, "CHANGE-ME-", 10) == 0) return false;
    if (strncasecmp(topic, "fan-testkit-CHANGE", 18) == 0) return false;
    return true;
}

static int do_push_once(const ip_announcer_settings_t *s, const char *ip,
                        char *err_out, size_t err_out_size)
{
    char url[160];
    snprintf(url, sizeof(url), "https://%s/%s", s->server, s->topic);

    char body[160];
    int body_n = snprintf(body, sizeof(body),
                          "IP: %s\nhttp://%s/\n", ip, ip);

    char click[64];
    snprintf(click, sizeof(click), "http://%s/", ip);

    char prio_s[2] = { (char)('0' + s->priority), '\0' };

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = PUSH_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        snprintf(err_out, err_out_size, "esp_http_client_init failed");
        return -1;
    }

    esp_http_client_set_header(cli, "Title",    "Fan-TestKit online");
    esp_http_client_set_header(cli, "Tags",     "green_circle");
    esp_http_client_set_header(cli, "Priority", prio_s);
    esp_http_client_set_header(cli, "Click",    click);
    esp_http_client_set_post_field(cli, body, body_n);

    esp_err_t e = esp_http_client_perform(cli);
    int http_code = -1;
    if (e == ESP_OK) {
        http_code = esp_http_client_get_status_code(cli);
        if (http_code < 200 || http_code >= 300) {
            snprintf(err_out, err_out_size, "HTTP %d", http_code);
        } else {
            err_out[0] = '\0';
        }
    } else {
        snprintf(err_out, err_out_size, "%s", esp_err_to_name(e));
    }
    esp_http_client_cleanup(cli);
    return http_code;
}

static void push_task(void *arg)
{
    (void)arg;
    push_job_t job;
    while (true) {
        if (xQueueReceive(s_push_q, &job, portMAX_DELAY) != pdTRUE) continue;

        ip_announcer_settings_t s;
        ip_announcer_priv_get_settings(&s);

        ip_announcer_telemetry_t t = {0};
        t.last_attempt_ms = esp_timer_get_time() / 1000;
        snprintf(t.last_pushed_ip, sizeof(t.last_pushed_ip), "%s", job.ip);

        if (!s.enable) {
            t.status = IP_ANN_STATUS_DISABLED;
            snprintf(t.last_err, sizeof(t.last_err), "%s", "announcer disabled");
            ip_announcer_priv_set_telemetry(&t);
            continue;
        }
        if (!topic_is_safe(s.topic)) {
            t.status = IP_ANN_STATUS_FAILED;
            snprintf(t.last_err, sizeof(t.last_err), "%s",
                     "topic placeholder; change before enabling");
            ip_announcer_priv_set_telemetry(&t);
            ESP_LOGW(TAG, "refusing push: %s", t.last_err);
            continue;
        }

        int http_code = -1;
        for (int attempt = 0; attempt < PUSH_RETRIES; attempt++) {
            http_code = do_push_once(&s, job.ip, t.last_err, sizeof(t.last_err));
            t.last_http_code = http_code;
            if (http_code >= 200 && http_code < 300) {
                t.status = IP_ANN_STATUS_OK;
                t.last_err[0] = '\0';
                ESP_LOGI(TAG, "push ok: %s -> ntfy %s/%s (HTTP %d)",
                         job.ip, s.server, s.topic, http_code);
                break;
            }
            // 4xx → no retry, save resources.
            if (http_code >= 400 && http_code < 500) {
                t.status = IP_ANN_STATUS_FAILED;
                ESP_LOGW(TAG, "push 4xx no-retry: %s", t.last_err);
                break;
            }
            ESP_LOGW(TAG, "push attempt %d failed: %s",
                     attempt + 1, t.last_err);
            if (attempt < PUSH_RETRIES - 1) {
                vTaskDelay(pdMS_TO_TICKS(PUSH_RETRY_GAP_MS));
            }
        }
        if (t.status != IP_ANN_STATUS_OK) {
            t.status = IP_ANN_STATUS_FAILED;
            ESP_LOGW(TAG, "push final failure: %s", t.last_err);
        }
        ip_announcer_priv_set_telemetry(&t);
    }
}

esp_err_t ip_announcer_push_init(void)
{
    s_push_q = xQueueCreate(PUSH_QUEUE_DEPTH, sizeof(push_job_t));
    if (!s_push_q) return ESP_ERR_NO_MEM;
    BaseType_t ok = xTaskCreate(push_task, "ip_ann_push", 6144, NULL, 2,
                                &s_push_task);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t ip_announcer_priv_enqueue_push(const char *ip)
{
    if (!ip || !s_push_q) return ESP_ERR_INVALID_STATE;
    push_job_t job;
    snprintf(job.ip, sizeof(job.ip), "%s", ip);
    return xQueueSend(s_push_q, &job, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}
