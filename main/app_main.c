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
#include "gpio_io.h"
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

// ---- CLI: gpio_mode <idx> <mode> -------------------------------------------

static struct {
    struct arg_int *idx;
    struct arg_str *mode;
    struct arg_end *end;
} s_gpio_mode_args;

static int cmd_gpio_mode(int argc, char **argv)
{
    int n = arg_parse(argc, argv, (void **)&s_gpio_mode_args);
    if (n != 0) { arg_print_errors(stderr, s_gpio_mode_args.end, argv[0]); return 1; }
    const char *m = s_gpio_mode_args.mode->sval[0];
    uint8_t mode;
    if      (strcmp(m, "i_pd") == 0) mode = 0;
    else if (strcmp(m, "i_pu") == 0) mode = 1;
    else if (strcmp(m, "i_fl") == 0) mode = 2;
    else if (strcmp(m, "o")    == 0) mode = 3;
    else { printf("mode must be i_pd | i_pu | i_fl | o\n"); return 1; }
    ctrl_cmd_t c = {
        .kind = CTRL_CMD_GPIO_SET_MODE,
        .gpio_set_mode = { .idx = (uint8_t)s_gpio_mode_args.idx->ival[0], .mode = mode },
    };
    return control_task_post(&c, pdMS_TO_TICKS(100)) == ESP_OK ? 0 : 1;
}

// ---- CLI: gpio_set <idx> <0|1> ---------------------------------------------

static struct {
    struct arg_int *idx;
    struct arg_int *level;
    struct arg_end *end;
} s_gpio_set_args;

static int cmd_gpio_set(int argc, char **argv)
{
    int n = arg_parse(argc, argv, (void **)&s_gpio_set_args);
    if (n != 0) { arg_print_errors(stderr, s_gpio_set_args.end, argv[0]); return 1; }
    ctrl_cmd_t c = {
        .kind = CTRL_CMD_GPIO_SET_LEVEL,
        .gpio_set_level = {
            .idx   = (uint8_t)s_gpio_set_args.idx->ival[0],
            .level = (uint8_t)(s_gpio_set_args.level->ival[0] ? 1 : 0),
        },
    };
    return control_task_post(&c, pdMS_TO_TICKS(100)) == ESP_OK ? 0 : 1;
}

// ---- CLI: gpio_pulse <idx> [width_ms] --------------------------------------

static struct {
    struct arg_int *idx;
    struct arg_int *width;
    struct arg_end *end;
} s_gpio_pulse_args;

static int cmd_gpio_pulse(int argc, char **argv)
{
    int n = arg_parse(argc, argv, (void **)&s_gpio_pulse_args);
    if (n != 0) { arg_print_errors(stderr, s_gpio_pulse_args.end, argv[0]); return 1; }
    uint32_t w = (s_gpio_pulse_args.width->count > 0)
                 ? (uint32_t)s_gpio_pulse_args.width->ival[0]
                 : gpio_io_get_pulse_width_ms();
    ctrl_cmd_t c = {
        .kind = CTRL_CMD_GPIO_PULSE,
        .gpio_pulse = { .idx = (uint8_t)s_gpio_pulse_args.idx->ival[0], .width_ms = w },
    };
    return control_task_post(&c, pdMS_TO_TICKS(100)) == ESP_OK ? 0 : 1;
}

// ---- CLI: power <0|1> ------------------------------------------------------

static struct {
    struct arg_int *on;
    struct arg_end *end;
} s_power_args;

static int cmd_power(int argc, char **argv)
{
    int n = arg_parse(argc, argv, (void **)&s_power_args);
    if (n != 0) { arg_print_errors(stderr, s_power_args.end, argv[0]); return 1; }
    ctrl_cmd_t c = {
        .kind = CTRL_CMD_POWER_SET,
        .power_set = { .on = (uint8_t)(s_power_args.on->ival[0] ? 1 : 0) },
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

    static const char *mode_str[] = { "i_pd", "i_pu", "i_fl", "o" };
    gpio_io_state_t st[GPIO_IO_PIN_COUNT];
    gpio_io_get_all(st);
    printf("power %s\n", gpio_io_get_power() ? "on" : "off");
    printf("gpio  ");
    for (int i = 0; i < GPIO_IO_PIN_COUNT; i++) {
        const char *grp = (i < 8) ? "A" : "B";
        int slot = (i < 8) ? (i + 1) : (i - 7);
        printf("%s%d=%s:%d%s ", grp, slot, mode_str[st[i].mode],
               st[i].level ? 1 : 0, st[i].pulsing ? "*" : "");
    }
    printf("(pulse_width=%lums)\n", (unsigned long)gpio_io_get_pulse_width_ms());
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

    s_gpio_mode_args.idx  = arg_int1(NULL, NULL, "<idx>",  "GPIO index 0..15");
    s_gpio_mode_args.mode = arg_str1(NULL, NULL, "<mode>", "i_pd|i_pu|i_fl|o");
    s_gpio_mode_args.end  = arg_end(2);
    const esp_console_cmd_t gm_cmd = { .command = "gpio_mode", .help = "set GPIO mode",
        .hint = NULL, .func = cmd_gpio_mode, .argtable = &s_gpio_mode_args };
    ESP_ERROR_CHECK(esp_console_cmd_register(&gm_cmd));

    s_gpio_set_args.idx   = arg_int1(NULL, NULL, "<idx>",   "GPIO index 0..15");
    s_gpio_set_args.level = arg_int1(NULL, NULL, "<level>", "0 or 1");
    s_gpio_set_args.end   = arg_end(2);
    const esp_console_cmd_t gs_cmd = { .command = "gpio_set", .help = "set GPIO output level",
        .hint = NULL, .func = cmd_gpio_set, .argtable = &s_gpio_set_args };
    ESP_ERROR_CHECK(esp_console_cmd_register(&gs_cmd));

    s_gpio_pulse_args.idx   = arg_int1(NULL, NULL, "<idx>",          "GPIO index 0..15");
    s_gpio_pulse_args.width = arg_int0(NULL, NULL, "[width_ms]",     "pulse width override (default global)");
    s_gpio_pulse_args.end   = arg_end(2);
    const esp_console_cmd_t gp_cmd = { .command = "gpio_pulse", .help = "one-shot idle-inverted pulse",
        .hint = NULL, .func = cmd_gpio_pulse, .argtable = &s_gpio_pulse_args };
    ESP_ERROR_CHECK(esp_console_cmd_register(&gp_cmd));

    s_power_args.on  = arg_int1(NULL, NULL, "<on>", "1 = ON, 0 = OFF");
    s_power_args.end = arg_end(1);
    const esp_console_cmd_t pw_cmd = { .command = "power", .help = "power switch ON/OFF",
        .hint = NULL, .func = cmd_power, .argtable = &s_power_args };
    ESP_ERROR_CHECK(esp_console_cmd_register(&pw_cmd));

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
    repl_cfg.prompt = "fan-testkit> ";
    repl_cfg.max_cmdline_length = 256;

    esp_console_dev_uart_config_t uart_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_cfg, &repl_cfg, &repl));

    register_commands();
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

void app_main(void)
{
    ESP_LOGI(TAG, "boot: Fan-TestKit (ESP32-S3 PWM + RPM capture)");

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

    ESP_ERROR_CHECK(gpio_io_init());

    ESP_ERROR_CHECK(ota_core_init());
    ESP_ERROR_CHECK(control_task_start());

    // Drive the default setpoint through the same path every later command
    // uses (pwm_gen_set → publish_pwm). This makes the published atomics, the
    // hardware, and downstream telemetry/HID status frames all reflect the
    // same boot default — so a duty-only command from the dashboard before any
    // freq change carries a real freq, not zero.
    {
        ctrl_cmd_t boot_pwm = {
            .kind    = CTRL_CMD_SET_PWM,
            .set_pwm = { .freq_hz = 10000u, .duty_pct = 0.0f },
        };
        control_task_post(&boot_pwm, pdMS_TO_TICKS(100));
    }

    // Boot the power switch OFF through the queue so the published state and
    // the hardware reflect the same value. gpio_io_init already configured
    // the pin as output and drove it OFF, but going through control_task here
    // keeps the user-visible toggle on the same single-handler path every
    // later set_power command uses.
    {
        ctrl_cmd_t boot_pwr = {
            .kind = CTRL_CMD_POWER_SET,
            .power_set = { .on = 0 },
        };
        control_task_post(&boot_pwr, pdMS_TO_TICKS(100));
    }

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
