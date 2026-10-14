#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs_flash.h"

// badge-bsp and the wifi components are plain C with no __cplusplus guards.
extern "C" {
#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/power.h"
#include "wifi_connection.h"
#include "wifi_remote.h"
}

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "arduino_compat.h"
#include "data/aircraft.h"
#include "data/enrichment.h"
#include "data/error_log.h"
#include "data/fetcher.h"
#include "data/storage.h"
#include "display_config.h"
#include "hal/keyboard_input.h"
#include "psram_alloc.h"
#include "ui/alerts.h"
#include "ui/detail_card.h"
#include "ui/filters.h"
#include "ui/input_nav.h"
#include "ui/range.h"
#include "ui/settings.h"
#include "ui/status_bar.h"
#include "ui/views.h"

static char const TAG[] = "adsb";

// --- Render buffers -------------------------------------------------------
// LVGL renders landscape 800x480 (LCD_H_RES x LCD_V_RES); the panel itself is
// 480x800 portrait, so the flush callback rotates each dirty area before blitting.
#define DRAW_BUF_LINES 40
#define DRAW_BUF_BYTES (LCD_H_RES * DRAW_BUF_LINES * (int)sizeof(uint16_t))

AircraftList aircraft_list;

static lv_display_t* _disp        = nullptr;
static uint16_t*     _draw_buf    = nullptr;
static uint16_t*     _rotate_buf  = nullptr;
static size_t        _panel_h_res = 0;  // panel x (short axis)
static size_t        _panel_v_res = 0;  // panel y (long axis)

static void disp_flush_cb(lv_display_t* display, const lv_area_t* area, uint8_t* px_map) {
    const int32_t src_w = lv_area_get_width(area);
    const int32_t src_h = lv_area_get_height(area);

    const int32_t src_stride  = (int32_t)lv_draw_buf_width_to_stride((uint32_t)src_w, LV_COLOR_FORMAT_RGB565);
    const int32_t dest_stride = src_h * (int32_t)sizeof(uint16_t);  // compact, no padding

    lv_area_t panel_area = *area;
    lv_display_rotate_area(display, &panel_area);

    lv_draw_rotate(px_map, _rotate_buf, src_w, src_h, src_stride, dest_stride, LV_DISPLAY_ROTATION_270,
                   LV_COLOR_FORMAT_RGB565);

    esp_err_t res = bsp_display_blit((size_t)panel_area.x1, (size_t)panel_area.y1, (size_t)panel_area.x2 + 1,
                                     (size_t)panel_area.y2 + 1, _rotate_buf);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to blit area (%d,%d)-(%d,%d): %d", (int)panel_area.x1, (int)panel_area.y1,
                 (int)panel_area.x2, (int)panel_area.y2, res);
    }

    lv_display_flush_ready(display);
}

static void splash_show(void) {
    lv_obj_t* screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0a1a0a), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    lv_obj_t* title = lv_label_create(screen);
    lv_label_set_text(title, "ADS-B RADAR");
    lv_obj_set_style_text_color(title, lv_color_hex(0x33ff33), 0);
    lv_obj_set_style_text_font(title, HERSHEY_28, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t* status = lv_label_create(screen);
    lv_label_set_text(status, "Starting radio...");
    lv_obj_set_style_text_color(status, lv_color_hex(0x55ff55), 0);
    lv_obj_set_style_text_font(status, HERSHEY_16, 0);
    lv_obj_align(status, LV_ALIGN_CENTER, 0, 20);

    for (int i = 0; i < 3; i++) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void gear_clicked_cb(lv_event_t* event) {
    (void)event;
    settings_show();
}

static void settings_changed_cb(const UserConfig* cfg) {
    g_config = *cfg;
}

static void status_bar_timer_cb(lv_timer_t* timer) {
    (void)timer;
    status_bar_update(fetcher_wifi_connected(), aircraft_list.count, fetcher_last_update());
}

// Keyboard front-end: keeps the LVGL keypad lane and the navigator lane in sync,
// then handles the F-key shortcuts.
static void input_timer_cb(lv_timer_t* timer) {
    (void)timer;

    const bool settings_open = settings_is_visible();
    keyboard_input_set_ui_mode(settings_open);
    keyboard_input_set_group(settings_open ? settings_get_group() : nullptr);

    if (input_function_pressed(1)) {
        bsp_device_restart_to_launcher();
        return;
    }
    if (input_function_pressed(2)) {
        if (settings_open) {
            settings_hide();
        } else {
            settings_show();
        }
    }
    if (input_function_pressed(3)) {
        range_cycle();
        views_pause_cycle();
    }
    if (input_function_pressed(4)) {
        int next = filter_get_active() + 1;
        if (next >= NUM_FILTERS) next = FILT_NONE;
        filter_set_active(next);
        views_pause_cycle();
    }
    if (input_function_pressed(5)) {
        g_config.cycle_enabled = !g_config.cycle_enabled;
        storage_save_config(g_config);
        status_bar_set_auto_indicator(g_config.cycle_enabled);
    }

    if (settings_open) {
        // Widgets own the arrow keys while the settings panel is open.
        input_cycle_delta();
        input_view_delta();
        input_select_pressed();
        if (input_back_pressed()) settings_hide();
        return;
    }

    input_nav_tick();

    if (input_back_pressed()) {
        if (alerts_is_visible()) {
            alerts_dismiss();
        } else if (!input_nav_handle_back()) {
            bsp_device_restart_to_launcher();
        }
    }
}

static void lvgl_task(void* arg) {
    (void)arg;
    while (true) {
        uint32_t next_delay = lv_timer_handler();
        if (next_delay > 100) next_delay = 100;
        vTaskDelay(pdMS_TO_TICKS((int)next_delay));
    }
}

static esp_err_t init_nvs(void) {
    esp_err_t res = nvs_flash_init();
    if (res == ESP_ERR_NVS_NO_FREE_PAGES || res == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        res = nvs_flash_erase();
        if (res != ESP_OK) return res;
        res = nvs_flash_init();
    }
    return res;
}

extern "C" void app_main(void) {
    // Before anything allocates: LVGL and cJSON go to PSRAM, not internal RAM.
    psram_alloc_init();

    ESP_ERROR_CHECK(gpio_install_isr_service(0));

    esp_err_t res = init_nvs();
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NVS flash: %d", res);
        return;
    }

    const bsp_configuration_t bsp_configuration = {
        .display =
            {
                .requested_color_format = BSP_DISPLAY_COLOR_FORMAT_16_565RGB,
                .num_fbs                = 1,
            },
    };
    res = bsp_device_initialize(&bsp_configuration);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize BSP: %d", res);
        return;
    }

    res = bsp_display_get_parameters(&_panel_h_res, &_panel_v_res, nullptr, nullptr);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get display parameters: %d", res);
        return;
    }
    if (_panel_h_res != (size_t)LCD_V_RES || _panel_v_res != (size_t)LCD_H_RES) {
        ESP_LOGW(TAG, "Panel is %ux%u but the UI assumes %dx%d", (unsigned)_panel_h_res, (unsigned)_panel_v_res,
                 LCD_V_RES, LCD_H_RES);
    }

    // --- LVGL ---
    lv_init();
    lv_tick_set_cb(millis);

    // Created with the panel's native geometry, then rotated: LVGL renders 800x480
    // and lv_display_rotate_area() maps dirty areas back onto the portrait panel.
    _disp = lv_display_create((int32_t)_panel_h_res, (int32_t)_panel_v_res);
    if (_disp == nullptr) {
        ESP_LOGE(TAG, "Failed to create LVGL display");
        return;
    }
    lv_display_set_color_format(_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_rotation(_disp, LV_DISPLAY_ROTATION_270);

    _draw_buf   = (uint16_t*)heap_caps_malloc(DRAW_BUF_BYTES, MALLOC_CAP_SPIRAM);
    _rotate_buf = (uint16_t*)heap_caps_malloc(DRAW_BUF_BYTES, MALLOC_CAP_SPIRAM);
    if (_draw_buf == nullptr || _rotate_buf == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate %d bytes of PSRAM for the render buffers", DRAW_BUF_BYTES);
        return;
    }
    lv_display_set_buffers(_disp, _draw_buf, nullptr, DRAW_BUF_BYTES, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(_disp, disp_flush_cb);

    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(indev, keyboard_input_lvgl_read_cb);
    lv_indev_set_display(indev, _disp);
    keyboard_input_set_indev(indev);

    splash_show();

    // --- WiFi (the fetch task does the actual associating/retrying) ---
    // Power-cycle the radio coprocessor before bringing up esp-hosted: the
    // launcher may have left it in any state, and the SDIO link only asks for a
    // slave reset once it has already failed.
    bsp_power_set_radio_state(BSP_POWER_RADIO_STATE_OFF);
    vTaskDelay(pdMS_TO_TICKS(500));
    bsp_power_set_radio_state(BSP_POWER_RADIO_STATE_APPLICATION);

    if (wifi_remote_initialize() != ESP_OK) {
        ESP_LOGE(TAG, "WiFi radio not responding, continuing without network");
        bsp_power_set_radio_state(BSP_POWER_RADIO_STATE_OFF);
    } else {
        wifi_connection_init_stack();
    }

    // --- UI ---
    aircraft_list.init();
    if (aircraft_list.aircraft == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate the aircraft list in PSRAM");
        return;
    }

    lv_obj_t* screen = lv_screen_active();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0a0a1a), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    status_bar_create(screen);
    views_init(screen, &aircraft_list);
    detail_card_init(screen);
    alerts_init(screen);
    settings_init(screen);

    g_config = storage_load_config();
    status_bar_set_gear_callback(gear_clicked_cb);
    status_bar_set_auto_indicator(g_config.cycle_enabled);
    settings_set_change_callback(settings_changed_cb);

    lv_timer_create(status_bar_timer_cb, 1000, nullptr);

    keyboard_input_start();
    input_nav_init(&aircraft_list);
    lv_timer_create(input_timer_cb, 50, nullptr);

    error_log_init();
    enrichment_init();
    fetcher_init(&aircraft_list);

    if (xTaskCreate(lvgl_task, "adsb_lvgl", 12288, nullptr, 3, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create the LVGL task");
        return;
    }

    // The largest free internal block is the number that matters: the SDIO link
    // to the WiFi coprocessor needs contiguous internal/DMA memory.
    ESP_LOGI(TAG,
             "ADS-B ready: internal heap %lu bytes free (largest block %lu), PSRAM %lu bytes free",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
