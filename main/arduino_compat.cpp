#include "arduino_compat.h"

#include <stdio.h>
#include "esp_log.h"

SerialShim Serial;

void SerialShim::printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

void SerialShim::println(const char* text) {
    printf("%s\n", text);
}
