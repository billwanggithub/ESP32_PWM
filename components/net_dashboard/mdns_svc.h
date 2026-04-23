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
