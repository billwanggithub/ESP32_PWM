#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_console.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "driver/uart.h"
#include "argtable3/argtable3.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sdkconfig.h"
#include "pwm_gen.h"
#include "rpm_cap.h"
#include "app_api.h"
#include "usb_composite.h"
#include "net_dashboard.h"
#include "ota_core.h"

static const char *TAG = "app";

// ---- CLI: pwm <freq> <duty> -------------------------------------------------

static struct {
    struct arg_int *freq;
    struct arg_dbl *duty;
    struct arg_end *end;
} s_pwm_args;

static int cmd_pwm(int argc, char **argv)
{
    int n = arg_parse(argc, argv, (void **)&s_pwm_args);
    if (n != 0) { arg_print_errors(stderr, s_pwm_args.end, argv[0]); return 1; }
    ctrl_cmd_t c = {
        .kind = CTRL_CMD_SET_PWM,
        .set_pwm = {
            .freq_hz  = (uint32_t)s_pwm_args.freq->ival[0],
            .duty_pct = (float)s_pwm_args.duty->dval[0],
        },
    };
    return control_task_post(&c, pdMS_TO_TICKS(100)) == ESP_OK ? 0 : 1;
}

// ---- CLI: rpm_params <pole> <mavg> -----------------------------------------

static struct {
    struct arg_int *pole;
    struct arg_int *mavg;
    struct arg_end *end;
} s_rpmparm_args;

static int cmd_rpm_params(int argc, char **argv)
{
    int n = arg_parse(argc, argv, (void **)&s_rpmparm_args);
    if (n != 0) { arg_print_errors(stderr, s_rpmparm_args.end, argv[0]); return 1; }
    ctrl_cmd_t c = {
        .kind = CTRL_CMD_SET_RPM_PARAMS,
        .set_rpm_params = {
            .pole = (uint8_t)s_rpmparm_args.pole->ival[0],
            .mavg = (uint16_t)s_rpmparm_args.mavg->ival[0],
        },
    };
    return control_task_post(&c, pdMS_TO_TICKS(100)) == ESP_OK ? 0 : 1;
}

// ---- CLI: rpm_timeout <us> --------------------------------------------------

static struct {
    struct arg_int *us;
    struct arg_end *end;
} s_rpmto_args;

static int cmd_rpm_timeout(int argc, char **argv)
{
    int n = arg_parse(argc, argv, (void **)&s_rpmto_args);
    if (n != 0) { arg_print_errors(stderr, s_rpmto_args.end, argv[0]); return 1; }
    ctrl_cmd_t c = {
        .kind = CTRL_CMD_SET_RPM_TIMEOUT,
        .set_rpm_timeout = { .timeout_us = (uint32_t)s_rpmto_args.us->ival[0] },
    };
    return control_task_post(&c, pdMS_TO_TICKS(100)) == ESP_OK ? 0 : 1;
}

// ---- CLI: status -----------------------------------------------------------

static int cmd_status(int argc, char **argv)
{
    uint32_t f; float d;
    control_task_get_pwm(&f, &d);
    float rpm = rpm_cap_get_latest();
    printf("pwm  freq=%lu Hz  duty=%.2f %%  (duty resolution %u bits)\n",
           (unsigned long)f, d, pwm_gen_duty_resolution_bits(f));
    printf("rpm  latest=%.2f\n", rpm);
    return 0;
}

static void register_commands(void)
{
    s_pwm_args.freq = arg_int1(NULL, NULL, "<freq_hz>",  "PWM frequency in Hz (1..1000000)");
    s_pwm_args.duty = arg_dbl1(NULL, NULL, "<duty_pct>", "Duty cycle in percent (0..100)");
    s_pwm_args.end  = arg_end(2);
    const esp_console_cmd_t pwm_cmd = {
        .command = "pwm", .help = "set PWM frequency and duty",
        .hint = NULL, .func = cmd_pwm, .argtable = &s_pwm_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&pwm_cmd));

    s_rpmparm_args.pole = arg_int1(NULL, NULL, "<pole>", "motor pole count");
    s_rpmparm_args.mavg = arg_int1(NULL, NULL, "<mavg>", "moving-average window");
    s_rpmparm_args.end  = arg_end(2);
    const esp_console_cmd_t rp_cmd = {
        .command = "rpm_params", .help = "set RPM pole count and moving average",
        .hint = NULL, .func = cmd_rpm_params, .argtable = &s_rpmparm_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&rp_cmd));

    s_rpmto_args.us  = arg_int1(NULL, NULL, "<us>", "RPM timeout microseconds (edge→0 RPM)");
    s_rpmto_args.end = arg_end(1);
    const esp_console_cmd_t rt_cmd = {
        .command = "rpm_timeout", .help = "set RPM timeout in microseconds",
        .hint = NULL, .func = cmd_rpm_timeout, .argtable = &s_rpmto_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&rt_cmd));

    const esp_console_cmd_t st_cmd = {
        .command = "status", .help = "print PWM + RPM snapshot",
        .hint = NULL, .func = cmd_status,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&st_cmd));

    ESP_ERROR_CHECK(esp_console_register_help_command());
}

static void start_console(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "esp32-pwm> ";
    repl_cfg.max_cmdline_length = 256;

    esp_console_dev_uart_config_t uart_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_cfg, &repl_cfg, &repl));

    register_commands();
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

void app_main(void)
{
    ESP_LOGI(TAG, "boot: ESP32-S3 PWM + RPM capture");

    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(nvs);
    }

    pwm_gen_config_t pwm_cfg = {
        .pwm_gpio     = CONFIG_APP_PWM_OUTPUT_GPIO,
        .trigger_gpio = CONFIG_APP_PWM_TRIGGER_GPIO,
    };
    ESP_ERROR_CHECK(pwm_gen_init(&pwm_cfg));

    rpm_cap_config_t rpm_cfg = {
        .input_gpio       = CONFIG_APP_RPM_INPUT_GPIO,
        .pole_count       = CONFIG_APP_DEFAULT_POLE_COUNT,
        .moving_avg_count = CONFIG_APP_DEFAULT_MAVG_COUNT,
        .rpm_timeout_us   = CONFIG_APP_DEFAULT_RPM_TIMEOUT_US,
    };
    ESP_ERROR_CHECK(rpm_cap_init(&rpm_cfg));

    ESP_ERROR_CHECK(ota_core_init());
    ESP_ERROR_CHECK(control_task_start());

    // USB composite on the native USB2 port (GPIO19/20). Requires the
    // board's USB-OTG 0 Ω jumper to be bridged.
    esp_err_t usb_err = usb_composite_start();
    if (usb_err != ESP_OK) {
        ESP_LOGW(TAG, "USB composite init failed: %s (is the USB-OTG jumper bridged?)",
                 esp_err_to_name(usb_err));
    }

    // Wi-Fi provisioning + dashboard HTTP/WS server.
    esp_err_t net_err = net_dashboard_start();
    if (net_err != ESP_OK) {
        ESP_LOGW(TAG, "net_dashboard init failed: %s", esp_err_to_name(net_err));
    }

    // Boot-health check: if we got this far (Wi-Fi up *or* USB reachable),
    // cancel rollback so the bootloader doesn't revert on next reset.
    ota_core_mark_current_image_valid();

    start_console();
}
