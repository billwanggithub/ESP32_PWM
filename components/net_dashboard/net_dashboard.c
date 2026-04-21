#include "net_dashboard.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "ota_core.h"

esp_err_t provisioning_run_and_connect(void);
void      ws_register(httpd_handle_t server);
void      ws_on_client_closed(httpd_handle_t hd, int fd);

static const char *TAG = "dashboard";

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
    cfg.max_uri_handlers = 8;
    cfg.close_fn = ws_on_client_closed;
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));

    httpd_uri_t root = { .uri = "/",        .method = HTTP_GET,  .handler = root_get };
    httpd_uri_t js   = { .uri = "/app.js",  .method = HTTP_GET,  .handler = js_get   };
    httpd_uri_t css  = { .uri = "/app.css", .method = HTTP_GET,  .handler = css_get  };
    httpd_uri_t ota  = { .uri = "/ota",     .method = HTTP_POST, .handler = ota_post };
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &js);
    httpd_register_uri_handler(server, &css);
    httpd_register_uri_handler(server, &ota);
    ws_register(server);
    return server;
}

esp_err_t net_dashboard_start(void)
{
    esp_err_t e = provisioning_run_and_connect();
    if (e != ESP_OK) return e;
    httpd_handle_t s = start_http();
    ESP_LOGI(TAG, "dashboard http server up: %p", s);
    return ESP_OK;
}
