#include "captive_portal.h"

#include <string.h>
#include <stdio.h>

#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "captive";

static httpd_handle_t               s_httpd;
static captive_portal_creds_cb_t    s_cb;

extern const char setup_html_start[]   asm("_binary_setup_html_start");
extern const char setup_html_end[]     asm("_binary_setup_html_end");
extern const char success_html_start[] asm("_binary_success_html_start");
extern const char success_html_end[]   asm("_binary_success_html_end");

static esp_err_t send_embedded(httpd_req_t *req, const char *ct,
                               const char *start, const char *end)
{
    httpd_resp_set_type(req, ct);
    // EMBED_TXTFILES adds a trailing '\0'; don't include it in the body.
    return httpd_resp_send(req, start, end - start - 1);
}

static esp_err_t root_get(httpd_req_t *req)
{
    return send_embedded(req, "text/html; charset=utf-8",
                         setup_html_start, setup_html_end);
}

static esp_err_t scan_get(httpd_req_t *req)
{
    wifi_scan_config_t cfg = {0};
    esp_err_t e = esp_wifi_scan_start(&cfg, true);
    if (e != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(e));
        return ESP_OK;
    }

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 20) n = 20;
    wifi_ap_record_t aps[20];
    esp_wifi_scan_get_ap_records(&n, aps);

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", (const char *)aps[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", aps[i].rssi);
        cJSON_AddNumberToObject(o, "auth", aps[i].authmode);
        cJSON_AddItemToArray(arr, o);
    }
    char *s = cJSON_PrintUnformatted(arr);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, s);
    cJSON_free(s);
    cJSON_Delete(arr);
    return ESP_OK;
}

static esp_err_t save_wifi_post(httpd_req_t *req)
{
    if (req->content_len > 512) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "too large");
        return ESP_OK;
    }
    char body[513] = {0};
    int received = 0;
    int remaining = (int)req->content_len;
    while (remaining > 0) {
        int n = httpd_req_recv(req, body + received, remaining);
        if (n <= 0) {
            if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;  // retry transient timeout
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "read failed");
            return ESP_OK;
        }
        received += n;
        remaining -= n;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_OK;
    }
    const cJSON *jssid = cJSON_GetObjectItem(root, "ssid");
    const cJSON *jpw   = cJSON_GetObjectItem(root, "password");
    const char *ssid = cJSON_IsString(jssid) ? jssid->valuestring : NULL;
    const char *pw   = cJSON_IsString(jpw)   ? jpw->valuestring   : "";

    if (!ssid || !ssid[0] || strlen(ssid) >= 32 || strlen(pw) >= 64) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid ssid/password");
        return ESP_OK;
    }

    captive_portal_result_t out = {0};
    esp_err_t e = s_cb ? s_cb(ssid, pw, &out) : ESP_FAIL;
    cJSON_Delete(root);

    if (e == ESP_OK) {
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "ip", out.ip);
        cJSON_AddStringToObject(resp, "mdns", out.mdns ? out.mdns : "esp32-pwm.local");
        char *s = cJSON_PrintUnformatted(resp);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, s);
        cJSON_free(s);
        cJSON_Delete(resp);
        return ESP_OK;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "error", out.err_msg[0] ? out.err_msg : "connect failed");
    char *s = cJSON_PrintUnformatted(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, s);
    cJSON_free(s);
    cJSON_Delete(resp);
    return ESP_OK;
}

static esp_err_t success_get(httpd_req_t *req)
{
    // Pull current STA IP + mDNS and substitute into the template.
    // Rewrite into a stack buffer (2 KB) so concurrent /success requests
    // do not corrupt each other. Template is ~700 bytes after expansion.
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info = {0};
    if (sta) esp_netif_get_ip_info(sta, &info);
    char ip_buf[16];
    snprintf(ip_buf, sizeof(ip_buf), IPSTR, IP2STR(&info.ip));

    const size_t tpl_len = (size_t)(success_html_end - success_html_start - 1);
    char rendered[2048];
    if (tpl_len + 64 > sizeof(rendered)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "template too large");
        return ESP_OK;
    }
    memcpy(rendered, success_html_start, tpl_len);
    rendered[tpl_len] = '\0';

    // Naive replace (each token appears twice in the template). Do them
    // one at a time; capacity check above guarantees room.
    const char *repl[][2] = { {"{{IP}}", ip_buf}, {"{{MDNS}}", "esp32-pwm.local"} };
    for (int r = 0; r < 2; r++) {
        const char *tok = repl[r][0];
        const char *val = repl[r][1];
        size_t tok_len = strlen(tok);
        size_t val_len = strlen(val);
        char *p;
        while ((p = strstr(rendered, tok)) != NULL) {
            size_t tail = strlen(p + tok_len);
            if ((p - rendered) + val_len + tail + 1 > sizeof(rendered)) break;
            memmove(p + val_len, p + tok_len, tail + 1);
            memcpy(p, val, val_len);
        }
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, rendered, strlen(rendered));
}

// Tiny HTML body served to unknown URLs. Needs to be small and clearly
// non-204: Android's captive-portal detector expects a 204 empty response
// from the real internet; anything else (especially a 200 + small HTML
// with a Location-style hint) triggers the "Sign in to Wi-Fi" UI.
// The meta-refresh forces the browser to load / so even browsers that
// ignore the captive-portal OS hint still end up on the setup page.
static const char CAPTIVE_BODY[] =
    "<!doctype html><html><head>"
    "<meta http-equiv=\"refresh\" content=\"0; url=http://192.168.4.1/\">"
    "</head><body><a href=\"http://192.168.4.1/\">Setup</a></body></html>";

static esp_err_t catchall(httpd_req_t *req)
{
    // Samsung One UI (and stock Android 11+) expects the well-known probe
    // URLs to respond with a status that is NOT 204. Serving a 200 with a
    // small HTML body hits that non-204 rule and includes a body the
    // detector can show to the user if it wants to. Serving 302 instead
    // is technically valid but Samsung's detector sometimes follows the
    // redirect silently and never raises the UI — 200 + body is more
    // reliable across Android versions.
    //
    // The RFC 8908 "captive-portal" Link header is also honoured by
    // modern Android; including it costs almost nothing.
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Link",
                      "<http://192.168.4.1/>; rel=\"captive-portal\"");
    httpd_resp_send(req, CAPTIVE_BODY, sizeof(CAPTIVE_BODY) - 1);
    return ESP_OK;
}

esp_err_t captive_portal_start(captive_portal_creds_cb_t cb)
{
    s_cb = cb;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.uri_match_fn = httpd_uri_match_wildcard;   // lets catchall match *
    // Max 7 is the LWIP-imposed ceiling (10 sockets total, 3 reserved
    // internally by httpd). Raising further needs CONFIG_LWIP_MAX_SOCKETS
    // bumped in sdkconfig.defaults, which isn't worth the RAM cost right
    // now — the Samsung probe burst tolerates socket recycling at 7.
    cfg.max_open_sockets = 7;
    // Tight recv timeout so captive-probe connections that open, send
    // zero bytes, and sit don't block sockets for the full default 5 s.
    cfg.recv_wait_timeout = 2;
    cfg.send_wait_timeout = 2;

    esp_err_t e = httpd_start(&s_httpd, &cfg);
    if (e != ESP_OK) return e;

    httpd_uri_t root   = { .uri = "/",           .method = HTTP_GET,  .handler = root_get };
    httpd_uri_t scan   = { .uri = "/scan",       .method = HTTP_GET,  .handler = scan_get };
    httpd_uri_t save   = { .uri = "/save_wifi",  .method = HTTP_POST, .handler = save_wifi_post };
    httpd_uri_t okpage = { .uri = "/success",    .method = HTTP_GET,  .handler = success_get };

    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &scan);
    httpd_register_uri_handler(s_httpd, &save);
    httpd_register_uri_handler(s_httpd, &okpage);

    // Register the wildcard catch-all separately for each HTTP method we
    // expect captive-portal probes or background apps might use. GET
    // covers Android/Apple/Windows detection probes; HEAD is a common
    // alt; POST catches app chatter (e.g. WeChat /mmtls/*) so those
    // requests 302 to / instead of 405'ing and potentially confusing
    // detectors. esp_http_server does not expose HTTP_ANY, so register
    // once per method.
    const httpd_method_t catch_methods[] = { HTTP_GET, HTTP_HEAD, HTTP_POST };
    for (size_t i = 0; i < sizeof(catch_methods)/sizeof(catch_methods[0]); i++) {
        httpd_uri_t any = {
            .uri     = "/*",
            .method  = catch_methods[i],
            .handler = catchall,
        };
        httpd_register_uri_handler(s_httpd, &any);
    }

    ESP_LOGI(TAG, "captive portal up on :80");
    return ESP_OK;
}

void captive_portal_stop(void)
{
    if (!s_httpd) return;
    httpd_stop(s_httpd);
    s_httpd = NULL;
    s_cb = NULL;
}
