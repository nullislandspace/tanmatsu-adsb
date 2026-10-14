#include "keyboard_input.h"

#include "../arduino_compat.h"
#include "../ui/views.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

// badge-bsp is plain C with no __cplusplus guard.
extern "C" {
#include "bsp/input.h"
}

static char const TAG[] = "kbd";

// Global "user is interacting" flag, read by view timers to defer heavy rendering.
volatile bool input_active = false;

static QueueHandle_t _queue = nullptr;

static volatile bool _ui_mode = false;
static lv_indev_t*   _indev   = nullptr;
static lv_group_t*   _group   = nullptr;

// --- Navigator lane ---
static volatile int    _cycle_delta = 0;
static volatile int    _view_delta  = 0;
static volatile bool   _select_edge = false;
static volatile bool   _back_edge   = false;
static volatile uint8_t _func_edges = 0;  // bit n set when F(n+1) was pressed
static volatile int    _held_count  = 0;

// --- LVGL keypad lane ---
#define KEY_RING_SIZE 24
static uint32_t         _ring[KEY_RING_SIZE];
static volatile uint8_t _ring_head  = 0;
static volatile uint8_t _ring_tail  = 0;
static bool             _press_sent = false;
static uint32_t         _press_key  = 0;

// --- Key repeat (the BSP does not repeat for us) ---
#define REPEAT_START_MS 450
#define REPEAT_PERIOD_MS 130
static volatile bsp_input_navigation_key_t _held_key = BSP_INPUT_NAVIGATION_KEY_NONE;
static volatile uint32_t                   _repeat_at = 0;

static void push_lv_key(uint32_t key) {
    uint8_t next = (uint8_t)((_ring_head + 1) % KEY_RING_SIZE);
    if (next == _ring_tail) return;  // Ring full: drop the key rather than block
    _ring[_ring_head] = key;
    _ring_head        = next;
}

static uint32_t decode_utf8(char const* text) {
    uint8_t const* p = (uint8_t const*)text;
    if (p == nullptr || p[0] == 0) return 0;

    uint32_t cp   = 0;
    int      left = 0;
    if (p[0] < 0x80) {
        cp = p[0];
    } else if ((p[0] & 0xE0) == 0xC0) {
        cp   = p[0] & 0x1F;
        left = 1;
    } else if ((p[0] & 0xF0) == 0xE0) {
        cp   = p[0] & 0x0F;
        left = 2;
    } else if ((p[0] & 0xF8) == 0xF0) {
        cp   = p[0] & 0x07;
        left = 3;
    } else {
        return 0;
    }

    for (int i = 0; i < left; i++) {
        if ((p[i + 1] & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (uint32_t)(p[i + 1] & 0x3F);
    }
    return cp;
}

static bool is_select_key(bsp_input_navigation_key_t key) {
    return key == BSP_INPUT_NAVIGATION_KEY_RETURN || key == BSP_INPUT_NAVIGATION_KEY_SPACE_L ||
           key == BSP_INPUT_NAVIGATION_KEY_SPACE_M || key == BSP_INPUT_NAVIGATION_KEY_SPACE_R;
}

static bool map_nav_to_lv(bsp_input_navigation_key_t key, uint32_t* out) {
    switch (key) {
        case BSP_INPUT_NAVIGATION_KEY_LEFT: *out = LV_KEY_LEFT; return true;
        case BSP_INPUT_NAVIGATION_KEY_RIGHT: *out = LV_KEY_RIGHT; return true;
        case BSP_INPUT_NAVIGATION_KEY_UP: *out = LV_KEY_UP; return true;
        case BSP_INPUT_NAVIGATION_KEY_DOWN: *out = LV_KEY_DOWN; return true;
        case BSP_INPUT_NAVIGATION_KEY_TAB: *out = LV_KEY_NEXT; return true;
        case BSP_INPUT_NAVIGATION_KEY_BACKSPACE: *out = LV_KEY_BACKSPACE; return true;
        default:
            if (is_select_key(key)) {
                *out = LV_KEY_ENTER;
                return true;
            }
            return false;
    }
}

static bool is_repeatable(bsp_input_navigation_key_t key) {
    switch (key) {
        case BSP_INPUT_NAVIGATION_KEY_LEFT:
        case BSP_INPUT_NAVIGATION_KEY_RIGHT:
        case BSP_INPUT_NAVIGATION_KEY_UP:
        case BSP_INPUT_NAVIGATION_KEY_DOWN:
        case BSP_INPUT_NAVIGATION_KEY_TAB:
        case BSP_INPUT_NAVIGATION_KEY_BACKSPACE: return true;
        default: return false;
    }
}

// Apply a key press to whichever lane is currently active.
static void apply_press(bsp_input_navigation_key_t key) {
    if (_ui_mode) {
        uint32_t lv_key = 0;
        if (map_nav_to_lv(key, &lv_key)) push_lv_key(lv_key);
        return;
    }

    switch (key) {
        case BSP_INPUT_NAVIGATION_KEY_LEFT: _view_delta = _view_delta - 1; break;
        case BSP_INPUT_NAVIGATION_KEY_RIGHT: _view_delta = _view_delta + 1; break;
        case BSP_INPUT_NAVIGATION_KEY_TAB: _view_delta = _view_delta + 1; break;
        case BSP_INPUT_NAVIGATION_KEY_UP: _cycle_delta = _cycle_delta - 1; break;
        case BSP_INPUT_NAVIGATION_KEY_DOWN: _cycle_delta = _cycle_delta + 1; break;
        case BSP_INPUT_NAVIGATION_KEY_BACKSPACE: _back_edge = true; break;
        default:
            if (is_select_key(key)) _select_edge = true;
            break;
    }
}

static void handle_navigation(bsp_input_event_t* event) {
    bsp_input_navigation_key_t key   = event->args_navigation.key;
    bool                       state = event->args_navigation.state;

    if (state) {
        _held_count = _held_count + 1;
        input_active = true;
        _held_key    = key;
        _repeat_at   = millis() + REPEAT_START_MS;
    } else {
        if (_held_count > 0) _held_count = _held_count - 1;
        input_active = _held_count > 0;
        if (_held_key == key) _held_key = BSP_INPUT_NAVIGATION_KEY_NONE;
    }

    if (!state) return;

    // ESC always belongs to the application, never to a widget.
    if (key == BSP_INPUT_NAVIGATION_KEY_ESC) {
        _back_edge = true;
        return;
    }

    if (key >= BSP_INPUT_NAVIGATION_KEY_F1 && key <= BSP_INPUT_NAVIGATION_KEY_F6) {
        _func_edges |= (uint8_t)(1u << (key - BSP_INPUT_NAVIGATION_KEY_F1));
        return;
    }

    if (key == BSP_INPUT_NAVIGATION_KEY_VOLUME_UP || key == BSP_INPUT_NAVIGATION_KEY_VOLUME_DOWN) return;

    apply_press(key);
}

static void handle_keyboard(bsp_input_event_t* event) {
    if (!_ui_mode) return;

    uint32_t cp = decode_utf8(event->args_keyboard.utf8);
    if (cp == 0) cp = (uint8_t)event->args_keyboard.ascii;
    if (cp < 32 || cp == 127) return;  // Control codes are handled as LV_KEY_* above

    push_lv_key(cp);
}

static void input_task(void* arg) {
    (void)arg;
    bsp_input_event_t event;
    TickType_t        wait = pdMS_TO_TICKS(50);

    while (true) {
        if (xQueueReceive(_queue, &event, wait) == pdTRUE) {
            switch (event.type) {
                case INPUT_EVENT_TYPE_NAVIGATION: handle_navigation(&event); break;
                case INPUT_EVENT_TYPE_KEYBOARD: handle_keyboard(&event); break;
                default: break;
            }
        }

        bsp_input_navigation_key_t held = _held_key;
        if (held != BSP_INPUT_NAVIGATION_KEY_NONE && is_repeatable(held)) {
            uint32_t now = millis();
            if (now >= _repeat_at) {
                apply_press(held);
                _repeat_at = now + REPEAT_PERIOD_MS;
                wait       = pdMS_TO_TICKS(REPEAT_PERIOD_MS);
            } else {
                wait = pdMS_TO_TICKS((int)(_repeat_at - now));
            }
        } else {
            wait = pdMS_TO_TICKS(50);
        }
    }
}

void keyboard_input_start(void) {
    ESP_ERROR_CHECK(bsp_input_get_queue(&_queue));
    if (xTaskCreate(input_task, "adsb_kbd", 4096, nullptr, 4, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create input task");
    }
}

void keyboard_input_set_indev(lv_indev_t* indev) {
    _indev = indev;
    if (_indev != nullptr && _group != nullptr) lv_indev_set_group(_indev, _group);
}

void keyboard_input_set_group(lv_group_t* group) {
    _group = group;
    if (_indev != nullptr) lv_indev_set_group(_indev, _group);
}

void keyboard_input_set_ui_mode(bool ui_mode) {
    _ui_mode = ui_mode;
    if (!ui_mode) {
        // Drop anything that was queued for a widget so it cannot leak into the navigator.
        _ring_head     = 0;
        _ring_tail     = 0;
        _press_sent    = false;
        _held_key      = BSP_INPUT_NAVIGATION_KEY_NONE;
        input_active   = false;
        _held_count    = 0;
    }
}

int input_cycle_delta(void) {
    int value       = _cycle_delta;
    _cycle_delta    = 0;
    return value;
}

int input_view_delta(void) {
    int value      = _view_delta;
    _view_delta    = 0;
    return value;
}

bool input_select_pressed(void) {
    bool value      = _select_edge;
    _select_edge    = false;
    return value;
}

bool input_back_pressed(void) {
    bool value   = _back_edge;
    _back_edge   = false;
    return value;
}

bool input_function_pressed(int fn) {
    if (fn < 1 || fn > 6) return false;
    uint8_t bit  = (uint8_t)(1u << (fn - 1));
    bool    hit  = (_func_edges & bit) != 0;
    _func_edges &= (uint8_t)~bit;
    return hit;
}

void keyboard_input_lvgl_read_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    (void)indev;

    if (_press_sent) {
        data->state    = LV_INDEV_STATE_RELEASED;
        data->key      = _press_key;
        _press_sent    = false;
        return;
    }

    if (_ring_tail != _ring_head) {
        uint32_t key   = _ring[_ring_tail];
        _ring_tail     = (uint8_t)((_ring_tail + 1) % KEY_RING_SIZE);
        data->state    = LV_INDEV_STATE_PRESSED;
        data->key      = key;
        _press_key     = key;
        _press_sent    = true;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        data->key   = 0;
    }
}
