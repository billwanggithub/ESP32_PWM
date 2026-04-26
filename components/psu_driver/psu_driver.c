#include "psu_driver.h"
#include "psu_backend.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_log.h"
#include "driver/uart.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "psu_driver";

#define PSU_UART_PORT     UART_NUM_1
#define PSU_RX_BUF_SIZE   256
#define PSU_TX_BUF_SIZE   256

#define NVS_NAMESPACE     "psu_driver"
#define NVS_KEY_SLAVE     "slave_addr"
#define NVS_KEY_FAMILY    "family"

#define POLL_PERIOD_MS       200
#define LINK_FAIL_THRESHOLD  5

// ---- shared atomic state -------------------------------------------------
static _Atomic uint8_t  s_slave_addr;
static _Atomic uint32_t s_v_set_bits, s_i_set_bits, s_v_out_bits, s_i_out_bits;
static _Atomic uint8_t  s_output_on;
static _Atomic uint8_t  s_link_ok;
static _Atomic uint16_t s_model_id;
static _Atomic uint32_t s_i_scale_bits;
static _Atomic uint32_t s_i_max_bits;
static const char *_Atomic s_model_name = "unknown";
static _Atomic int  s_link_fails;

// ---- backend dispatch ----------------------------------------------------
static const psu_backend_t *s_backend = &psu_backend_riden;

// ---- handles -------------------------------------------------------------
static SemaphoreHandle_t s_uart_mutex;
static TaskHandle_t      s_psu_task;

// ---- bit-pun helpers -----------------------------------------------------
static inline void store_f(_Atomic uint32_t *slot, float v)
{
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    atomic_store_explicit(slot, bits, memory_order_relaxed);
}
static inline float load_f(_Atomic uint32_t *slot)
{
    uint32_t bits = atomic_load_explicit(slot, memory_order_relaxed);
    float v;
    memcpy(&v, &bits, sizeof(v));
    return v;
}

// ---- publish helpers (called by backends) --------------------------------
SemaphoreHandle_t psu_driver_priv_get_uart_mutex(void) { return s_uart_mutex; }
uint8_t psu_driver_priv_get_slave(void) {
    return atomic_load_explicit(&s_slave_addr, memory_order_relaxed);
}
void psu_driver_priv_publish_v_set(float v) { store_f(&s_v_set_bits, v); }
void psu_driver_priv_publish_i_set(float i) { store_f(&s_i_set_bits, i); }
void psu_driver_priv_publish_v_out(float v) { store_f(&s_v_out_bits, v); }
void psu_driver_priv_publish_i_out(float i) { store_f(&s_i_out_bits, i); }
void psu_driver_priv_publish_output(bool on) {
    atomic_store_explicit(&s_output_on, on ? 1 : 0, memory_order_relaxed);
}
void psu_driver_priv_publish_model(uint16_t id, const char *name,
                                   float i_scale_div, float i_max)
{
    atomic_store_explicit(&s_model_id, id, memory_order_relaxed);
    atomic_store_explicit(&s_model_name, name, memory_order_relaxed);
    store_f(&s_i_scale_bits, i_scale_div);
    store_f(&s_i_max_bits,   i_max);
}

void psu_driver_priv_note_txn_result(esp_err_t e)
{
    if (e == ESP_OK) {
        int prev = atomic_exchange_explicit(&s_link_fails, 0, memory_order_relaxed);
        if (prev >= LINK_FAIL_THRESHOLD) ESP_LOGI(TAG, "link recovered");
        atomic_store_explicit(&s_link_ok, 1, memory_order_relaxed);
    } else {
        int cur = atomic_load_explicit(&s_link_fails, memory_order_relaxed);
        while (cur < LINK_FAIL_THRESHOLD) {
            if (atomic_compare_exchange_weak_explicit(&s_link_fails, &cur, cur + 1,
                    memory_order_relaxed, memory_order_relaxed)) {
                if (cur + 1 == LINK_FAIL_THRESHOLD) {
                    ESP_LOGW(TAG, "link lost: %s", esp_err_to_name(e));
                    atomic_store_explicit(&s_link_ok, 0, memory_order_relaxed);
                }
                break;
            }
        }
    }
}

// ---- NVS -----------------------------------------------------------------
static const psu_backend_t *resolve_backend_by_name(const char *name)
{
    if (!name) return &psu_backend_riden;
    if (strcmp(name, "riden")    == 0) return &psu_backend_riden;
    if (strcmp(name, "xy_sk120") == 0) return &psu_backend_xy_sk120;
    if (strcmp(name, "wz5005")   == 0) return &psu_backend_wz5005;
    return &psu_backend_riden;
}

static void load_nvs_state(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        atomic_store_explicit(&s_slave_addr,
                              (uint8_t)CONFIG_APP_PSU_SLAVE_DEFAULT,
                              memory_order_relaxed);
        s_backend = &psu_backend_riden;
        return;
    }
    uint8_t v = (uint8_t)CONFIG_APP_PSU_SLAVE_DEFAULT;
    (void)nvs_get_u8(h, NVS_KEY_SLAVE, &v);
    if (v < 1) v = (uint8_t)CONFIG_APP_PSU_SLAVE_DEFAULT;
    atomic_store_explicit(&s_slave_addr, v, memory_order_relaxed);

    char name[16] = {0};
    size_t n = sizeof(name);
    if (nvs_get_str(h, NVS_KEY_FAMILY, name, &n) != ESP_OK) {
        s_backend = &psu_backend_riden;
    } else {
        s_backend = resolve_backend_by_name(name);
    }
    nvs_close(h);
}

static esp_err_t save_slave_to_nvs(uint8_t v)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    nvs_set_u8(h, NVS_KEY_SLAVE, v);
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

static esp_err_t save_family_to_nvs(const char *name)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    nvs_set_str(h, NVS_KEY_FAMILY, name);
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

// ---- polling task --------------------------------------------------------
static void psu_task_fn(void *arg)
{
    (void)arg;
    s_backend->detect();

    const TickType_t period = pdMS_TO_TICKS(POLL_PERIOD_MS);
    TickType_t last = xTaskGetTickCount();
    while (true) {
        if (atomic_load_explicit(&s_model_id, memory_order_relaxed) == 0 &&
            atomic_load_explicit(&s_link_ok,  memory_order_relaxed) == 1) {
            s_backend->detect();
        }
        s_backend->poll();
        vTaskDelayUntil(&last, period);
    }
}

// ---- public API ----------------------------------------------------------
esp_err_t psu_driver_init(void)
{
    s_uart_mutex = xSemaphoreCreateMutex();
    if (!s_uart_mutex) return ESP_ERR_NO_MEM;

    // pre-detect defaults so get_telemetry() before first detect returns sane values
    store_f(&s_i_scale_bits, 1000.0f);
    store_f(&s_i_max_bits,   6.0f);

    load_nvs_state();

    const uart_config_t cfg = {
        .baud_rate  = CONFIG_APP_PSU_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t e = uart_driver_install(PSU_UART_PORT, PSU_RX_BUF_SIZE,
                                      PSU_TX_BUF_SIZE, 0, NULL, 0);
    if (e != ESP_OK) { ESP_LOGE(TAG, "driver_install: %s", esp_err_to_name(e)); return e; }
    e = uart_param_config(PSU_UART_PORT, &cfg);
    if (e != ESP_OK) { ESP_LOGE(TAG, "param_config: %s", esp_err_to_name(e));  return e; }
    e = uart_set_pin(PSU_UART_PORT,
                     CONFIG_APP_PSU_UART_TX_GPIO,
                     CONFIG_APP_PSU_UART_RX_GPIO,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (e != ESP_OK) { ESP_LOGE(TAG, "set_pin: %s", esp_err_to_name(e));       return e; }

    ESP_LOGI(TAG, "UART1 ready: family=%s tx=%d rx=%d baud=%d slave=%u",
             s_backend->name,
             CONFIG_APP_PSU_UART_TX_GPIO, CONFIG_APP_PSU_UART_RX_GPIO,
             CONFIG_APP_PSU_UART_BAUD,
             psu_driver_priv_get_slave());
    if (CONFIG_APP_PSU_UART_BAUD != s_backend->default_baud) {
        ESP_LOGW(TAG, "baud %d differs from family default %d — confirm panel-set rate",
                 CONFIG_APP_PSU_UART_BAUD, s_backend->default_baud);
    }
    return ESP_OK;
}

esp_err_t psu_driver_start(void)
{
    if (s_psu_task) return ESP_ERR_INVALID_STATE;
    BaseType_t ok = xTaskCreate(psu_task_fn, "psu_driver", 4096, NULL, 4, &s_psu_task);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t psu_driver_set_voltage(float v) { return s_backend->set_voltage(v); }
esp_err_t psu_driver_set_current(float i) { return s_backend->set_current(i); }
esp_err_t psu_driver_set_output(bool on)  { return s_backend->set_output(on); }

uint8_t psu_driver_get_slave_addr(void) { return psu_driver_priv_get_slave(); }

esp_err_t psu_driver_set_slave_addr(uint8_t addr)
{
    if (addr < 1) return ESP_ERR_INVALID_ARG;
    atomic_store_explicit(&s_slave_addr, addr, memory_order_relaxed);
    save_slave_to_nvs(addr);
    ESP_LOGI(TAG, "slave addr set to %u (NVS)", addr);
    return ESP_OK;
}

void psu_driver_get_telemetry(psu_driver_telemetry_t *out)
{
    if (!out) return;
    out->v_set       = load_f(&s_v_set_bits);
    out->i_set       = load_f(&s_i_set_bits);
    out->v_out       = load_f(&s_v_out_bits);
    out->i_out       = load_f(&s_i_out_bits);
    out->output_on   = atomic_load_explicit(&s_output_on, memory_order_relaxed) != 0;
    out->link_ok     = atomic_load_explicit(&s_link_ok,   memory_order_relaxed) != 0;
    out->model_id    = atomic_load_explicit(&s_model_id,  memory_order_relaxed);
    out->i_scale_div = load_f(&s_i_scale_bits);
}

const char *psu_driver_get_model_name(void)
{
    return atomic_load_explicit(&s_model_name, memory_order_relaxed);
}

float psu_driver_get_i_max(void) { return load_f(&s_i_max_bits); }

const char *psu_driver_get_family(void) { return s_backend->name; }

esp_err_t psu_driver_set_family(const char *name)
{
    if (!name) return ESP_ERR_INVALID_ARG;
    if (strcmp(name, "riden")    != 0 &&
        strcmp(name, "xy_sk120") != 0 &&
        strcmp(name, "wz5005")   != 0) return ESP_ERR_INVALID_ARG;
    esp_err_t e = save_family_to_nvs(name);
    if (e == ESP_OK) ESP_LOGI(TAG, "family set to %s — reboot to apply", name);
    return e;
}
