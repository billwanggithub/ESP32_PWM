#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tinyusb.h"
#include "tusb.h"
#include "class/hid/hid_device.h"

#include "usb_protocol.h"
#include "app_api.h"
#include "gpio_io.h"
#include "pwm_gen.h"
#include "rpm_cap.h"
#include "net_dashboard.h"

static const char *TAG = "usb_hid";

// 50 Hz status push from telemetry cadence.
#define STATUS_PERIOD_MS 20

static uint32_t s_status_seq;

// TinyUSB callback: device received HID OUT report from host.
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    (void)instance;
    if (report_type != HID_REPORT_TYPE_OUTPUT) return;

    switch (report_id) {
    case USB_HID_REPORT_SET_PWM: {
        if (bufsize < sizeof(usb_hid_set_pwm_t)) return;
        usb_hid_set_pwm_t p;
        memcpy(&p, buffer, sizeof(p));
        ctrl_cmd_t c = {
            .kind    = CTRL_CMD_SET_PWM,
            .set_pwm = { .freq_hz = p.freq_hz, .duty_pct = p.duty_pct },
        };
        control_task_post(&c, 0);
    } break;
    case USB_HID_REPORT_SET_RPM: {
        if (bufsize < sizeof(usb_hid_set_rpm_t)) return;
        usb_hid_set_rpm_t p;
        memcpy(&p, buffer, sizeof(p));
        ctrl_cmd_t c1 = {
            .kind = CTRL_CMD_SET_RPM_PARAMS,
            .set_rpm_params = { .pole = p.pole_count, .mavg = p.moving_avg_count },
        };
        ctrl_cmd_t c2 = {
            .kind = CTRL_CMD_SET_RPM_TIMEOUT,
            .set_rpm_timeout = { .timeout_us = p.timeout_us },
        };
        control_task_post(&c1, 0);
        control_task_post(&c2, 0);
    } break;
    case USB_HID_REPORT_FACTORY_RESET: {
        // Magic byte guards against a stray report triggering a wipe.
        if (bufsize < 1 || buffer[0] != USB_HID_FACTORY_RESET_MAGIC) {
            ESP_LOGW(TAG, "factory_reset HID report with bad magic; ignored");
            return;
        }
        ESP_LOGW(TAG, "factory_reset requested via HID");
        net_dashboard_factory_reset();
    } break;
    case USB_HID_REPORT_GPIO: {
        if (bufsize < 4) return;
        uint8_t op = buffer[0];
        switch (op) {
        case USB_HID_GPIO_OP_SET_MODE: {
            ctrl_cmd_t c = {
                .kind = CTRL_CMD_GPIO_SET_MODE,
                .gpio_set_mode = { .idx = buffer[1], .mode = buffer[2] },
            };
            control_task_post(&c, 0);
        } break;
        case USB_HID_GPIO_OP_SET_LEVEL: {
            ctrl_cmd_t c = {
                .kind = CTRL_CMD_GPIO_SET_LEVEL,
                .gpio_set_level = { .idx = buffer[1], .level = buffer[2] ? 1u : 0u },
            };
            control_task_post(&c, 0);
        } break;
        case USB_HID_GPIO_OP_PULSE: {
            uint16_t width = (uint16_t)buffer[2] | ((uint16_t)buffer[3] << 8);
            ctrl_cmd_t c = {
                .kind = CTRL_CMD_GPIO_PULSE,
                .gpio_pulse = { .idx = buffer[1], .width_ms = width },
            };
            control_task_post(&c, 0);
        } break;
        case USB_HID_GPIO_OP_POWER: {
            ctrl_cmd_t c = {
                .kind = CTRL_CMD_POWER_SET,
                .power_set = { .on = buffer[1] ? 1u : 0u },
            };
            control_task_post(&c, 0);
        } break;
        default:
            ESP_LOGW(TAG, "unknown gpio op 0x%02x", op);
            break;
        }
    } break;
    case USB_HID_REPORT_PSU: {
        if (bufsize < 1) return;
        uint8_t op = buffer[0];
        ctrl_cmd_t cmd = {0};
        bool ok = false;
        switch (op) {
        case USB_HID_PSU_OP_SET_VOLTAGE:
            if (bufsize < 5) break;
            memcpy(&cmd.psu_set_voltage.v, &buffer[1], 4);
            cmd.kind = CTRL_CMD_PSU_SET_VOLTAGE;
            ok = true;
            break;
        case USB_HID_PSU_OP_SET_CURRENT:
            if (bufsize < 5) break;
            memcpy(&cmd.psu_set_current.i, &buffer[1], 4);
            cmd.kind = CTRL_CMD_PSU_SET_CURRENT;
            ok = true;
            break;
        case USB_HID_PSU_OP_SET_OUTPUT:
            if (bufsize < 2) break;
            cmd.kind = CTRL_CMD_PSU_SET_OUTPUT;
            cmd.psu_set_output.on = buffer[1] ? 1 : 0;
            ok = true;
            break;
        case USB_HID_PSU_OP_SET_SLAVE:
            if (bufsize < 6) break;
            if (buffer[5] != USB_HID_PSU_SLAVE_MAGIC) break;
            if (buffer[1] < 1 || buffer[1] > 247) break;
            cmd.kind = CTRL_CMD_PSU_SET_SLAVE;
            cmd.psu_set_slave.addr = buffer[1];
            ok = true;
            break;
        default:
            ESP_LOGW(TAG, "unknown psu op 0x%02x", op);
            break;
        }
        if (ok) control_task_post(&cmd, 0);
    } break;
    case USB_HID_REPORT_SETTINGS_SAVE: {
        if (bufsize < 8) break;
        uint8_t op = buffer[0];
        switch (op) {
        case USB_HID_SAVE_OP_PWM_FREQ: {
            ctrl_cmd_t c = { .kind = CTRL_CMD_SAVE_PWM_FREQ };
            control_task_post(&c, 0);
        } break;
        case USB_HID_SAVE_OP_RPM_PARAMS: {
            ctrl_cmd_t c = { .kind = CTRL_CMD_SAVE_RPM_PARAMS };
            control_task_post(&c, 0);
        } break;
        case USB_HID_SAVE_OP_RPM_TIMEOUT: {
            ctrl_cmd_t c = { .kind = CTRL_CMD_SAVE_RPM_TIMEOUT };
            control_task_post(&c, 0);
        } break;
        case USB_HID_SAVE_OP_UI_STEPS: {
            usb_hid_settings_save_steps_t p;
            if (bufsize < sizeof(p)) break;
            memcpy(&p, buffer, sizeof(p));
            ctrl_cmd_t c = {
                .kind = CTRL_CMD_SAVE_UI_STEPS,
                .save_ui_steps = {
                    .duty_step = (float)p.duty_step_x100 / 100.0f,
                    .freq_step = p.freq_step,
                },
            };
            control_task_post(&c, 0);
        } break;
        default:
            ESP_LOGW(TAG, "unknown settings_save op 0x%02x", op);
            break;
        }
    } break;
    default:
        ESP_LOGW(TAG, "unknown OUT report id 0x%02x", report_id);
        break;
    }
}

// TinyUSB callback: host requested a GET_REPORT (rare; we push IN reports).
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    (void)instance; (void)report_type; (void)buffer; (void)reqlen; (void)report_id;
    return 0;
}

static void send_status_once(void)
{
    if (!tud_hid_ready()) return;
    usb_hid_status_t s;
    uint32_t f; float d;
    control_task_get_pwm(&f, &d);
    s.freq_hz  = f;
    s.duty_pct = d;
    s.rpm      = rpm_cap_get_latest();
    s.seq      = ++s_status_seq;
    tud_hid_report(USB_HID_REPORT_STATUS, &s, sizeof(s));
}

static void hid_task(void *arg)
{
    const TickType_t period = pdMS_TO_TICKS(STATUS_PERIOD_MS);
    TickType_t last = xTaskGetTickCount();
    while (true) {
        vTaskDelayUntil(&last, period);
        if (tud_mounted()) send_status_once();
    }
}

void usb_hid_task_start(void)
{
    xTaskCreate(hid_task, "usb_hid", 3072, NULL, 3, NULL);
}
