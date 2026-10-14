#pragma once

#include <stdarg.h>
#include <stdint.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Small compatibility layer so the ported ADS-B sources keep their original timing/logging code.

static inline uint32_t millis(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static inline void delay(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS((int)ms));
}

struct SerialShim {
    void printf(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void println(const char* text);
};

extern SerialShim Serial;
