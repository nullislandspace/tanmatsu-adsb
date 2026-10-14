#pragma once

#include "esp_err.h"

// SD card on the Tanmatsu: SDMMC slot 0, 4-bit, on the native IOMUX pins.
// Mounting is ported from badge-bsp's storage code as used by the launcher,
// which runs ESP-Hosted WiFi on SDIO slot 1 at the same time.

// Mount the card at /sd. Non-fatal: a missing card just means no /sd.
esp_err_t sdcard_mount(void);

bool sdcard_is_mounted(void);
