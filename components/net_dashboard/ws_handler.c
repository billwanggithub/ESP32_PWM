#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <unistd.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_api.h"
#include "pwm_gen.h"
#include "rpm_cap.h"
#include "net_dashboard.h"

static const char *TAG = "ws";

#define MAX_CLIENTS 4
static int s_client_fds[MAX_CLIENTS];
static httpd_handle_t s_httpd_for_telemetry;

static void add_client(int fd)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (s_client_fds[i] == 0) { s_client_fds[i] = fd; return; }
}
static void remove_client(int fd)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (s_client_fds[i] == fd) s_client_fds[i] = 0;
}

static void ws_send_json_to(int fd, const char *payload)
{
    httpd_ws_frame_t f = {
        .type    = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)payload,
        .len     = strlen(payload),
    };
    httpd_ws_send_frame_async(s_httpd_for_telemetry, fd, &f);
}

static void handle_json(cJSON *root, int fd)
{
    const cJSON *type_j = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type_j)) return;

    if (strcmp(type_j->valuestring, "set_pwm") == 0) {
        const cJSON *f = cJSON_GetObjectItem(root, "freq");
        const cJSON *d = cJSON_GetObjectItem(root, "duty");
        if (!cJSON_IsNumber(f) || !cJSON_IsNumber(d)) return;
        ctrl_cmd_t c = {
            .kind = CTRL_CMD_SET_PWM,
            .set_pwm = { .freq_hz = (uint32_t)f->valuedouble, .duty_pct = (float)d->valuedouble },
        };
        control_task_post(&c, 0);
    } else if (strcmp(type_j->valuestring, "set_rpm") == 0) {
        const cJSON *pole = cJSON_GetObjectItem(root, "pole");
        const cJSON *mavg = cJSON_GetObjectItem(root, "mavg");
        const cJSON *to   = cJSON_GetObjectItem(root, "timeout_us");
        if (cJSON_IsNumber(pole) && cJSON_IsNumber(mavg)) {
            ctrl_cmd_t c1 = {
                .kind = CTRL_CMD_SET_RPM_PARAMS,
                .set_rpm_params = { .pole = (uint8_t)pole->valuedouble,
                                    .mavg = (uint16_t)mavg->valuedouble },
            };
            control_task_post(&c1, 0);
        }
        if (cJSON_IsNumber(to)) {
            ctrl_cmd_t c2 = {
                .kind = CTRL_CMD_SET_RPM_TIMEOUT,
                .set_rpm_timeout = { .timeout_us = (uint32_t)to->valuedouble },
            };
            control_task_post(&c2, 0);
        }
    } else if (strcmp(type_j->valuestring, "factory_reset") == 0) {
        ESP_LOGW(TAG, "factory_reset requested via ws fd=%d", fd);
        ws_send_json_to(fd, "{\"type\":\"ack\",\"op\":\"factory_reset\"}");
        net_dashboard_factory_reset();
    }
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "ws connected, fd=%d", httpd_req_to_sockfd(req));
        s_httpd_for_telemetry = req->handle;
        add_client(httpd_req_to_sockfd(req));
        return ESP_OK;
    }

    httpd_ws_frame_t frame = { .type = HTTPD_WS_TYPE_TEXT };
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) return err;
    if (frame.len == 0) return ESP_OK;

    uint8_t buf[512];
    if (frame.len >= sizeof(buf)) return ESP_ERR_NO_MEM;
    frame.payload = buf;
    err = httpd_ws_recv_frame(req, &frame, frame.len);
    if (err != ESP_OK) return err;
    buf[frame.len] = 0;

    cJSON *root = cJSON_Parse((char *)buf);
    if (root) {
        handle_json(root, httpd_req_to_sockfd(req));
        cJSON_Delete(root);
    }
    return ESP_OK;
}

static void telemetry_task(void *arg)
{
    TickType_t last = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(50);  // 20 Hz to the browser
    while (true) {
        vTaskDelayUntil(&last, period);
        if (!s_httpd_for_telemetry) continue;

        uint32_t f; float d;
        control_task_get_pwm(&f, &d);
        float rpm = rpm_cap_get_latest();
        char payload[160];
        int64_t ts = (int64_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
        snprintf(payload, sizeof(payload),
                 "{\"type\":\"status\",\"freq\":%lu,\"duty\":%.2f,\"rpm\":%.2f,\"ts\":%" PRId64 "}",
                 (unsigned long)f, d, rpm, ts);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (s_client_fds[i] != 0) ws_send_json_to(s_client_fds[i], payload);
        }
    }
}

void ws_register(httpd_handle_t server)
{
    httpd_uri_t ws = {
        .uri      = "/ws",
        .method   = HTTP_GET,
        .handler  = ws_handler,
        .is_websocket = true,
        .supported_subprotocol = NULL,
    };
    httpd_register_uri_handler(server, &ws);
    xTaskCreate(telemetry_task, "telemetry", 4096, NULL, 2, NULL);
}

// httpd close_fn: prune from our client table, then close the socket
// (the server stops closing it itself once a close_fn is registered).
void ws_on_client_closed(httpd_handle_t hd, int fd)
{
    (void)hd;
    remove_client(fd);
    close(fd);
}
