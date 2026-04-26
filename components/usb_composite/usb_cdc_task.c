#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"

#include "tinyusb.h"
#include "tusb.h"
#include "tusb_cdc_acm.h"

#include "usb_protocol.h"
#include "app_api.h"
#include "gpio_io.h"
#include "psu_driver.h"
#include "ota_core.h"
#include "net_dashboard.h"
#include "ip_announcer.h"

static const char *TAG = "usb_cdc";

#define CDC_LOG_RB_BYTES  4096

static RingbufHandle_t   s_log_rb;
static vprintf_like_t    s_prev_vprintf;
static SemaphoreHandle_t s_cdc_tx_mutex;

// ESP_LOG redirect: copy to the ring buffer, also fall through to UART.
static int cdc_vprintf(const char *fmt, va_list ap)
{
    char buf[256];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n > 0 && s_log_rb) {
        size_t to_send = (n > (int)sizeof(buf)) ? sizeof(buf) : (size_t)n;
        xRingbufferSend(s_log_rb, buf, to_send, 0);
    }
    return s_prev_vprintf ? s_prev_vprintf(fmt, ap) : vprintf(fmt, ap);
}

// Write bytes to CDC with SLIP-framed op 0x01 (log).
static void cdc_send_log_frame(const uint8_t *data, size_t len)
{
    if (s_cdc_tx_mutex && xSemaphoreTake(s_cdc_tx_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    uint8_t end = USB_CDC_SLIP_END;
    uint8_t op  = USB_CDC_OP_LOG;
    tud_cdc_write(&end, 1);
    tud_cdc_write(&op, 1);
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        if (b == USB_CDC_SLIP_END) {
            uint8_t esc[2] = { USB_CDC_SLIP_ESC, USB_CDC_SLIP_ESC_END };
            tud_cdc_write(esc, 2);
        } else if (b == USB_CDC_SLIP_ESC) {
            uint8_t esc[2] = { USB_CDC_SLIP_ESC, USB_CDC_SLIP_ESC_ESC };
            tud_cdc_write(esc, 2);
        } else {
            tud_cdc_write(&b, 1);
        }
    }
    tud_cdc_write(&end, 1);
    tud_cdc_write_flush();
    if (s_cdc_tx_mutex) xSemaphoreGive(s_cdc_tx_mutex);
}

// ---- SLIP parser for incoming OTA frames -----------------------------------

typedef enum { PARSE_NORMAL, PARSE_ESC } parse_state_t;

typedef struct {
    parse_state_t state;
    uint8_t       buf[1100];     // max OTA chunk payload + header
    size_t        len;
    bool          in_frame;
} slip_parser_t;

static void send_ota_status(uint8_t state, uint32_t progress, uint8_t error)
{
    if (s_cdc_tx_mutex && xSemaphoreTake(s_cdc_tx_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    usb_cdc_ota_status_t st = { .state = state, .progress = progress, .error = error };
    uint8_t end = USB_CDC_SLIP_END;
    uint8_t st_op = USB_CDC_OP_OTA_STATUS;
    tud_cdc_write(&end, 1);
    tud_cdc_write(&st_op, 1);
    tud_cdc_write(&st, sizeof(st));
    tud_cdc_write(&end, 1);
    tud_cdc_write_flush();
    if (s_cdc_tx_mutex) xSemaphoreGive(s_cdc_tx_mutex);
}

static void handle_frame(const uint8_t *data, size_t len)
{
    if (len < 1) return;
    uint8_t op = data[0];
    const uint8_t *payload = data + 1;
    size_t plen = len - 1;

    switch (op) {
    case USB_CDC_OP_OTA_BEGIN: {
        if (plen < sizeof(usb_cdc_ota_begin_t)) { send_ota_status(OTA_STATE_ERROR, 0, 1); break; }
        usb_cdc_ota_begin_t b; memcpy(&b, payload, sizeof(b));
        esp_err_t e = ota_core_begin(b.total_size);
        send_ota_status(e == ESP_OK ? OTA_STATE_WRITING : OTA_STATE_ERROR, 0, e == ESP_OK ? 0 : 2);
    } break;
    case USB_CDC_OP_OTA_CHUNK: {
        if (plen < sizeof(usb_cdc_ota_chunk_hdr_t)) break;
        usb_cdc_ota_chunk_hdr_t h; memcpy(&h, payload, sizeof(h));
        const uint8_t *chunk = payload + sizeof(h);
        size_t chunk_len = plen - sizeof(h);
        esp_err_t e = ota_core_write(chunk, chunk_len);
        if (e != ESP_OK) send_ota_status(OTA_STATE_ERROR, ota_core_progress(), 3);
        else             send_ota_status(OTA_STATE_WRITING, ota_core_progress(), 0);
    } break;
    case USB_CDC_OP_OTA_END: {
        send_ota_status(OTA_STATE_DONE, ota_core_progress(), 0);
        ota_core_end_and_reboot();   // does not return on success
    } break;
    case USB_CDC_OP_GPIO_SET_MODE: {
        if (plen < 2) break;
        ctrl_cmd_t c = {
            .kind = CTRL_CMD_GPIO_SET_MODE,
            .gpio_set_mode = { .idx = payload[0], .mode = payload[1] },
        };
        control_task_post(&c, 0);
    } break;
    case USB_CDC_OP_GPIO_SET_LEVEL: {
        if (plen < 2) break;
        ctrl_cmd_t c = {
            .kind = CTRL_CMD_GPIO_SET_LEVEL,
            .gpio_set_level = { .idx = payload[0], .level = payload[1] ? 1u : 0u },
        };
        control_task_post(&c, 0);
    } break;
    case USB_CDC_OP_GPIO_PULSE: {
        if (plen < 3) break;
        uint16_t width = (uint16_t)payload[1] | ((uint16_t)payload[2] << 8);
        ctrl_cmd_t c = {
            .kind = CTRL_CMD_GPIO_PULSE,
            .gpio_pulse = { .idx = payload[0], .width_ms = width },
        };
        control_task_post(&c, 0);
    } break;
    case USB_CDC_OP_POWER: {
        if (plen < 1) break;
        ctrl_cmd_t c = {
            .kind = CTRL_CMD_POWER_SET,
            .power_set = { .on = payload[0] ? 1u : 0u },
        };
        control_task_post(&c, 0);
    } break;
    case USB_CDC_OP_PULSE_WIDTH_SET: {
        if (plen < 2) break;
        uint16_t w = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
        ctrl_cmd_t c = {
            .kind = CTRL_CMD_PULSE_WIDTH_SET,
            .pulse_width_set = { .width_ms = w },
        };
        control_task_post(&c, 0);
    } break;
    case USB_CDC_OP_FACTORY_RESET: {
        // Magic byte guards against stray frames — 1-byte payload with 0xA5.
        if (plen < 1 || payload[0] != USB_CDC_FACTORY_RESET_MAGIC) {
            ESP_LOGW(TAG, "factory_reset CDC frame with bad magic; ignored");
            break;
        }
        // Send empty-payload ack frame so host knows the reset is committing.
        if (s_cdc_tx_mutex && xSemaphoreTake(s_cdc_tx_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            uint8_t end = USB_CDC_SLIP_END;
            uint8_t ack_op = USB_CDC_OP_FACTORY_ACK;
            tud_cdc_write(&end, 1);
            tud_cdc_write(&ack_op, 1);
            tud_cdc_write(&end, 1);
            tud_cdc_write_flush();
            xSemaphoreGive(s_cdc_tx_mutex);
        }
        ESP_LOGW(TAG, "factory_reset requested via CDC");
        net_dashboard_factory_reset();
    } break;
    case USB_CDC_OP_PSU_SET_VOLTAGE: {
        if (plen < 4) break;
        ctrl_cmd_t c = { .kind = CTRL_CMD_PSU_SET_VOLTAGE };
        memcpy(&c.psu_set_voltage.v, payload, 4);
        control_task_post(&c, 0);
    } break;
    case USB_CDC_OP_PSU_SET_CURRENT: {
        if (plen < 4) break;
        ctrl_cmd_t c = { .kind = CTRL_CMD_PSU_SET_CURRENT };
        memcpy(&c.psu_set_current.i, payload, 4);
        control_task_post(&c, 0);
    } break;
    case USB_CDC_OP_PSU_SET_OUTPUT: {
        if (plen < 1) break;
        ctrl_cmd_t c = {
            .kind = CTRL_CMD_PSU_SET_OUTPUT,
            .psu_set_output = { .on = payload[0] ? 1u : 0u },
        };
        control_task_post(&c, 0);
    } break;
    case USB_CDC_OP_PSU_SET_SLAVE: {
        if (plen < 2) break;
        if (payload[1] != USB_CDC_PSU_SLAVE_MAGIC) break;
        if (payload[0] < 1 || payload[0] > 247)    break;
        ctrl_cmd_t c = {
            .kind = CTRL_CMD_PSU_SET_SLAVE,
            .psu_set_slave = { .addr = payload[0] },
        };
        control_task_post(&c, 0);
    } break;
    case USB_CDC_OP_SAVE_RPM_PARAMS: {
        ctrl_cmd_t c = { .kind = CTRL_CMD_SAVE_RPM_PARAMS };
        control_task_post(&c, 0);
    } break;
    case USB_CDC_OP_SAVE_RPM_TIMEOUT: {
        ctrl_cmd_t c = { .kind = CTRL_CMD_SAVE_RPM_TIMEOUT };
        control_task_post(&c, 0);
    } break;
    case USB_CDC_OP_SAVE_PWM_FREQ: {
        ctrl_cmd_t c = { .kind = CTRL_CMD_SAVE_PWM_FREQ };
        control_task_post(&c, 0);
    } break;
    case USB_CDC_OP_SAVE_UI_STEPS: {
        if (plen < 6) break;  // 4 B float + 2 B u16
        float    duty_step;
        uint16_t freq_step;
        memcpy(&duty_step, &payload[0], 4);
        memcpy(&freq_step, &payload[4], 2);
        ctrl_cmd_t c = {
            .kind = CTRL_CMD_SAVE_UI_STEPS,
            .save_ui_steps = { .duty_step = duty_step, .freq_step = freq_step },
        };
        control_task_post(&c, 0);
    } break;
    case USB_CDC_OP_ANNOUNCER_SET: {
        // payload: u8 enable, u8 priority, str topic\0 str server\0
        if (plen < 4) break;
        const uint8_t *p = payload;
        uint8_t en  = p[0];
        uint8_t pri = p[1];
        const char *topic  = (const char *)(p + 2);
        size_t topic_n = strnlen(topic, plen - 2);
        if (topic_n + 2 + 1 >= plen) break;
        const char *server = topic + topic_n + 1;
        size_t server_max  = plen - (topic_n + 3);  // bytes available after topic\0
        size_t server_n    = strnlen(server, server_max);
        if (server_n == server_max) {
            // Missing trailing NUL — malformed frame, drop.
            break;
        }

        ctrl_cmd_t c = {
            .kind = CTRL_CMD_ANNOUNCER_SET,
            .announcer_set = { .enable = en, .priority = pri },
        };
        strncpy(c.announcer_set.topic, topic,
                sizeof(c.announcer_set.topic) - 1);
        c.announcer_set.topic[sizeof(c.announcer_set.topic) - 1] = '\0';
        strncpy(c.announcer_set.server, server,
                sizeof(c.announcer_set.server) - 1);
        c.announcer_set.server[sizeof(c.announcer_set.server) - 1] = '\0';
        control_task_post(&c, 0);
    } break;
    case USB_CDC_OP_ANNOUNCER_TEST: {
        ctrl_cmd_t c = { .kind = CTRL_CMD_ANNOUNCER_TEST };
        control_task_post(&c, 0);
    } break;
    default:
        ESP_LOGW(TAG, "unknown CDC op 0x%02x", op);
        break;
    }
}

static void slip_feed(slip_parser_t *p, const uint8_t *data, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t b = data[i];
        if (b == USB_CDC_SLIP_END) {
            if (p->in_frame && p->len > 0) {
                handle_frame(p->buf, p->len);
            }
            p->len = 0;
            p->in_frame = true;
            p->state = PARSE_NORMAL;
            continue;
        }
        if (!p->in_frame) continue;
        if (p->state == PARSE_ESC) {
            if (b == USB_CDC_SLIP_ESC_END)      b = USB_CDC_SLIP_END;
            else if (b == USB_CDC_SLIP_ESC_ESC) b = USB_CDC_SLIP_ESC;
            p->state = PARSE_NORMAL;
        } else if (b == USB_CDC_SLIP_ESC) {
            p->state = PARSE_ESC;
            continue;
        }
        if (p->len < sizeof(p->buf)) p->buf[p->len++] = b;
        else {
            p->in_frame = false;
            p->len = 0;
        }
    }
}

// ---- Tasks ------------------------------------------------------------------

static void cdc_tx_task(void *arg)
{
    while (true) {
        size_t item_len = 0;
        uint8_t *chunk = xRingbufferReceive(s_log_rb, &item_len, pdMS_TO_TICKS(20));
        if (chunk) {
            if (tud_cdc_connected()) {
                cdc_send_log_frame(chunk, item_len);
            }
            vRingbufferReturnItem(s_log_rb, chunk);
        }
    }
}

static void cdc_rx_task(void *arg)
{
    static slip_parser_t parser;
    uint8_t buf[64];
    while (true) {
        if (tud_cdc_available()) {
            uint32_t n = tud_cdc_read(buf, sizeof(buf));
            if (n > 0) slip_feed(&parser, buf, n);
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

// ---- PSU telemetry push (op 0x44 @ 5 Hz) -----------------------------------

// SLIP-escaped writer used for both the op byte and the float payload.
// Caller holds s_cdc_tx_mutex.
static void cdc_write_byte_escaped(uint8_t b)
{
    if (b == USB_CDC_SLIP_END) {
        uint8_t esc[2] = { USB_CDC_SLIP_ESC, USB_CDC_SLIP_ESC_END };
        tud_cdc_write(esc, 2);
    } else if (b == USB_CDC_SLIP_ESC) {
        uint8_t esc[2] = { USB_CDC_SLIP_ESC, USB_CDC_SLIP_ESC_ESC };
        tud_cdc_write(esc, 2);
    } else {
        tud_cdc_write(&b, 1);
    }
}

static void cdc_psu_telemetry_task(void *arg)
{
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(200);   // 5 Hz
    TickType_t last = xTaskGetTickCount();
    while (true) {
        vTaskDelayUntil(&last, period);
        if (!tud_cdc_connected()) continue;

        psu_driver_telemetry_t pt;
        psu_driver_get_telemetry(&pt);

        // Frame body: [op][v_set LE][i_set LE][v_out LE][i_out LE][flags]
        // 17 payload bytes (4×float + 1×u8). Op byte adds 1 = 18 byte body.
        uint8_t body[1 /*op*/ + 4*4 /*floats*/ + 1 /*flags*/];
        body[0]  = USB_CDC_OP_PSU_TELEMETRY;
        memcpy(&body[1],  &pt.v_set, 4);
        memcpy(&body[5],  &pt.i_set, 4);
        memcpy(&body[9],  &pt.v_out, 4);
        memcpy(&body[13], &pt.i_out, 4);
        body[17] = (uint8_t)((pt.output_on ? 0x01u : 0u) | (pt.link_ok ? 0x02u : 0u));

        if (!s_cdc_tx_mutex) continue;
        if (xSemaphoreTake(s_cdc_tx_mutex, pdMS_TO_TICKS(50)) != pdTRUE) continue;

        uint8_t end = USB_CDC_SLIP_END;
        tud_cdc_write(&end, 1);
        // Op byte is 0x44 — never collides with SLIP_END/ESC, but pass through
        // the escaping helper for consistency.
        cdc_write_byte_escaped(body[0]);
        for (size_t i = 1; i < sizeof(body); i++) {
            cdc_write_byte_escaped(body[i]);
        }
        tud_cdc_write(&end, 1);
        tud_cdc_write_flush();

        // ---- Announcer telemetry mirror (op 0x62, 5 Hz) ---------------------
        // Build NUL-delimited variable-length frame:
        //   [op][enable][priority][status][http_lo][http_hi]
        //   [last_pushed_ip\0][topic\0][server\0][last_err\0]
        ip_announcer_settings_t  ann_s;
        ip_announcer_telemetry_t ann_t;
        ip_announcer_get_settings(&ann_s);
        ip_announcer_get_telemetry(&ann_t);

        uint8_t abuf[256];
        size_t  an = 0;
        bool    ann_ok = true;
        abuf[an++] = USB_CDC_OP_ANNOUNCER_TELEMETRY;
        abuf[an++] = ann_s.enable ? 1 : 0;
        abuf[an++] = ann_s.priority;
        abuf[an++] = (uint8_t)ann_t.status;
        abuf[an++] = (uint8_t)(ann_t.last_http_code & 0xff);
        abuf[an++] = (uint8_t)((ann_t.last_http_code >> 8) & 0xff);

        // C-strings: last_pushed_ip, topic, server, last_err
        const char *ann_strs[4] = {
            ann_t.last_pushed_ip,
            ann_s.topic,
            ann_s.server,
            ann_t.last_err,
        };
        for (int si = 0; si < 4 && ann_ok; si++) {
            size_t l = strlen(ann_strs[si]) + 1;
            if (an + l > sizeof(abuf)) { ann_ok = false; break; }
            memcpy(abuf + an, ann_strs[si], l);
            an += l;
        }

        if (ann_ok) {
            tud_cdc_write(&end, 1);
            for (size_t i = 0; i < an; i++) {
                cdc_write_byte_escaped(abuf[i]);
            }
            tud_cdc_write(&end, 1);
            tud_cdc_write_flush();
        }

        xSemaphoreGive(s_cdc_tx_mutex);
    }
}

void usb_cdc_task_start(void)
{
    s_log_rb = xRingbufferCreate(CDC_LOG_RB_BYTES, RINGBUF_TYPE_BYTEBUF);
    s_cdc_tx_mutex = xSemaphoreCreateMutex();
    s_prev_vprintf = esp_log_set_vprintf(cdc_vprintf);
    xTaskCreate(cdc_tx_task,            "usb_cdc_tx",  4096, NULL, 2, NULL);
    xTaskCreate(cdc_rx_task,            "usb_cdc_rx",  4096, NULL, 2, NULL);
    xTaskCreate(cdc_psu_telemetry_task, "cdc_psu_tel", 3072, NULL, 2, NULL);
}
