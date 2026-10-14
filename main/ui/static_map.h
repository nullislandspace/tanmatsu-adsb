#pragma once

#include "lvgl.h"

// Pre-rendered map backgrounds for the map view.
//
// The original adsb project compiled these into the app as a generated C
// header. Here they are a file on the SD card instead: tools/generate_static_map.py
// writes a flat binary, the app reads it into PSRAM at startup, and changing the
// map is a file copy instead of a rebuild.

// Start loading the map in the background. Safe to call once, from app_main().
void static_map_start(void);

// How many zoom levels have been read and are safe to draw.
int static_map_ready_count(void);

// Image descriptor for one range level in nautical miles, or nullptr if that
// level is not loaded (no map file, or the file does not contain this zoom).
const lv_image_dsc_t *static_map_for_radius(int radius_nm);

// Pixel size of the stored images, needed to scale them onto the canvas.
void static_map_size(int &out_w, int &out_h);
