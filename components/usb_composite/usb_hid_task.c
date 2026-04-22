#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tinyusb.h"
#include "tusb.h"
#include "class/hid/hid_device.h"

#include "usb_protocol.h"
#include "app_api.h"
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
