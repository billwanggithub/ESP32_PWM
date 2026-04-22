# SoftAP + Captive Portal Provisioning — Design

**Date:** 2026-04-22
**Status:** Approved for implementation planning
**Replaces:** BLE-based Wi-Fi provisioning via `espressif/network_provisioning`

## Problem

The current provisioning flow uses BLE via the Espressif
`network_provisioning` component. It requires the user to install the
"ESP BLE Provisioning" Android app, pair over BLE, and manually open a
browser afterwards using an IP the app does not surface. Two specific
user asks:

1. Know the device IP after provisioning finishes.
2. Auto-open the browser on the Android phone that just provisioned the
   device.

The BLE app cannot be extended to open arbitrary URLs without forking
it. A SoftAP + captive portal flow lets Android's OS-level captive-
portal detector open the phone's browser automatically, which is the
canonical consumer-IoT setup UX.

## Goals

- Phone's browser opens automatically when the user joins the device's
  setup Wi-Fi.
- The success page shows both an mDNS hostname and the raw IP, so the
  user can get to the dashboard after reconnecting to their home Wi-Fi
  regardless of whether their browser resolves `.local` names.
- Drop the `network_provisioning` component and the
  `PROTOCOMM_SECURITY_1` Kconfig enable that only exists to support it.
- Keep the existing factory-reset path (`prov_clear_credentials()`,
  wired through 4 transports as of commit `7ef24d6`) working; the only
  change is the internals.
- Keep the existing `net_dashboard` HTTP server and WebSocket dashboard
  unchanged post-provisioning.

## Non-Goals

- Multi-network storage, roaming, or priority.
- Enterprise WPA2-EAP.
- HTTPS for the captive portal (browsers warn; TOFU cert complexity not
  justified on LAN setup).
- Fallback from failed STA back into SoftAP at runtime — user triggers
  factory reset via existing transports, device re-enters setup on next
  boot.
- Concurrent AP+STA.
- iOS-specific tuning (captive-portal detection on iOS works with the
  same HTTP 302 catch-all, but we're only validating on Android).

## Architecture

### Boot-time mode selection

Single-mode-per-boot state machine in `provisioning.c`:

```
boot
 │
 ├─ esp_wifi_get_config(STA) has non-empty ssid? ──yes──►
 │                                                   set_mode(STA)
 │                                                   esp_wifi_start()
 │                                                   (NVS creds auto-load)
 │                                                   mdns_svc_start()
 │                                                   net_dashboard_start()  (existing)
 │                                                   DONE
 │
 └─ no ──► set_mode(AP)
           AP SSID "ESP32-PWM-setup", open, ch 1
           esp_wifi_start()
           dns_hijack_start()      (UDP:53 on AP netif)
           captive_portal_start()  (HTTP:80 on AP netif)
              │
              ├─ POST /save_wifi {ssid, password}
              │     set_mode(APSTA)               ← temporarily, so AP
              │     esp_wifi_set_config(STA, …)     keeps serving while
              │     esp_wifi_connect()              STA dials home Wi-Fi
              │     wait on event group for GOT_IP or FAIL (20 s)
              │
              ├─ on GOT_IP:
              │     respond to pending POST 200
              │       {ip:"x.x.x.x", mdns:"esp32-pwm.local"}
              │     arm 30-s oneshot timer
              │     on timer expiry:
              │       dns_hijack_stop()
              │       captive_portal_stop()
              │       set_mode(STA)
              │       mdns_svc_start()
              │       net_dashboard_start()
              │
              └─ on connect fail / timeout:
                    respond 400 {error:"auth failed"}
                    esp_wifi_disconnect(), set_mode(AP)
                    user retries via the same page
```

NVS persistence is handled by the Wi-Fi driver itself
(`WIFI_STORAGE_FLASH`, default). No custom namespace. `esp_wifi_restore()`
(called from `prov_clear_credentials()`) wipes it.

### Components & files

All changes scoped to `components/net_dashboard/`.

| File                              | Action    | Purpose                                          |
|-----------------------------------|-----------|--------------------------------------------------|
| `provisioning.c`                  | **rewrite** | Boot-mode state machine; no more BLE           |
| `prov_internal.h`                 | unchanged | `provisioning_run_and_connect()`, `prov_clear_credentials()` signatures stay |
| `captive_portal.c` / `.h`         | **new**   | HTTP handlers + lifecycle                        |
| `dns_hijack.c` / `.h`             | **new**   | UDP:53 catch-all DNS server                      |
| `mdns_svc.c` / `.h`               | **new**   | `esp32-pwm.local` advertisement                  |
| `assets/setup_page.html`          | **new**   | SSID scan list + credential form                 |
| `assets/success_page.html`        | **new**   | Shows IP + mDNS link                             |
| `CMakeLists.txt`                  | **edit**  | Drop `network_provisioning` REQUIRES; add        |
|                                   |           | `esp_wifi`, `esp_http_server`, `mdns`, `lwip`    |
|                                   |           | EMBED `assets/*.html`                            |
| `main/idf_component.yml`          | **edit**  | Drop `espressif/network_provisioning`            |
| `sdkconfig.defaults`              | **edit**  | Drop `CONFIG_NETWORK_PROV_*`, drop               |
|                                   |           | `CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_1`|
|                                   |           | Ensure `CONFIG_MDNS_MAX_SERVICES` ≥ 1            |

The `net_dashboard` HTTP server is started only after provisioning
succeeds. The captive portal runs its own `httpd_handle_t` on port 80
bound to the AP netif, so there is no port collision: only one server
is active at any time during setup mode, and after the 30-s post-GOT_IP
grace window we swap AP→STA and start the dashboard server.

### DNS hijack

Single FreeRTOS task, priority 2, stack 2048. Binds UDP:53 to the AP
netif's IP (`192.168.4.1`). For every inbound datagram:

1. Parse header; if QDCOUNT != 1, drop.
2. Copy the question section byte-for-byte into the response.
3. Set QR=1, RA=1, RCODE=0, ANCOUNT=1 in header.
4. Append one A record: compressed name pointer → question, TYPE=A,
   CLASS=IN, TTL=60, RDLENGTH=4, RDATA=`192.168.4.1`.
5. Send.

Stops via a flag checked each `recvfrom` cycle (500 ms timeout) so the
task can exit cleanly when captive portal tears down.

No resolver library dependency — ~80 lines of direct UDP socket code.

### HTTP handlers (captive-portal httpd)

| Method | Path          | Behavior                                                     |
|--------|---------------|--------------------------------------------------------------|
| GET    | `/`           | Serve embedded `setup_page.html`                             |
| GET    | `/scan`       | `esp_wifi_scan_start(block=true)`; return JSON array of `{ssid, rssi, auth}` |
| POST   | `/save_wifi`  | Body `{ssid, password}`; attempt connect; respond when `GOT_IP` or timeout |
| GET    | `/success`    | Serve `success_page.html` rendered with IP + mDNS hostname   |
| *      | (any other)   | 302 `Location: /`                                            |

The catch-all is what triggers Android's "Sign in to Wi-Fi network"
notification that auto-opens the browser. Android probes:

- `http://connectivitycheck.gstatic.com/generate_204`
- `http://www.google.com/generate_204`
- `http://clients3.google.com/generate_204`

All are resolved by the DNS hijack to `192.168.4.1`, all hit the
catch-all, all get 302 → `/`. Android sees "this is not a 204" and
raises the captive-portal UI.

### Setup page

Plain HTML + vanilla JS, embedded via `target_add_binary_data`. On
load: fetch `/scan`, populate a `<select>` with SSID options plus a
"Other..." entry that reveals a text input. Password `<input
type=password>`. Submit button POSTs JSON to `/save_wifi`, shows a
spinner, on 200 redirects to `/success`, on 4xx shows the error inline
and re-enables the form.

No framework. Single file, probably <4 KB.

### Success page

Plain HTML. Server-side substitution of two placeholders when served
(simple string replace in the handler, not a template engine):

```
{{IP}}    → 192.168.1.47
{{MDNS}}  → esp32-pwm.local
```

Content (ASCII only, per project convention — no emoji):

```
Setup complete [OK]

Reconnect your phone to your home Wi-Fi, then tap either link:

  http://{{MDNS}}/     -- try this first
  http://{{IP}}/       -- fallback
```

### mDNS service

Started after GOT_IP in STA mode. Hostname `esp32-pwm`, instance name
"ESP32-PWM Dashboard", service `_http._tcp` on port 80. One call to
`mdns_init()` + `mdns_hostname_set()` + `mdns_service_add()`. Stopped
and restarted if the device's IP changes (rare on a DHCP-stable LAN;
no special handling beyond what the mDNS component does itself).

## Data Flow — Happy Path

```
phone joins "ESP32-PWM-setup"
   │
   ▼
Android probes connectivitycheck.gstatic.com
   │  DNS query → DNS hijack → 192.168.4.1
   │  HTTP GET → catch-all → 302 /
   ▼
Android auto-opens browser at http://192.168.4.1/
   │
   ▼
setup_page.html loads → GET /scan → SSID list
   │
   ▼
user picks SSID, enters password, submits
   │
   ▼
POST /save_wifi
   │  set_mode(APSTA)
   │  esp_wifi_set_config(STA, ssid+pw)
   │  esp_wifi_connect()
   │
   ▼
GOT_IP event (say, 192.168.1.47)
   │
   ▼
POST response 200 {ip, mdns}
   │
   ▼
browser redirects to /success (populated with IP + mdns)
   │
   ▼
30 s later: teardown AP + hijack, switch to STA-only,
            start mDNS + dashboard httpd
   │
   ▼
user reconnects phone to home Wi-Fi, taps link, dashboard loads
```

## Error Paths

- **Wrong password / STA connect fails within 20 s timeout:** `/save_wifi`
  returns 400 with `{error: "auth_failed"}`. Device goes back to AP-only.
  Setup page shows the error and lets user retry without reload.
- **SSID scan fails (driver error):** `/scan` returns 500; setup page
  falls back to manual SSID entry only.
- **User enters garbage JSON in `/save_wifi`:** 400 `{error: "bad_request"}`.
- **Power loss mid-provision:** no partial state — Wi-Fi NVS only
  commits on successful `esp_wifi_set_config` followed by `esp_wifi_start`.
  If power drops before GOT_IP, next boot sees no creds and re-enters
  setup.
- **User never submits:** AP stays up forever. Acceptable — it's a
  setup-only AP, not reachable from outside the physical vicinity, and
  the device is useless until provisioned anyway.

## Testing Plan

1. Fresh flash on a board whose Wi-Fi NVS has been erased.
   - Phone joins `ESP32-PWM-setup`.
   - Android notification "Sign in to Wi-Fi network" appears within
     ~5 s of association.
   - Tapping it opens the setup page (or the browser opens on its own
     on newer Android versions).
2. Submit valid creds.
   - Success page renders with correct IP and `esp32-pwm.local`.
   - Phone reconnects to home Wi-Fi.
   - Tapping the mDNS link opens the dashboard (Chrome on Android
     resolves `.local`).
   - Tapping the raw-IP link opens the dashboard.
3. Submit an invalid password.
   - Error shown in the setup page within ~20 s.
   - Retry with correct password succeeds without a reboot.
4. Power-cycle after successful provisioning.
   - Device comes up in STA mode only, no SoftAP visible.
   - Dashboard reachable via mDNS and IP.
5. Factory reset via each of the 4 reprovision transports (HTTP,
   WebSocket, HID, CDC).
   - Device reboots into SoftAP mode.
   - Setup flow from step 1 works again.
6. After re-provisioning, verify the MCPWM band-cross regression
   vector: `pwm 100 50` then `pwm 200 50` — scope GPIO4 to confirm
   output is correct frequency (the LOGD-timing workaround must still
   be in place; this test is unrelated to provisioning but is a known
   regression vector when touching Kconfig).

## Migration & Rollback

**Migration:** Drop the three BLE-specific things in one commit:
the `network_provisioning` component manager entry, the
`PROTOCOMM_SECURITY_1` Kconfig line, and the body of `provisioning.c`.
Add the new files in the same commit so the tree never builds in an
intermediate broken state.

**Rollback:** Revert the commit. The `network_provisioning` component
is still available in the registry; no external infrastructure to
re-provision.

**Device compatibility:** Existing field devices that were BLE-
provisioned have their SSID/password in the Wi-Fi NVS namespace
(`nvs.net80211`) — same place the new flow reads from. They come up
in STA mode normally on first boot with the new firmware. No migration
script needed. A factory reset on such a device drops them into
SoftAP mode as expected.

## Open Items Deferred to Implementation Plan

- Exact JSON schema for `/save_wifi` request and response.
- Exact embed mechanism for HTML assets (`target_add_binary_data`
  vs. `EMBED_FILES` in CMakeLists).
- Error code strings / i18n (currently assumed English-only).
- AP channel selection — fixed to 1, or scan-based? (Leaning fixed 1
  for simplicity.)
- Whether to include an "AP password" or leave it open. (Leaning open,
  since the AP is setup-only and any password leaks to anyone who sees
  the QR sticker we'd print.)
