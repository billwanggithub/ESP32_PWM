# SoftAP + Captive Portal Provisioning Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace BLE-based Wi-Fi provisioning with a SoftAP + captive-portal flow so the phone's browser auto-opens (via Android's captive-portal detector), then shows both `http://fan-testkit.local/` and the raw IP on success.

**Architecture:** On boot, [components/net_dashboard/provisioning.c](components/net_dashboard/provisioning.c) checks Wi-Fi NVS. If credentials exist → STA + mDNS + existing dashboard. Otherwise → SoftAP `Fan-TestKit-setup` + captive-portal HTTP server on `192.168.4.1` + DNS hijack on UDP:53 answering every query with `192.168.4.1`. On credentials submit, briefly enter APSTA, wait for `IP_EVENT_STA_GOT_IP`, serve a success page, then tear down AP after 30 s grace and bring up the dashboard. Drop the `espressif/network_provisioning` component and BLE entirely.

**Tech Stack:** ESP-IDF v6.0, `esp_wifi` (SoftAP + STA), `esp_http_server`, raw UDP sockets (lwIP) for DNS hijack, `mdns` component for hostname advertisement, `cJSON` for request/response bodies.

---

## Reference — Spec

Design doc: [docs/superpowers/specs/2026-04-22-softap-captive-portal-design.md](docs/superpowers/specs/2026-04-22-softap-captive-portal-design.md)

## Reference — Why no unit tests

This firmware project has no existing test infrastructure. Verification is **on-hardware** per the spec's testing plan (phone browser auto-opens, dashboard reachable via mDNS and raw IP, etc.). Each task ends with a concrete build + observe step, not a unit-test assert.

## Reference — Hardware invariants already in CLAUDE.md

- `idf.py flash` via USB1 (CH343P) on whichever COM port the board enumerates as (user's machine: `COM24`). If the chip gets stuck in `DOWNLOAD(USB/UART0) waiting for download`, physical power-cycle — do not try to remove the auto-program transistors.
- `sdkconfig` trap: after editing `sdkconfig.defaults` for a Kconfig already in `sdkconfig`, you MUST `del sdkconfig` then `idf.py fullclean` then rebuild. Symbol changes to `sdkconfig.defaults` are otherwise silently ignored.
- MCPWM band-cross workaround (`LOGD` timing) must stay intact — verify with `pwm 100 50` then scope GPIO4 after any `sdkconfig.defaults` edit, even ones unrelated to MCPWM.

## File Structure

### Files to create

| Path | Responsibility |
|------|----------------|
| `components/net_dashboard/captive_portal.c` | HTTP handlers (`/`, `/scan`, `/save_wifi`, `/success`, catch-all) + lifecycle (`captive_portal_start`/`stop`). Owns the captive httpd handle. |
| `components/net_dashboard/captive_portal.h` | Public API: `captive_portal_start(on_creds_cb_t)`, `captive_portal_stop(void)`, typedef for the creds callback. |
| `components/net_dashboard/dns_hijack.c` | FreeRTOS task binding UDP:53 on the AP netif, answering every A query with `192.168.4.1`. |
| `components/net_dashboard/dns_hijack.h` | `dns_hijack_start(void)`, `dns_hijack_stop(void)`. |
| `components/net_dashboard/mdns_svc.c` | Wraps `mdns_init/hostname_set/service_add`. Stopped via `mdns_free()`. |
| `components/net_dashboard/mdns_svc.h` | `mdns_svc_start(void)`, `mdns_svc_stop(void)`. |
| `components/net_dashboard/web/setup.html` | SSID dropdown (populated from `/scan`), password input, submit button, inline error area. Vanilla JS, no framework. |
| `components/net_dashboard/web/success.html` | Two placeholder tokens `{{IP}}` and `{{MDNS}}` substituted by the handler. ASCII only. |

### Files to modify

| Path | Change |
|------|--------|
| `components/net_dashboard/provisioning.c` | Rewrite entirely: drop BLE, implement boot-mode state machine. Signatures of `provisioning_run_and_connect()` and `prov_clear_credentials()` unchanged. |
| `components/net_dashboard/prov_internal.h` | Unchanged (the two existing entry points keep the same contract). |
| `components/net_dashboard/CMakeLists.txt` | Drop `espressif__network_provisioning` and `bt` from REQUIRES; add `mdns`, `lwip`. Add new SRCS and EMBED_TXTFILES. |
| `main/idf_component.yml` | Remove the `espressif/network_provisioning` dependency. |
| `sdkconfig.defaults` | Remove BLE block (`CONFIG_BT_*`), `CONFIG_NETWORK_PROV_NETWORK_TYPE_WIFI`, `CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_1`. Add `CONFIG_MDNS_MAX_SERVICES=4` (the mDNS component's Kconfig default is fine but we make it explicit to avoid silent-default traps per CLAUDE.md). |
| `CLAUDE.md` | Update the "Component manager dependencies" section to remove the `network_provisioning` paragraph, update the "Security posture" / provisioning notes if they reference BLE. |
| `HANDOFF.md` | Append a migration note: BLE provisioning removed in favour of SoftAP captive portal. |

---

## Task 1: Remove BLE provisioning dependency

**Why first:** Gets us out of the broken intermediate state quickly — the old `provisioning.c` will not compile once we drop `network_provisioning`, and we want a single commit that removes BLE and adds the SoftAP skeleton. This task only does the dependency wipe; the next task wires up a stub `provisioning.c` that unblocks the build.

**Files:**
- Modify: `main/idf_component.yml`
- Modify: `sdkconfig.defaults`
- Modify: `components/net_dashboard/CMakeLists.txt`

- [ ] **Step 1: Remove network_provisioning from component manager**

Edit `main/idf_component.yml` to this:

```yaml
## Managed components fetched by the ESP-IDF Component Manager on first build.
## Docs: https://docs.espressif.com/projects/idf-component-manager/
##
## esp_tinyusb is pinned to the 1.7.x series intentionally. The 2.x rewrite
## requires the application to hand-build a composite configuration
## descriptor (tinyusb_desc_config_t.full_speed_config), while 1.x still
## auto-generates the descriptor from Kconfig for the common HID+CDC case
## that this firmware needs. If you upgrade to 2.x later, usb_composite.c
## and usb_descriptors.c must both be rewritten.
dependencies:
  idf:
    version: ">=6.0.0"
  espressif/esp_tinyusb:
    version: "~1.7.0"
```

- [ ] **Step 2: Remove BLE + provisioning Kconfigs from sdkconfig.defaults**

Delete these blocks from `sdkconfig.defaults`:

Lines containing the BT section (from the `# Bluetooth (BLE) for network_provisioning` comment through `CONFIG_BT_CONTROLLER_ENABLED=y`) AND the `# network_provisioning` section AND the `# protocomm security version 1` section. Replace with:

```
# mDNS service for dashboard discovery (`fan-testkit.local`). Default
# MAX_SERVICES=10 is fine, but we pin it explicitly so cleaning
# sdkconfig doesn't silently drop below 1.
CONFIG_MDNS_MAX_SERVICES=4
```

- [ ] **Step 3: Drop dependencies from net_dashboard CMakeLists**

Edit `components/net_dashboard/CMakeLists.txt` — remove `espressif__network_provisioning` and `bt` from REQUIRES. Final REQUIRES block:

```cmake
idf_component_register(
    SRCS        "net_dashboard.c"
                "ws_handler.c"
                "provisioning.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES    esp_http_server
                esp_wifi
                esp_netif
                esp_event
                nvs_flash
                espressif__cjson
                app_api
                pwm_gen
                rpm_cap
                ota_core
                # BOOT-button watcher in net_dashboard.c uses gpio_config/
                # gpio_get_level (v6.0 moved these out of the driver umbrella).
                esp_driver_gpio
                # SoftAP captive portal DNS hijack (raw UDP socket).
                lwip
                # mDNS advertisement of fan-testkit.local after provisioning.
                mdns
    EMBED_TXTFILES "web/index.html"
                   "web/app.js"
                   "web/app.css"
)
```

- [ ] **Step 4: Force sdkconfig regeneration**

Per the CLAUDE.md `sdkconfig` trap — Kconfigs removed from `sdkconfig.defaults` stay in `sdkconfig` and would be silently respected. Delete `sdkconfig` so the build re-derives it:

```
del sdkconfig
```

(Git Bash equivalent: `rm sdkconfig`.)

- [ ] **Step 5: Verify — nothing builds yet**

Run in an ESP-IDF v6.0 shell (the `esp6 pwm` alias):

```
idf.py fullclean
idf.py build
```

Expected: build **fails** in `provisioning.c` with errors about `network_provisioning/manager.h` not found. This is intentional — next task replaces `provisioning.c`.

- [ ] **Step 6: Commit**

```
git add main/idf_component.yml sdkconfig.defaults components/net_dashboard/CMakeLists.txt
git commit -m "chore(prov): drop BLE network_provisioning dependency

Preparing to replace BLE provisioning flow with SoftAP captive portal.
provisioning.c will fail to build until the next commit — bundle these
for easier rollback but acknowledge the broken intermediate state."
```

---

## Task 2: Stub provisioning.c — build-green skeleton

**Why:** Get the tree building again before adding real captive-portal logic. `provisioning.c` is rewritten to a minimal "boot STA if credentials exist, log a TODO otherwise" so the rest of the firmware (dashboard, PWM, etc.) still links.

**Files:**
- Modify: `components/net_dashboard/provisioning.c` (rewrite)

- [ ] **Step 1: Replace provisioning.c with the skeleton**

Write this to `components/net_dashboard/provisioning.c`:

```c
#include <string.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "prov_internal.h"

static const char *TAG = "prov";

static EventGroupHandle_t s_wifi_events;
#define WIFI_CONNECTED_BIT BIT0

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static bool nvs_has_wifi_credentials(void)
{
    wifi_config_t cfg = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) != ESP_OK) return false;
    return cfg.sta.ssid[0] != '\0';
}

esp_err_t provisioning_run_and_connect(void)
{
    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL));

    if (nvs_has_wifi_credentials()) {
        ESP_LOGI(TAG, "credentials present → STA");
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
        xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
        ESP_LOGI(TAG, "wifi connected");
        return ESP_OK;
    }

    // TODO(captive-portal): SoftAP + captive portal flow is wired up in later tasks.
    ESP_LOGW(TAG, "no credentials — captive portal not yet implemented");
    return ESP_FAIL;
}

esp_err_t prov_clear_credentials(void)
{
    ESP_LOGW(TAG, "clearing wifi credentials");
    return esp_wifi_restore();
}
```

- [ ] **Step 2: Build**

```
idf.py build
```

Expected: **build succeeds**. Warnings OK; no errors.

- [ ] **Step 3: Flash and observe (smoke test)**

Only run this on a board whose Wi-Fi NVS still holds valid creds from the previous BLE-provisioned firmware (most dev boards will). Otherwise skip to the next task.

```
idf.py -p COM24 flash monitor
```

Expected log sequence:
- `prov: credentials present → STA`
- `prov: wifi connected`
- Dashboard reachable at the device's IP.

If the board has no creds stored (or creds for a now-unavailable SSID), you'll see `prov: no credentials — captive portal not yet implemented` and the firmware returns `ESP_FAIL` from `net_dashboard_start`. That's OK — the next tasks fix it.

- [ ] **Step 4: Commit**

```
git add components/net_dashboard/provisioning.c
git commit -m "refactor(prov): stub provisioning.c to unblock build

Replace BLE flow with a minimal STA-only implementation that reads
credentials from Wi-Fi NVS. No-credentials path logs a TODO and
returns ESP_FAIL; captive portal added in subsequent commits."
```

---

## Task 3: DNS hijack module

**Why:** Isolating the DNS server in its own compilation unit keeps `captive_portal.c` focused on HTTP. A DNS hijack that answers every query with the AP gateway IP is what triggers Android's captive-portal detector.

**Files:**
- Create: `components/net_dashboard/dns_hijack.h`
- Create: `components/net_dashboard/dns_hijack.c`
- Modify: `components/net_dashboard/CMakeLists.txt` (add to SRCS)

- [ ] **Step 1: Write dns_hijack.h**

```c
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Start a UDP:53 listener on the SoftAP netif. Every A-record query is
// answered with 192.168.4.1 (the AP gateway). This is what makes
// Android's captive-portal detector fire and auto-open a browser.
// Safe to call only while WIFI_MODE_AP or WIFI_MODE_APSTA is active.
esp_err_t dns_hijack_start(void);

// Stop the DNS task and close the socket. Blocks up to ~500ms waiting
// for the task to exit its recv loop.
void dns_hijack_stop(void);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Write dns_hijack.c**

```c
#include "dns_hijack.h"

#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "dns_hijack";

static TaskHandle_t s_task;
static int          s_sock = -1;
static volatile bool s_run;

// AP gateway IP. Must match esp_netif's default SoftAP config (192.168.4.1).
static const uint8_t AP_IP[4] = {192, 168, 4, 1};

// Build the A-record answer appended after the question section.
// Uses name compression (0xC00C) pointing back at the question name.
// RR wire format: NAME(2) TYPE(2) CLASS(2) TTL(4) RDLENGTH(2) RDATA(4) = 16 bytes.
static int append_answer(uint8_t *out, int question_end_off)
{
    uint8_t *p = out + question_end_off;
    *p++ = 0xC0; *p++ = 0x0C;               // compressed name → offset 12
    *p++ = 0x00; *p++ = 0x01;               // TYPE = A
    *p++ = 0x00; *p++ = 0x01;               // CLASS = IN
    *p++ = 0x00; *p++ = 0x00; *p++ = 0x00; *p++ = 0x3C;  // TTL = 60
    *p++ = 0x00; *p++ = 0x04;               // RDLENGTH = 4
    memcpy(p, AP_IP, 4); p += 4;
    return (int)(p - (out + question_end_off));
}

// Walk the first question's NAME field to find where the question section ends.
// Returns offset just past the QTYPE/QCLASS pair, or -1 on malformed input.
static int question_end_offset(const uint8_t *buf, int len)
{
    int off = 12;                            // skip DNS header
    while (off < len) {
        uint8_t l = buf[off];
        if (l == 0) { off += 1; break; }
        if ((l & 0xC0) == 0xC0) { off += 2; break; }  // compressed, unusual in queries
        off += 1 + l;
    }
    off += 4;                                // QTYPE + QCLASS
    return off <= len ? off : -1;
}

static void dns_task(void *arg)
{
    uint8_t buf[512];
    struct sockaddr_in peer;
    socklen_t peer_len;

    while (s_run) {
        peer_len = sizeof(peer);
        int n = recvfrom(s_sock, buf, sizeof(buf), 0,
                         (struct sockaddr *)&peer, &peer_len);
        if (n < 12) continue;                // need at least a header

        int qend = question_end_offset(buf, n);
        if (qend < 0 || qend + 16 > (int)sizeof(buf)) continue;

        // Flip flags: QR=1 response, Opcode=query (leave), RA=1, RCODE=0.
        buf[2] = 0x81;                       // QR=1, OPCODE=0, AA=0, TC=0, RD=copy(=1 usually)
        buf[3] = 0x80;                       // RA=1, RCODE=0
        buf[6] = 0x00; buf[7] = 0x01;        // ANCOUNT = 1
        buf[8] = 0x00; buf[9] = 0x00;        // NSCOUNT = 0
        buf[10] = 0x00; buf[11] = 0x00;      // ARCOUNT = 0

        int ans_len = append_answer(buf, qend);
        sendto(s_sock, buf, qend + ans_len, 0,
               (struct sockaddr *)&peer, peer_len);
    }

    close(s_sock);
    s_sock = -1;
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t dns_hijack_start(void)
{
    if (s_sock >= 0) return ESP_ERR_INVALID_STATE;

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) return ESP_FAIL;

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind failed: errno=%d", errno);
        close(s_sock); s_sock = -1;
        return ESP_FAIL;
    }

    // 500ms recv timeout so the task can poll s_run during shutdown.
    struct timeval tv = { .tv_sec = 0, .tv_usec = 500 * 1000 };
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    s_run = true;
    if (xTaskCreate(dns_task, "dns_hijack", 3072, NULL, 2, &s_task) != pdPASS) {
        close(s_sock); s_sock = -1;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "dns hijack up (UDP:53 → 192.168.4.1)");
    return ESP_OK;
}

void dns_hijack_stop(void)
{
    if (!s_run) return;
    s_run = false;
    // Task will exit within one recv timeout (~500 ms) and self-delete.
    for (int i = 0; i < 20 && s_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
```

- [ ] **Step 3: Add to CMakeLists**

Edit `components/net_dashboard/CMakeLists.txt`, append `"dns_hijack.c"` to the SRCS list:

```cmake
    SRCS        "net_dashboard.c"
                "ws_handler.c"
                "provisioning.c"
                "dns_hijack.c"
```

- [ ] **Step 4: Build**

```
idf.py build
```

Expected: success. No link errors.

- [ ] **Step 5: Commit**

```
git add components/net_dashboard/dns_hijack.h components/net_dashboard/dns_hijack.c components/net_dashboard/CMakeLists.txt
git commit -m "feat(prov): DNS hijack for captive portal

Raw UDP:53 server that answers every A query with 192.168.4.1 so
Android's captive-portal detector fires when the phone joins the
setup AP."
```

---

## Task 4: mDNS service wrapper

**Why:** After successful provisioning the user expects `http://fan-testkit.local/` to work. Wrapping the mDNS init in two functions keeps `provisioning.c` tidy and gives us a single teardown point if we need to restart mDNS on IP change later.

**Files:**
- Create: `components/net_dashboard/mdns_svc.h`
- Create: `components/net_dashboard/mdns_svc.c`
- Modify: `components/net_dashboard/CMakeLists.txt` (add to SRCS)

- [ ] **Step 1: Write mdns_svc.h**

```c
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Start mDNS advertising "fan-testkit.local" and an _http._tcp service on
// port 80. Call after IP_EVENT_STA_GOT_IP.
esp_err_t mdns_svc_start(void);

// Tear down the mDNS service (mdns_free).
void mdns_svc_stop(void);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Write mdns_svc.c**

```c
#include "mdns_svc.h"

#include "mdns.h"
#include "esp_log.h"

static const char *TAG = "mdns_svc";

esp_err_t mdns_svc_start(void)
{
    esp_err_t e = mdns_init();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init: %s", esp_err_to_name(e));
        return e;
    }
    ESP_ERROR_CHECK(mdns_hostname_set("fan-testkit"));
    ESP_ERROR_CHECK(mdns_instance_name_set("Fan-TestKit Dashboard"));
    ESP_ERROR_CHECK(mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0));
    ESP_LOGI(TAG, "mdns up: fan-testkit.local");
    return ESP_OK;
}

void mdns_svc_stop(void)
{
    mdns_free();
}
```

- [ ] **Step 3: Add to CMakeLists**

Append `"mdns_svc.c"` to the SRCS list in `components/net_dashboard/CMakeLists.txt`.

- [ ] **Step 4: Build**

```
idf.py build
```

Expected: success.

- [ ] **Step 5: Commit**

```
git add components/net_dashboard/mdns_svc.h components/net_dashboard/mdns_svc.c components/net_dashboard/CMakeLists.txt
git commit -m "feat(prov): mDNS service wrapper for fan-testkit.local

Two-function shim around esp-idf mdns component. Called from
provisioning.c after GOT_IP."
```

---

## Task 5: Setup page HTML

**Why:** Static asset. Embedding early means the captive portal module can reference the `_binary_setup_html_*` symbols in the next task.

**Files:**
- Create: `components/net_dashboard/web/setup.html`
- Modify: `components/net_dashboard/CMakeLists.txt` (add to EMBED_TXTFILES)

- [ ] **Step 1: Write web/setup.html**

Create `components/net_dashboard/web/setup.html`:

```html
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Fan-TestKit Setup</title>
<style>
body { font-family: sans-serif; max-width: 420px; margin: 1em auto; padding: 0 1em; }
h1 { font-size: 1.3em; }
label { display: block; margin: 1em 0 0.3em; }
select, input, button { font-size: 1em; width: 100%; padding: 0.6em; box-sizing: border-box; }
button { margin-top: 1em; background: #205080; color: #fff; border: none; border-radius: 4px; }
button:disabled { background: #888; }
#err { color: #a00; margin-top: 0.8em; white-space: pre-wrap; }
#manual { display: none; }
.hint { color: #666; font-size: 0.9em; }
</style>
</head>
<body>
<h1>Fan-TestKit Setup</h1>
<p class="hint">Choose your home Wi-Fi and enter the password. The device will then switch off this setup network and join your Wi-Fi.</p>

<label for="ssid">Wi-Fi network</label>
<select id="ssid">
  <option value="">Scanning...</option>
</select>

<div id="manual">
  <label for="ssid_manual">SSID</label>
  <input id="ssid_manual" type="text" autocapitalize="off" autocorrect="off" spellcheck="false">
</div>

<label for="pw">Password</label>
<input id="pw" type="password">

<button id="go">Connect</button>
<div id="err"></div>

<script>
const ssidSel = document.getElementById('ssid');
const ssidMan = document.getElementById('ssid_manual');
const manualBox = document.getElementById('manual');
const pw = document.getElementById('pw');
const err = document.getElementById('err');
const go = document.getElementById('go');

async function scan() {
  try {
    const r = await fetch('/scan');
    const list = await r.json();
    ssidSel.innerHTML = '';
    list.forEach(n => {
      const o = document.createElement('option');
      o.value = n.ssid;
      o.textContent = `${n.ssid}  (${n.rssi} dBm)`;
      ssidSel.appendChild(o);
    });
    const o = document.createElement('option');
    o.value = '__manual__';
    o.textContent = 'Other...';
    ssidSel.appendChild(o);
  } catch (e) {
    ssidSel.innerHTML = '';
    const o = document.createElement('option');
    o.value = '__manual__';
    o.textContent = 'Scan failed — enter SSID manually';
    ssidSel.appendChild(o);
    ssidSel.value = '__manual__';
    manualBox.style.display = 'block';
  }
}

ssidSel.addEventListener('change', () => {
  manualBox.style.display = ssidSel.value === '__manual__' ? 'block' : 'none';
});

go.addEventListener('click', async () => {
  err.textContent = '';
  go.disabled = true;
  go.textContent = 'Connecting... (up to 20 s)';
  const ssid = ssidSel.value === '__manual__' ? ssidMan.value : ssidSel.value;
  if (!ssid) { err.textContent = 'Pick or type an SSID.'; go.disabled = false; go.textContent = 'Connect'; return; }
  try {
    const r = await fetch('/save_wifi', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({ssid, password: pw.value}),
    });
    if (r.ok) {
      window.location = '/success';
    } else {
      const j = await r.json().catch(() => ({error: 'unknown'}));
      err.textContent = 'Failed: ' + j.error;
      go.disabled = false;
      go.textContent = 'Connect';
    }
  } catch (e) {
    err.textContent = 'Request failed: ' + e;
    go.disabled = false;
    go.textContent = 'Connect';
  }
});

scan();
</script>
</body>
</html>
```

- [ ] **Step 2: Add to CMakeLists EMBED_TXTFILES**

Edit `components/net_dashboard/CMakeLists.txt`, extend the EMBED_TXTFILES block:

```cmake
    EMBED_TXTFILES "web/index.html"
                   "web/app.js"
                   "web/app.css"
                   "web/setup.html"
```

- [ ] **Step 3: Build**

```
idf.py build
```

Expected: success. Symbols `_binary_setup_html_start`/`_binary_setup_html_end` are now linked in.

- [ ] **Step 4: Commit**

```
git add components/net_dashboard/web/setup.html components/net_dashboard/CMakeLists.txt
git commit -m "feat(prov): setup page for captive portal

Vanilla HTML+JS — fetches /scan for SSID list, POSTs credentials
to /save_wifi, redirects to /success on 200."
```

---

## Task 6: Success page HTML (template)

**Why:** Same as Task 5 — embed early so the HTTP handler has symbols to reference. This file contains the two placeholder tokens that `captive_portal.c` will substitute at serve time.

**Files:**
- Create: `components/net_dashboard/web/success.html`
- Modify: `components/net_dashboard/CMakeLists.txt`

- [ ] **Step 1: Write web/success.html**

```html
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Fan-TestKit Setup Complete</title>
<style>
body { font-family: sans-serif; max-width: 420px; margin: 1em auto; padding: 0 1em; }
h1 { font-size: 1.3em; }
.link { display: block; padding: 0.7em; margin: 0.6em 0; background: #eef; border: 1px solid #88a; border-radius: 4px; text-decoration: none; color: #205080; word-break: break-all; }
.hint { color: #555; font-size: 0.95em; }
</style>
</head>
<body>
<h1>Setup complete [OK]</h1>
<p class="hint">Reconnect your phone to your home Wi-Fi, then tap either link:</p>

<a class="link" href="http://{{MDNS}}/">http://{{MDNS}}/</a>
<p class="hint">(try this first)</p>

<a class="link" href="http://{{IP}}/">http://{{IP}}/</a>
<p class="hint">(fallback if .local names do not resolve on your phone)</p>
</body>
</html>
```

- [ ] **Step 2: Add to CMakeLists EMBED_TXTFILES**

Extend EMBED_TXTFILES:

```cmake
    EMBED_TXTFILES "web/index.html"
                   "web/app.js"
                   "web/app.css"
                   "web/setup.html"
                   "web/success.html"
```

- [ ] **Step 3: Build**

```
idf.py build
```

Expected: success.

- [ ] **Step 4: Commit**

```
git add components/net_dashboard/web/success.html components/net_dashboard/CMakeLists.txt
git commit -m "feat(prov): success page template with {{IP}} and {{MDNS}} tokens

Substituted at serve time by captive_portal.c in the next commit."
```

---

## Task 7: Captive portal HTTP handlers + lifecycle

**Why:** The core of the feature. A self-contained module that owns its own httpd handle and exposes `start` / `stop` plus a credentials callback so `provisioning.c` can be notified asynchronously when the user submits the form.

**Files:**
- Create: `components/net_dashboard/captive_portal.h`
- Create: `components/net_dashboard/captive_portal.c`
- Modify: `components/net_dashboard/CMakeLists.txt`

- [ ] **Step 1: Write captive_portal.h**

```c
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Fired when the user submits /save_wifi. Callback receives null-terminated
// ssid and password. Return ESP_OK if the credentials drove a successful
// STA connect; otherwise the captive portal will keep the form open and
// show the error string (err_msg) to the user.
//
// The callback is invoked from the httpd thread and MUST block until it
// knows the outcome (success or failure), because the HTTP response to
// POST /save_wifi is sent after the callback returns.
typedef struct {
    char        ip[16];      // dotted-quad, filled on success
    const char *mdns;        // e.g. "fan-testkit.local" — filled on success
    char        err_msg[64]; // filled on failure
} captive_portal_result_t;

typedef esp_err_t (*captive_portal_creds_cb_t)(const char *ssid,
                                               const char *password,
                                               captive_portal_result_t *out);

// Start the HTTP server on port 80 and register handlers. Does NOT start
// the DNS hijack (that's separate — see dns_hijack.h).
esp_err_t captive_portal_start(captive_portal_creds_cb_t cb);

// Stop the HTTP server.
void captive_portal_stop(void);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Write captive_portal.c**

```c
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
    int n = httpd_req_recv(req, body, req->content_len);
    if (n <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "read failed");
        return ESP_OK;
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
        cJSON_AddStringToObject(resp, "mdns", out.mdns ? out.mdns : "fan-testkit.local");
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
    // We rewrite into a stack buffer; template is ~700 bytes, buffer 1 KB safe.
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info = {0};
    if (sta) esp_netif_get_ip_info(sta, &info);
    char ip_buf[16];
    snprintf(ip_buf, sizeof(ip_buf), IPSTR, IP2STR(&info.ip));

    const size_t tpl_len = (size_t)(success_html_end - success_html_start - 1);
    static char rendered[2048];
    if (tpl_len + 64 > sizeof(rendered)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "template too large");
        return ESP_OK;
    }
    memcpy(rendered, success_html_start, tpl_len);
    rendered[tpl_len] = '\0';

    // Naive replace (each token appears twice in the template). Do them
    // one at a time; capacity check above guarantees room.
    const char *repl[][2] = { {"{{IP}}", ip_buf}, {"{{MDNS}}", "fan-testkit.local"} };
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

static esp_err_t catchall(httpd_req_t *req)
{
    // Any unknown URL — including Android / Apple / Windows captive-portal
    // probes — gets 302 to root so the detector sees "this is not the
    // expected 204 / success blob" and raises the sign-in UI.
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

esp_err_t captive_portal_start(captive_portal_creds_cb_t cb)
{
    s_cb = cb;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.uri_match_fn = httpd_uri_match_wildcard;   // lets catchall match *

    esp_err_t e = httpd_start(&s_httpd, &cfg);
    if (e != ESP_OK) return e;

    httpd_uri_t root   = { .uri = "/",           .method = HTTP_GET,  .handler = root_get };
    httpd_uri_t scan   = { .uri = "/scan",       .method = HTTP_GET,  .handler = scan_get };
    httpd_uri_t save   = { .uri = "/save_wifi",  .method = HTTP_POST, .handler = save_wifi_post };
    httpd_uri_t okpage = { .uri = "/success",    .method = HTTP_GET,  .handler = success_get };
    httpd_uri_t any    = { .uri = "/*",          .method = HTTP_GET,  .handler = catchall };

    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &scan);
    httpd_register_uri_handler(s_httpd, &save);
    httpd_register_uri_handler(s_httpd, &okpage);
    httpd_register_uri_handler(s_httpd, &any);

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
```

- [ ] **Step 3: Add to CMakeLists SRCS**

```cmake
    SRCS        "net_dashboard.c"
                "ws_handler.c"
                "provisioning.c"
                "dns_hijack.c"
                "mdns_svc.c"
                "captive_portal.c"
```

- [ ] **Step 4: Build**

```
idf.py build
```

Expected: success. Warnings about unused params OK; no errors.

- [ ] **Step 5: Commit**

```
git add components/net_dashboard/captive_portal.h components/net_dashboard/captive_portal.c components/net_dashboard/CMakeLists.txt
git commit -m "feat(prov): captive portal HTTP handlers

Handlers for /, /scan, /save_wifi, /success, and a wildcard catch-all
that 302s to / so Android's captive-portal detector fires. Module
owns its own httpd handle; provisioning.c starts and stops it."
```

---

## Task 8: Wire it all up in provisioning.c

**Why:** Replace the stub's "no credentials — TODO" branch with the real SoftAP + captive portal flow, driving the modules from Tasks 3-7.

**Files:**
- Modify: `components/net_dashboard/provisioning.c` (rewrite)

- [ ] **Step 1: Final provisioning.c**

Replace the file contents with:

```c
#include <string.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "prov_internal.h"
#include "captive_portal.h"
#include "dns_hijack.h"
#include "mdns_svc.h"

static const char *TAG = "prov";

static EventGroupHandle_t s_ev;
#define EV_GOT_IP   BIT0
#define EV_STA_FAIL BIT1

static char s_last_ip[16] = {0};

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        // Only signal failure when captive portal is waiting; for post-
        // provision steady state we want auto-reconnect behaviour.
        xEventGroupSetBits(s_ev, EV_STA_FAIL);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        snprintf(s_last_ip, sizeof(s_last_ip), IPSTR, IP2STR(&evt->ip_info.ip));
        ESP_LOGI(TAG, "got ip: %s", s_last_ip);
        xEventGroupSetBits(s_ev, EV_GOT_IP);
    }
}

static bool nvs_has_wifi_credentials(void)
{
    wifi_config_t cfg = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) != ESP_OK) return false;
    return cfg.sta.ssid[0] != '\0';
}

// Invoked from the captive portal httpd thread. Applies credentials,
// waits up to 20 s for IP, fills result.
static esp_err_t on_credentials(const char *ssid, const char *password,
                                captive_portal_result_t *out)
{
    ESP_LOGI(TAG, "applying creds: ssid=%s", ssid);

    // Switch into APSTA so the phone stays associated while STA dials out.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    wifi_config_t sta = {0};
    strncpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid) - 1);
    strncpy((char *)sta.sta.password, password, sizeof(sta.sta.password) - 1);
    sta.sta.threshold.authmode = WIFI_AUTH_OPEN;  // accept any authmode that matches the password
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));

    xEventGroupClearBits(s_ev, EV_GOT_IP | EV_STA_FAIL);
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_ev, EV_GOT_IP | EV_STA_FAIL,
                                           pdTRUE, pdFALSE, pdMS_TO_TICKS(20000));
    if (bits & EV_GOT_IP) {
        strncpy(out->ip, s_last_ip, sizeof(out->ip) - 1);
        out->mdns = "fan-testkit.local";
        return ESP_OK;
    }

    // Timeout or explicit disconnect. Roll back to AP-only so user can retry.
    snprintf(out->err_msg, sizeof(out->err_msg),
             (bits & EV_STA_FAIL) ? "auth/connect failed" : "timeout (20 s)");
    esp_wifi_disconnect();
    wifi_config_t blank = {0};
    esp_wifi_set_config(WIFI_IF_STA, &blank);
    esp_wifi_set_mode(WIFI_MODE_AP);
    return ESP_FAIL;
}

static void ap_teardown_task(void *arg)
{
    // Let the phone load /success before we kill the AP.
    vTaskDelay(pdMS_TO_TICKS(30000));
    ESP_LOGI(TAG, "tearing down AP + DNS hijack");
    dns_hijack_stop();
    captive_portal_stop();
    esp_wifi_set_mode(WIFI_MODE_STA);
    mdns_svc_start();
    vTaskDelete(NULL);
}

static esp_err_t run_softap_portal(void)
{
    esp_netif_create_default_wifi_ap();

    wifi_config_t ap = {0};
    strcpy((char *)ap.ap.ssid, "Fan-TestKit-setup");
    ap.ap.ssid_len       = strlen("Fan-TestKit-setup");
    ap.ap.channel        = 1;
    ap.ap.authmode       = WIFI_AUTH_OPEN;
    ap.ap.max_connection = 4;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_ERROR_CHECK(dns_hijack_start());
    ESP_ERROR_CHECK(captive_portal_start(on_credentials));

    ESP_LOGI(TAG, "SoftAP 'Fan-TestKit-setup' up — waiting for credentials");

    // Block until on_credentials succeeds — sets EV_GOT_IP.
    xEventGroupWaitBits(s_ev, EV_GOT_IP, pdFALSE, pdTRUE, portMAX_DELAY);

    // Hand control back to caller; schedule AP teardown in 30 s.
    xTaskCreate(ap_teardown_task, "ap_down", 2048, NULL, 2, NULL);
    return ESP_OK;
}

esp_err_t provisioning_run_and_connect(void)
{
    s_ev = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, on_wifi_event, NULL));

    if (nvs_has_wifi_credentials()) {
        ESP_LOGI(TAG, "credentials present → STA");
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
        xEventGroupWaitBits(s_ev, EV_GOT_IP, pdFALSE, pdTRUE, portMAX_DELAY);
        mdns_svc_start();
        return ESP_OK;
    }

    ESP_LOGI(TAG, "no credentials → SoftAP + captive portal");
    return run_softap_portal();
}

esp_err_t prov_clear_credentials(void)
{
    ESP_LOGW(TAG, "clearing wifi credentials");
    return esp_wifi_restore();
}
```

- [ ] **Step 2: Build**

```
idf.py build
```

Expected: success.

- [ ] **Step 3: Commit**

```
git add components/net_dashboard/provisioning.c
git commit -m "feat(prov): SoftAP + captive portal state machine

Replace BLE flow: on boot, if Wi-Fi NVS has credentials go STA +
mDNS + dashboard. Otherwise bring up AP 'Fan-TestKit-setup', hijack
DNS, serve captive portal. On /save_wifi success, wait 30 s so the
phone can load /success, then tear down AP and hand off to dashboard."
```

---

## Task 9: Hardware verification — factory-reset path

**Why:** Prove the flow works end-to-end on real hardware, covering every item in the spec's testing plan. Factory reset first so we're in the "no credentials" branch; then the provisioning flow itself; then the post-provision reconnect paths.

**Files:** None.

- [ ] **Step 1: Flash**

```
idf.py -p COM24 flash monitor
```

If the board still has creds from a previous firmware, trigger factory reset: either long-press BOOT ≥ 3 s, or call any of the 4 reprovision transports. Reboot the device.

Expected log on next boot: `prov: no credentials → SoftAP + captive portal` followed by `captive: captive portal up on :80` and `dns_hijack: dns hijack up (UDP:53 → 192.168.4.1)`.

- [ ] **Step 2: Phone joins the AP**

On the Android phone: Settings → Wi-Fi → pick `Fan-TestKit-setup`. It's an open network.

**Expected:** within 5-10 s, Android shows a notification "Sign in to Wi-Fi network" and/or auto-opens a browser pointing at `http://192.168.4.1/` (or any URL — DNS hijack routes it there).

If the browser does not auto-open but the notification appears, tap the notification. (Auto-open depends on Android version and manufacturer skin — Samsung One UI sometimes requires the manual tap; stock Android 12+ typically auto-opens.)

- [ ] **Step 3: Submit valid credentials**

On the setup page, select your home SSID (or type it via "Other..."), enter the password, tap Connect.

**Expected:**
- Button flips to "Connecting... (up to 20 s)".
- Within 20 s, browser redirects to `/success`.
- Success page displays `http://fan-testkit.local/` and the raw IP assigned by DHCP (e.g. `http://192.168.1.47/`).

Serial log on device:
```
prov: applying creds: ssid=<YOUR_SSID>
prov: got ip: 192.168.1.47
```

- [ ] **Step 4: Reconnect phone to home Wi-Fi, open dashboard**

Disconnect the phone from `Fan-TestKit-setup` and reconnect to the home Wi-Fi. Open Chrome, tap the mDNS link from the success page.

**Expected:** dashboard loads over `fan-testkit.local`. If it doesn't resolve (some Android builds), tap the raw IP link — expected to work unconditionally.

- [ ] **Step 5: Power-cycle, verify STA-only reboot**

Unplug, replug. Watch serial monitor.

**Expected:** `prov: credentials present → STA`, followed by `prov: got ip: ...`, and NO SoftAP (`Fan-TestKit-setup`) visible in the phone's Wi-Fi list.

- [ ] **Step 6: Test wrong-password rejection**

Factory-reset again. Join `Fan-TestKit-setup`. Submit an intentionally wrong password.

**Expected:** after ~20 s the setup page shows "Failed: auth/connect failed" (or "timeout (20 s)"). Retry with correct password in the same session works without a reboot.

- [ ] **Step 7: Verify MCPWM band-cross regression**

Per CLAUDE.md, any Kconfig edit is a regression vector for the MCPWM `LOGD` timing workaround. Scope GPIO4 while running:

```
pwm 100 50
pwm 200 50
pwm 500 50
```

**Expected:** output frequency matches setpoint at each step (especially the 100→200 Hz case, which was the bug's original repro). If output frequency is 16× the setpoint on the first band-cross, the Kconfig cleanup in Task 1 broke the workaround; revisit `CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y` in `sdkconfig.defaults` and the `esp_log_level_set("mcpwm", ESP_LOG_DEBUG)` call in `pwm_gen_init`.

- [ ] **Step 8: No commit**

This task is pure verification — no code changes.

---

## Task 10: Docs — CLAUDE.md and HANDOFF.md

**Why:** CLAUDE.md's "Component manager dependencies" section currently describes `network_provisioning` in detail, including the `PROTOCOMM_SECURITY_1` trap. Outdated memory makes future Claude sessions propose fixes that reference components that no longer exist. HANDOFF.md should gain a short migration note.

**Files:**
- Modify: `CLAUDE.md`
- Modify: `HANDOFF.md`

- [ ] **Step 1: Update CLAUDE.md — provisioning section**

Find the `## Component manager dependencies` section in `CLAUDE.md`. Replace the `network_provisioning` bullet and the "protocomm SECURITY_1" sentences with:

```
- **`espressif/cjson ^1.7.19`** — v6.0 把 IDF built-in `json` component
  拔掉，cJSON 改 component manager 上 `espressif/cjson`。
  `net_dashboard/CMakeLists.txt` 的 REQUIRES 寫 `espressif__cjson`。

Wi-Fi provisioning 現在走 SoftAP + captive portal，不再依賴
`network_provisioning` component — 板子第一次 boot 沒有 creds 時會開
`Fan-TestKit-setup` 這個 open AP，phone 接上後 Android captive-portal
detector 會自動跳 browser。成功後 success page 同時秀 `fan-testkit.local`
跟 raw IP。細節見 [components/net_dashboard/provisioning.c](components/net_dashboard/provisioning.c) 跟 spec
[docs/superpowers/specs/2026-04-22-softap-captive-portal-design.md](docs/superpowers/specs/2026-04-22-softap-captive-portal-design.md)。
```

(Adjust the surrounding bullets so the cjson item is still in its existing list; the goal is to delete the `network_provisioning` and `esp_tinyusb 1.7.x ... BLE ...` references and add the new paragraph.)

If there's a "Security posture" paragraph referencing BLE — just drop the BLE-specific sentence, leave the Secure Boot / Flash Encryption content.

- [ ] **Step 2: Append to HANDOFF.md**

Add a new dated section at the top of `HANDOFF.md` (format consistent with existing entries):

```
## 2026-04-22 — provisioning migration: BLE → SoftAP captive portal

Dropped `espressif/network_provisioning` + BLE (NimBLE) + PoP-based
protocomm SECURITY_1. Replaced with in-project SoftAP + captive portal
flow (components/net_dashboard/captive_portal.c, dns_hijack.c,
mdns_svc.c). Saves ~60 KB flash, removes the Espressif BLE Provisioning
Android app requirement.

User flow: on boot no creds → AP `Fan-TestKit-setup` + DNS hijack (every
query → 192.168.4.1) + catch-all HTTP 302 to /. Android captive-portal
detector auto-opens browser on setup page. After submit + STA connect,
success page shows `fan-testkit.local` and raw DHCP IP. AP torn down 30 s
later. Factory reset path (`prov_clear_credentials` → `esp_wifi_restore`)
unchanged for callers.

Open items:
- iOS not validated yet — protocol identical, likely works, untested.
- STA failure doesn't fall back to AP at runtime — user must factory-reset
  to re-enter setup. Acceptable per spec (Q4 in brainstorming).
```

- [ ] **Step 3: Commit**

```
git add CLAUDE.md HANDOFF.md
git commit -m "docs: refresh provisioning notes after captive-portal migration

- CLAUDE.md: drop network_provisioning + PROTOCOMM_SECURITY_1 paragraph,
  add short description of SoftAP captive-portal flow with link to spec.
- HANDOFF.md: migration note covering what was removed and open items."
```

---

## Self-review summary

**Spec coverage:**

- ✓ Spec "Architecture → boot-time mode selection" → Task 8 (`provisioning.c`).
- ✓ Spec "DNS hijack" → Task 3.
- ✓ Spec "HTTP handlers table" → Task 7 (root, /scan, /save_wifi, /success, catch-all).
- ✓ Spec "Setup page" → Task 5.
- ✓ Spec "Success page" with `{{IP}}`/`{{MDNS}}` substitution → Task 6 (template) + Task 7 (`success_get` does the replace).
- ✓ Spec "mDNS service" → Task 4 (wrapper), Task 8 (start call).
- ✓ Spec "Credential storage via built-in Wi-Fi NVS" → Task 8 (`esp_wifi_set_config`, no custom namespace) + stub Task 2 (`esp_wifi_restore()` in `prov_clear_credentials`).
- ✓ Spec "Error paths — wrong password, STA timeout, malformed JSON" → Task 7 (400 responses) + Task 8 (`on_credentials` returns ESP_FAIL with err_msg).
- ✓ Spec "Testing Plan" (6 items) → Task 9 steps 1–7.
- ✓ Spec "Migration & Rollback → single commit removing BLE" → deliberately split across Tasks 1–2 with an explicit broken-intermediate acknowledgement in the Task 1 commit message. Not a single commit, but with documented reason in the plan, the rollback path is still one `git revert` per commit.

**Placeholder scan:** No `TBD` / `TODO` / "implement later" / "add validation" / "similar to Task N". The one `TODO(captive-portal)` comment is inside Task 2's stub code and is explicitly replaced in Task 8.

**Type consistency:**

- `captive_portal_creds_cb_t` signature identical across Task 7 header and Task 8 `on_credentials` definition: `(const char *ssid, const char *password, captive_portal_result_t *out) → esp_err_t`.
- `captive_portal_result_t` members (`ip[16]`, `mdns`, `err_msg[64]`) referenced consistently.
- `dns_hijack_start` / `_stop` signatures match across Task 3 and Task 8.
- `mdns_svc_start` / `_stop` match across Task 4 and Task 8.
- Binary symbol names (`_binary_setup_html_start` / `_binary_success_html_start`) match EMBED_TXTFILES entries (`web/setup.html` / `web/success.html`).

Plan is internally consistent.
