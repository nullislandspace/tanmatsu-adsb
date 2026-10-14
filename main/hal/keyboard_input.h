#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

// Tanmatsu keyboard front-end for the ADS-B UI.
//
// The original app used a rotary encoder plus two buttons. The Tanmatsu has a
// full keyboard instead, so input is split into two lanes:
//   - navigator lane (view/aircraft selection, consumed by input_nav)
//   - LVGL keypad lane (settings panel widgets, consumed by the indev read cb)

// Start pumping BSP input events. Call once after bsp_device_initialize().
void keyboard_input_start(void);

// Attach the LVGL keypad input device (used to route Tab/Enter to a group).
void keyboard_input_set_indev(lv_indev_t *indev);

// Group that receives focus navigation. Pass nullptr to detach.
void keyboard_input_set_group(lv_group_t *group);

// When true, arrow/tab/enter keys go to LVGL widgets instead of the navigator.
void keyboard_input_set_ui_mode(bool ui_mode);

// Navigator lane, edge detected and auto clearing.
int  input_cycle_delta(void);        // UP -1, DOWN +1
int  input_view_delta(void);         // LEFT -1, RIGHT/TAB +1
bool input_select_pressed(void);     // RETURN or any SPACE key
bool input_back_pressed(void);       // ESC (also BACKSPACE outside the UI)
bool input_function_pressed(int fn); // 1..6, F1..F6

// LVGL keypad indev read callback.
void keyboard_input_lvgl_read_cb(lv_indev_t *indev, lv_indev_data_t *data);
