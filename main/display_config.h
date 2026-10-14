#pragma once

// Tanmatsu drawing surface.
// The ST7701 MIPI-DSI panel is 480x800 in portrait and the BSP default rotation is 270 degrees,
// so LVGL renders landscape 800x480 and the flush callback rotates into the panel framebuffer.
#define LCD_H_RES 800
#define LCD_V_RES 480
