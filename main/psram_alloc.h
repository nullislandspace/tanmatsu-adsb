// Allocator placement policy for this app.
//
// See psram_alloc.cpp for why LVGL and cJSON are pointed at PSRAM.

#pragma once

// Install the PSRAM-first allocator hooks. Must be called before LVGL or
// cJSON are used, i.e. at the top of app_main().
void psram_alloc_init(void);
