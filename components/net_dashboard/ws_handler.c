#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <unistd.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_api.h"
#include "gpio_io.h"
#include "ip_announcer.h"
#include "psu_driver.h"
#include "pwm_gen.h"
#include "rpm_cap.h"
#include "ui_settings.h"
#include "net_dashboard.h"

static const char *TAG = "ws";

#define MAX_CLIENTS 4
// Drop a client after this many consecutive telemetry-frame send failures.
// Telemetry runs at 20 Hz, so 20 ≈ 1 s of nothing leaving the socket — long
// enough to ride out brief congestion, short enough that a vanished peer
// (laptop lid closed, phone screen off, AP roam) doesn't keep its slot.
#define FAIL_THRESHOLD 20
static int s_client_fds[MAX_CLIENTS];
static uint8_t s_client_fail[MAX_CLIENTS];
static httpd_handle_t s_httpd_for_telemetry;

static void add_client(int fd)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (s_client_fds[i] == 0) {
            s_client_fds[i]  = fd;
            s_client_fail[i] = 0;
            return;
        }
}
static void remove_client(int fd)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (s_client_fds[i] == fd) {
            s_client_fds[i]  = 0;
            s_client_fail[i] = 0;
        }
}

// Returns ESP_OK on success, or the underlying httpd error so the caller
// can age the fd's fail counter.
static esp_err_t ws_send_json_to(int fd, const char *payload)
{
    httpd_ws_frame_t f = {
        .type    = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)payload,
        .len     = strlen(payload),
    };
    return httpd_ws_send_frame_async(s_httpd_for_telemetry, fd, &f);
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
    } else if (strcmp(type_j->valuestring, "save_rpm_params") == 0) {
        ctrl_cmd_t c = { .kind = CTRL_CMD_SAVE_RPM_PARAMS };
        control_task_post(&c, 0);
        ws_send_json_to(fd, "{\"type\":\"ack\",\"op\":\"save_rpm_params\",\"ok\":true}");
    } else if (strcmp(type_j->valuestring, "save_rpm_timeout") == 0) {
        ctrl_cmd_t c = { .kind = CTRL_CMD_SAVE_RPM_TIMEOUT };
        control_task_post(&c, 0);
        ws_send_json_to(fd, "{\"type\":\"ack\",\"op\":\"save_rpm_timeout\",\"ok\":true}");
    } else if (strcmp(type_j->valuestring, "save_pwm_freq") == 0) {
        ctrl_cmd_t c = { .kind = CTRL_CMD_SAVE_PWM_FREQ };
        control_task_post(&c, 0);
        ws_send_json_to(fd, "{\"type\":\"ack\",\"op\":\"save_pwm_freq\",\"ok\":true}");
    } else if (strcmp(type_j->valuestring, "save_ui_steps") == 0) {
        const cJSON *ds = cJSON_GetObjectItem(root, "duty_step");
        const cJSON *fs = cJSON_GetObjectItem(root, "freq_step");
        if (cJSON_IsNumber(ds) && cJSON_IsNumber(fs)) {
            ctrl_cmd_t c = {
                .kind = CTRL_CMD_SAVE_UI_STEPS,
                .save_ui_steps = {
                    .duty_step = (float)ds->valuedouble,
                    .freq_step = (uint16_t)fs->valuedouble,
                },
            };
            control_task_post(&c, 0);
            ws_send_json_to(fd, "{\"type\":\"ack\",\"op\":\"save_ui_steps\",\"ok\":true}");
        } else {
            ws_send_json_to(fd, "{\"type\":\"ack\",\"op\":\"save_ui_steps\",\"ok\":false}");
        }
    } else if (strcmp(type_j->valuestring, "set_gpio_mode") == 0) {
        const cJSON *idx  = cJSON_GetObjectItem(root, "idx");
        const cJSON *mode = cJSON_GetObjectItem(root, "mode");
        if (!cJSON_IsNumber(idx) || !cJSON_IsString(mode)) return;
        const char *m = mode->valuestring;
        uint8_t mv;
        if      (strcmp(m, "input_pulldown") == 0) mv = 0;
        else if (strcmp(m, "input_pullup")   == 0) mv = 1;
        else if (strcmp(m, "input_floating") == 0) mv = 2;
        else if (strcmp(m, "output")         == 0) mv = 3;
        else return;
        ctrl_cmd_t c = {
            .kind = CTRL_CMD_GPIO_SET_MODE,
            .gpio_set_mode = { .idx = (uint8_t)idx->valuedouble, .mode = mv },
        };
        control_task_post(&c, 0);
    } else if (strcmp(type_j->valuestring, "set_gpio_level") == 0) {
        const cJSON *idx   = cJSON_GetObjectItem(root, "idx");
        const cJSON *level = cJSON_GetObjectItem(root, "level");
        if (!cJSON_IsNumber(idx) || !cJSON_IsNumber(level)) return;
        ctrl_cmd_t c = {
            .kind = CTRL_CMD_GPIO_SET_LEVEL,
            .gpio_set_level = {
                .idx   = (uint8_t)idx->valuedouble,
                .level = (level->valuedouble != 0) ? 1u : 0u,
            },
        };
        control_task_post(&c, 0);
    } else if (strcmp(type_j->valuestring, "pulse_gpio") == 0) {
        const cJSON *idx = cJSON_GetObjectItem(root, "idx");
        const cJSON *w   = cJSON_GetObjectItem(root, "width_ms");
        if (!cJSON_IsNumber(idx)) return;
        uint32_t width = cJSON_IsNumber(w) ? (uint32_t)w->valuedouble
                                           : gpio_io_get_pulse_width_ms();
        ctrl_cmd_t c = {
            .kind = CTRL_CMD_GPIO_PULSE,
            .gpio_pulse = { .idx = (uint8_t)idx->valuedouble, .width_ms = width },
        };
        control_task_post(&c, 0);
    } else if (strcmp(type_j->valuestring, "set_power") == 0) {
        const cJSON *on = cJSON_GetObjectItem(root, "on");
        if (!cJSON_IsBool(on) && !cJSON_IsNumber(on)) return;
        bool b = cJSON_IsBool(on) ? cJSON_IsTrue(on) : (on->valuedouble != 0);
        ctrl_cmd_t c = {
            .kind = CTRL_CMD_POWER_SET,
            .power_set = { .on = b ? 1u : 0u },
        };
        control_task_post(&c, 0);
    } else if (strcmp(type_j->valuestring, "set_pulse_width") == 0) {
        const cJSON *w = cJSON_GetObjectItem(root, "width_ms");
        if (!cJSON_IsNumber(w)) return;
        ctrl_cmd_t c = {
            .kind = CTRL_CMD_PULSE_WIDTH_SET,
            .pulse_width_set = { .width_ms = (uint32_t)w->valuedouble },
        };
        control_task_post(&c, 0);
    } else if (strcmp(type_j->valuestring, "set_psu_voltage") == 0) {
        const cJSON *v = cJSON_GetObjectItem(root, "v");
        if (!cJSON_IsNumber(v)) return;
        ctrl_cmd_t c = {
            .kind = CTRL_CMD_PSU_SET_VOLTAGE,
            .psu_set_voltage = { .v = (float)v->valuedouble },
        };
        control_task_post(&c, 0);
    } else if (strcmp(type_j->valuestring, "set_psu_current") == 0) {
        const cJSON *i = cJSON_GetObjectItem(root, "i");
        if (!cJSON_IsNumber(i)) return;
        ctrl_cmd_t c = {
            .kind = CTRL_CMD_PSU_SET_CURRENT,
            .psu_set_current = { .i = (float)i->valuedouble },
        };
        control_task_post(&c, 0);
    } else if (strcmp(type_j->valuestring, "set_psu_output") == 0) {
        const cJSON *on = cJSON_GetObjectItem(root, "on");
        if (!cJSON_IsBool(on) && !cJSON_IsNumber(on)) return;
        bool b = cJSON_IsBool(on) ? cJSON_IsTrue(on) : (on->valuedouble != 0);
        ctrl_cmd_t c = {
            .kind = CTRL_CMD_PSU_SET_OUTPUT,
            .psu_set_output = { .on = b ? 1u : 0u },
        };
        control_task_post(&c, 0);
    } else if (strcmp(type_j->valuestring, "set_psu_slave") == 0) {
        const cJSON *a = cJSON_GetObjectItem(root, "addr");
        if (!cJSON_IsNumber(a)) return;
        int v = (int)a->valuedouble;
        if (v < 1 || v > 247) return;
        ctrl_cmd_t c = {
            .kind = CTRL_CMD_PSU_SET_SLAVE,
            .psu_set_slave = { .addr = (uint8_t)v },
        };
        control_task_post(&c, 0);
    } else if (strcmp(type_j->valuestring, "set_psu_family") == 0) {
        const cJSON *fam = cJSON_GetObjectItem(root, "family");
        if (!cJSON_IsString(fam)) return;
        esp_err_t e = psu_driver_set_family(fam->valuestring);
        char ack[96];
        snprintf(ack, sizeof(ack),
                 "{\"type\":\"ack\",\"op\":\"set_psu_family\",\"ok\":%s}",
                 e == ESP_OK ? "true" : "false");
        ws_send_json_to(fd, ack);
    } else if (strcmp(type_j->valuestring, "set_announcer") == 0) {
        const cJSON *en = cJSON_GetObjectItem(root, "enable");
        const cJSON *tp = cJSON_GetObjectItem(root, "topic");
        const cJSON *sv = cJSON_GetObjectItem(root, "server");
        const cJSON *pr = cJSON_GetObjectItem(root, "priority");
        if (!cJSON_IsString(tp) || !cJSON_IsString(sv) || !cJSON_IsNumber(pr)) {
            ws_send_json_to(fd, "{\"type\":\"ack\",\"op\":\"set_announcer\",\"ok\":false}");
            return;
        }
        ctrl_cmd_t c = {
            .kind = CTRL_CMD_ANNOUNCER_SET,
            .announcer_set = {
                .enable   = cJSON_IsBool(en) ? (cJSON_IsTrue(en) ? 1 : 0)
                                              : (en && cJSON_IsNumber(en)
                                                 && en->valuedouble != 0 ? 1 : 0),
                .priority = (uint8_t)pr->valuedouble,
            },
        };
        strncpy(c.announcer_set.topic, tp->valuestring,
                sizeof(c.announcer_set.topic) - 1);
        c.announcer_set.topic[sizeof(c.announcer_set.topic) - 1] = '\0';
        strncpy(c.announcer_set.server, sv->valuestring,
                sizeof(c.announcer_set.server) - 1);
        c.announcer_set.server[sizeof(c.announcer_set.server) - 1] = '\0';
        control_task_post(&c, 0);
        ws_send_json_to(fd, "{\"type\":\"ack\",\"op\":\"set_announcer\",\"ok\":true}");
    } else if (strcmp(type_j->valuestring, "test_announcer") == 0) {
        ctrl_cmd_t c = { .kind = CTRL_CMD_ANNOUNCER_TEST };
        control_task_post(&c, 0);
        ws_send_json_to(fd, "{\"type\":\"ack\",\"op\":\"test_announcer\",\"ok\":true}");
    } else if (strcmp(type_j->valuestring, "reboot") == 0) {
        ESP_LOGW(TAG, "reboot requested via ws fd=%d", fd);
        ws_send_json_to(fd, "{\"type\":\"ack\",\"op\":\"reboot\"}");
        // 200 ms grace for the ack to flush, mirror of factory_reset
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
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
    char payload[768];
    static const char *mode_short[] = { "i_pd", "i_pu", "i_fl", "o" };
    while (true) {
        vTaskDelayUntil(&last, period);
        if (!s_httpd_for_telemetry) continue;

        uint32_t f; float d;
        control_task_get_pwm(&f, &d);
        float rpm = rpm_cap_get_latest();
        gpio_io_state_t st[GPIO_IO_PIN_COUNT];
        gpio_io_get_all(st);
        bool power = gpio_io_get_power();
        uint32_t pw = gpio_io_get_pulse_width_ms();

        int64_t ts = (int64_t)xTaskGetTickCount() * portTICK_PERIOD_MS;

        int n = snprintf(payload, sizeof(payload),
                "{\"type\":\"status\",\"freq\":%lu,\"duty\":%.2f,\"rpm\":%.2f,\"ts\":%" PRId64
                ",\"power\":%d,\"pulse_width_ms\":%lu,\"gpio\":[",
                (unsigned long)f, d, rpm, ts, power ? 1 : 0, (unsigned long)pw);

        for (int i = 0; i < GPIO_IO_PIN_COUNT && n < (int)sizeof(payload); i++) {
            n += snprintf(payload + n, sizeof(payload) - n,
                          "%s{\"m\":\"%s\",\"v\":%d,\"p\":%d}",
                          (i == 0) ? "" : ",",
                          mode_short[st[i].mode],
                          st[i].level ? 1 : 0,
                          st[i].pulsing ? 1 : 0);
        }
        if (n < (int)sizeof(payload)) {
            n += snprintf(payload + n, sizeof(payload) - n, "]");
        }

        psu_driver_telemetry_t pt;
        psu_driver_get_telemetry(&pt);
        if (n < (int)sizeof(payload)) {
            n += snprintf(payload + n, sizeof(payload) - n,
                ",\"psu\":{\"v_set\":%.2f,\"i_set\":%.3f,\"v_out\":%.2f,\"i_out\":%.3f,"
                "\"output\":%s,\"link\":%s,\"model\":\"%s\",\"slave\":%u,\"family\":\"%s\"}",
                (double)pt.v_set, (double)pt.i_set,
                (double)pt.v_out, (double)pt.i_out,
                pt.output_on ? "true" : "false",
                pt.link_ok   ? "true" : "false",
                psu_driver_get_model_name(),
                psu_driver_get_slave_addr(),
                psu_driver_get_family());
        }
        float ui_duty_step; uint16_t ui_freq_step;
        ui_settings_get_steps(&ui_duty_step, &ui_freq_step);
        if (n < (int)sizeof(payload)) {
            n += snprintf(payload + n, sizeof(payload) - n,
                ",\"ui\":{\"duty_step\":%.2f,\"freq_step\":%u}",
                (double)ui_duty_step, (unsigned)ui_freq_step);
        }
        ip_announcer_settings_t  ann_s;
        ip_announcer_telemetry_t ann_t;
        ip_announcer_get_settings(&ann_s);
        ip_announcer_get_telemetry(&ann_t);
        static const char *status_str[] = { "never", "ok", "failed", "disabled" };
        if (n < (int)sizeof(payload)) {
            n += snprintf(payload + n, sizeof(payload) - n,
                ",\"announcer\":{\"enable\":%s,\"topic\":\"%s\",\"server\":\"%s\","
                "\"priority\":%u,\"status\":\"%s\","
                "\"last_pushed_ip\":\"%s\",\"last_http_code\":%d,\"last_err\":\"%s\"}",
                ann_s.enable ? "true" : "false",
                ann_s.topic,
                ann_s.server,
                (unsigned)ann_s.priority,
                status_str[ann_t.status],
                ann_t.last_pushed_ip,
                ann_t.last_http_code,
                ann_t.last_err);
        }
        if (n < (int)sizeof(payload)) {
            n += snprintf(payload + n, sizeof(payload) - n, "}");
        }

        for (int i = 0; i < MAX_CLIENTS; i++) {
            int fd = s_client_fds[i];
            if (fd == 0) continue;
            esp_err_t err = ws_send_json_to(fd, payload);
            if (err == ESP_OK) {
                s_client_fail[i] = 0;
            } else if (++s_client_fail[i] >= FAIL_THRESHOLD) {
                // Peer is gone or stuck — ask httpd to tear the session
                // down. Its sess_delete path invokes ws_on_client_closed,
                // which clears the slot.
                ESP_LOGW(TAG, "ws fd=%d unresponsive, closing", fd);
                httpd_sess_trigger_close(s_httpd_for_telemetry, fd);
            }
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
