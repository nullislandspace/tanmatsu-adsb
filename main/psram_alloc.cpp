// Allocator placement policy.
//
// The board has 32 MB of PSRAM but only a few hundred kB of usable internal
// DIRAM, and internal RAM is what the SDIO link to the WiFi coprocessor, lwIP
// and every task stack have to come out of. The UI and the JSON parser are
// made of thousands of small blocks, and IDF steers every malloc under
// CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (16 kB here) into internal RAM by
// default, so a single poll of api.adsb.lol used to fragment and exhaust it.
// esp-hosted then can't get a contiguous DMA buffer any more and asserts in
// sdio_rx_get_buffer().
//
// Both allocators below ask for PSRAM first and only fall back to internal
// RAM if PSRAM is somehow unavailable.

#include <cstddef>
#include <cstdint>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "lvgl.h"

#include "psram_alloc.h"

static void* psram_malloc(size_t size) {
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static void psram_free(void* ptr) {
    heap_caps_free(ptr);
}

static void* psram_realloc(void* ptr, size_t size) {
    return heap_caps_realloc_prefer(ptr, size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void psram_alloc_init(void) {
    cJSON_Hooks hooks = {};
    hooks.malloc_fn     = psram_malloc;
    hooks.free_fn       = psram_free;
    cJSON_InitHooks(&hooks);
}

// --- LVGL's custom stdlib backend (CONFIG_LV_USE_CUSTOM_MALLOC) -------------
// lv_mem.c calls through these; they are the only place LVGL gets memory from.

extern "C" {

void lv_mem_init(void) {}

void lv_mem_deinit(void) {}

lv_mem_pool_t lv_mem_add_pool(void* mem, size_t bytes) {
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return nullptr;
}

void lv_mem_remove_pool(lv_mem_pool_t pool) {
    LV_UNUSED(pool);
}

void* lv_malloc_core(size_t size) {
    return psram_malloc(size);
}

void* lv_realloc_core(void* ptr, size_t new_size) {
    return psram_realloc(ptr, new_size);
}

void lv_free_core(void* ptr) {
    psram_free(ptr);
}

void lv_mem_monitor_core(lv_mem_monitor_t* mon_p) {
    size_t const total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) + heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t const free  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) + heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    mon_p->total_size        = total;
    mon_p->free_size         = free;
    mon_p->free_biggest_size = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    mon_p->max_used          = total - heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    mon_p->used_pct          = total ? (uint8_t)((total - free) * 100 / total) : 0;
    mon_p->frag_pct          = 0;
    mon_p->used_cnt          = 0;
    mon_p->free_cnt          = 0;
}

lv_result_t lv_mem_test_core(void) {
    return LV_RESULT_OK;
}

}
