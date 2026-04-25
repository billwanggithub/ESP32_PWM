#include "net_dashboard.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota_core.h"
#include "prov_internal.h"
#include "cJSON.h"
#include "pwm_gen.h"
#include "sdkconfig.h"

esp_err_t provisioning_run_and_connect(void);
void      ws_register(httpd_handle_t server);
void      ws_on_client_closed(httpd_handle_t hd, int fd);

static const char *TAG = "dashboard";

// GPIO0 is the BOOT strapping pin — sampled at reset to select
// flash/download mode, but free for application use afterwards. Physical
// pull-up on the YD board means idle=1, pressed=0.
#define BOOT_BUTTON_GPIO           0
#define BOOT_LONGPRESS_MS          3000
#define BOOT_POLL_MS               50

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");
extern const char app_js_start[]     asm("_binary_app_js_start");
extern const char app_js_end[]       asm("_binary_app_js_end");
extern const char app_css_start[]    asm("_binary_app_css_start");
extern const char app_css_end[]      asm("_binary_app_css_end");

static esp_err_t serve_embedded(httpd_req_t *req, const char *ct,
                                const char *start, const char *end)
{
    httpd_resp_set_type(req, ct);
    // EMBED_TXTFILES appends a trailing '\0' after the real content so the
    // symbols form a C string; that NUL is *before* _end, so subtract one
    // byte, otherwise browsers see a stray NUL and JS/CSS parsers choke.
    return httpd_resp_send(req, start, end - start - 1);
}

static esp_err_t root_get(httpd_req_t *req)
{ return serve_embedded(req, "text/html; charset=utf-8", index_html_start, index_html_end); }
static esp_err_t js_get(httpd_req_t *req)
{ return serve_embedded(req, "application/javascript", app_js_start, app_js_end); }
static esp_err_t css_get(httpd_req_t *req)
{ return serve_embedded(req, "text/css", app_css_start, app_css_end); }

// One-shot static device config for the dashboard's Help panel. Returned as
// JSON. Fields are compile-time constants (Kconfig + pwm_gen header), so the
// handler is allocation-bounded and never blocks.
static esp_err_t device_info_get(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_OK;
    }

    cJSON *pins     = cJSON_AddObjectToObject(root, "pins");
    cJSON *defaults = cJSON_AddObjectToObject(root, "defaults");
    if (!pins || !defaults) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_OK;
    }

    cJSON_AddNumberToObject(pins, "pwm",        CONFIG_APP_PWM_OUTPUT_GPIO);
    cJSON_AddNumberToObject(pins, "trigger",    CONFIG_APP_PWM_TRIGGER_GPIO);
    cJSON_AddNumberToObject(pins, "rpm",        CONFIG_APP_RPM_INPUT_GPIO);
    cJSON_AddNumberToObject(pins, "status_led", CONFIG_APP_STATUS_LED_GPIO);
    cJSON_AddNumberToObject(pins, "power_switch", CONFIG_APP_POWER_SWITCH_GPIO);
    cJSON *gpio_a = cJSON_AddArrayToObject(pins, "group_a");
    cJSON *gpio_b = cJSON_AddArrayToObject(pins, "group_b");
    if (gpio_a) {
        cJSON_AddItemToArray(gpio_a, cJSON_CreateNumber(CONFIG_APP_GPIO_GROUP_A_PIN_0));
        cJSON_AddItemToArray(gpio_a, cJSON_CreateNumber(CONFIG_APP_GPIO_GROUP_A_PIN_1));
        cJSON_AddItemToArray(gpio_a, cJSON_CreateNumber(CONFIG_APP_GPIO_GROUP_A_PIN_2));
        cJSON_AddItemToArray(gpio_a, cJSON_CreateNumber(CONFIG_APP_GPIO_GROUP_A_PIN_3));
        cJSON_AddItemToArray(gpio_a, cJSON_CreateNumber(CONFIG_APP_GPIO_GROUP_A_PIN_4));
        cJSON_AddItemToArray(gpio_a, cJSON_CreateNumber(CONFIG_APP_GPIO_GROUP_A_PIN_5));
        cJSON_AddItemToArray(gpio_a, cJSON_CreateNumber(CONFIG_APP_GPIO_GROUP_A_PIN_6));
        cJSON_AddItemToArray(gpio_a, cJSON_CreateNumber(CONFIG_APP_GPIO_GROUP_A_PIN_7));
    }
    if (gpio_b) {
        cJSON_AddItemToArray(gpio_b, cJSON_CreateNumber(CONFIG_APP_GPIO_GROUP_B_PIN_0));
        cJSON_AddItemToArray(gpio_b, cJSON_CreateNumber(CONFIG_APP_GPIO_GROUP_B_PIN_1));
        cJSON_AddItemToArray(gpio_b, cJSON_CreateNumber(CONFIG_APP_GPIO_GROUP_B_PIN_2));
        cJSON_AddItemToArray(gpio_b, cJSON_CreateNumber(CONFIG_APP_GPIO_GROUP_B_PIN_3));
        cJSON_AddItemToArray(gpio_b, cJSON_CreateNumber(CONFIG_APP_GPIO_GROUP_B_PIN_4));
        cJSON_AddItemToArray(gpio_b, cJSON_CreateNumber(CONFIG_APP_GPIO_GROUP_B_PIN_5));
        cJSON_AddItemToArray(gpio_b, cJSON_CreateNumber(CONFIG_APP_GPIO_GROUP_B_PIN_6));
        cJSON_AddItemToArray(gpio_b, cJSON_CreateNumber(CONFIG_APP_GPIO_GROUP_B_PIN_7));
    }

    cJSON_AddNumberToObject(defaults, "pole_count",     CONFIG_APP_DEFAULT_POLE_COUNT);
    cJSON_AddNumberToObject(defaults, "mavg_count",     CONFIG_APP_DEFAULT_MAVG_COUNT);
    cJSON_AddNumberToObject(defaults, "rpm_timeout_us", CONFIG_APP_DEFAULT_RPM_TIMEOUT_US);

    cJSON_AddNumberToObject(root, "freq_hz_min", PWM_GEN_FREQ_MIN_HZ);
    cJSON_AddNumberToObject(root, "freq_hz_max", PWM_GEN_FREQ_MAX_HZ);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "render");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_sendstr(req, body);
    cJSON_free(body);
    return e;
}

static esp_err_t ota_post(httpd_req_t *req)
{
    esp_err_t e = ota_core_begin((uint32_t)req->content_len);
    if (e != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(e));
        return ESP_OK;
    }
    uint8_t buf[1024];
    int remaining = req->content_len;
    while (remaining > 0) {
        int to_read = remaining > (int)sizeof(buf) ? (int)sizeof(buf) : remaining;
        int n = httpd_req_recv(req, (char *)buf, to_read);
        if (n <= 0) { ota_core_abort(); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv"); return ESP_OK; }
        if (ota_core_write(buf, (size_t)n) != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write");
            return ESP_OK;
        }
        remaining -= n;
    }
    httpd_resp_sendstr(req, "OK, rebooting");
    ota_core_end_and_reboot();   // does not return
    return ESP_OK;
}

static httpd_handle_t start_http(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 9;  // root, js, css, ota, device_info, ws, +headroom
    cfg.close_fn = ws_on_client_closed;
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));

    httpd_uri_t root = { .uri = "/",        .method = HTTP_GET,  .handler = root_get };
    httpd_uri_t js   = { .uri = "/app.js",  .method = HTTP_GET,  .handler = js_get   };
    httpd_uri_t css  = { .uri = "/app.css", .method = HTTP_GET,  .handler = css_get  };
    httpd_uri_t ota  = { .uri = "/ota",     .method = HTTP_POST, .handler = ota_post };
    httpd_uri_t info = { .uri = "/api/device_info", .method = HTTP_GET, .handler = device_info_get };
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &js);
    httpd_register_uri_handler(server, &css);
    httpd_register_uri_handler(server, &ota);
    httpd_register_uri_handler(server, &info);
    ws_register(server);
    return server;
}

static void factory_reset_task(void *arg)
{
    // Short delay lets any caller-side ack (ws frame, HID IN report, CDC
    // status frame) flush before the restart interrupts those transports.
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGW(TAG, "factory reset: clearing credentials and restarting");
    prov_clear_credentials();
    esp_restart();
}

void net_dashboard_factory_reset(void)
{
    // Idempotent: if a task is already running we don't need a second.
    static volatile bool s_triggered = false;
    if (s_triggered) return;
    s_triggered = true;
    xTaskCreate(factory_reset_task, "fact_rst", 4096, NULL, 5, NULL);
}

static void boot_button_task(void *arg)
{
    // Hold BOOT ≥3 s → factory reset. Short presses are ignored so a casual
    // BOOT press during dev work doesn't nuke credentials.
    uint32_t held_ms = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(BOOT_POLL_MS));
        if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
            held_ms += BOOT_POLL_MS;
            if (held_ms >= BOOT_LONGPRESS_MS) {
                ESP_LOGW(TAG, "BOOT long-press detected → factory reset");
                net_dashboard_factory_reset();
                // factory_reset_task will restart the chip; we can just spin.
                vTaskSuspend(NULL);
            }
        } else {
            held_ms = 0;
        }
    }
}

static void start_boot_button_watcher(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,    // board has hw pull-up too; redundant but safe
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    xTaskCreate(boot_button_task, "boot_btn", 2048, NULL, 2, NULL);
}

esp_err_t net_dashboard_start(void)
{
    esp_err_t e = provisioning_run_and_connect();
    if (e != ESP_OK) return e;
    httpd_handle_t s = start_http();
    ESP_LOGI(TAG, "dashboard http server up: %p", s);
    start_boot_button_watcher();
    return ESP_OK;
}
