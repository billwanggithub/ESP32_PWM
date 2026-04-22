#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Clear stored Wi-Fi credentials from NVS. Caller must esp_restart() after
// this returns — provisioning.c only re-checks credentials at boot.
esp_err_t prov_clear_credentials(void);

#ifdef __cplusplus
}
#endif
