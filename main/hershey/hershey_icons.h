// =====================================================================
//  LVGL's symbol codepoints, drawn as Hershey strokes
// ---------------------------------------------------------------------
//  LV_SYMBOL_* are codepoints in a private area that only a symbol font
//  knows about. Hershey does not have them, so the handful this app uses
//  get their own stroke data here, in the same font units as the glyphs
//  (x from the pen, y up from the baseline, cap height 21, (-1,-1) = pen
//  up). They are line drawings, not the filled LVGL icons, which is the
//  point: one alphabet for the whole UI.
// =====================================================================

#ifndef HERSHEY_ICONS_H
#define HERSHEY_ICONS_H

#include <stdint.h>

typedef struct {
    uint32_t      cp;
    int8_t        adv;
    int8_t const* pts;
    uint8_t       n;   // (x, y) pairs
} hershey_icon_t;

// LV_SYMBOL_WIFI
static int8_t const HersheyIconWifi[] = {
    10,  3, 11,  2, 12,  3, 11,  4, 10,  3,   // the dot
    -1, -1,
     7,  7, 11, 11, 15,  7,                    // inner arc
    -1, -1,
     4, 11, 11, 16, 18, 11,                    // middle arc
    -1, -1,
     1, 15, 11, 21, 21, 15,                    // outer arc
};

// LV_SYMBOL_SETTINGS
static int8_t const HersheyIconGear[] = {
    21, 11, 18, 14, 18, 18, 14, 18, 11, 21,    // eight teeth
     8, 18,  4, 18,  4, 14,  1, 11,  4,  8,
     4,  4,  8,  4, 11,  1, 14,  4, 18,  4,
    18,  8, 21, 11,
    -1, -1,
    15, 11, 14, 14, 11, 15,  8, 14,  7, 11,    // the hole
     8,  8, 11,  7, 14,  8, 15, 11,
};

// LV_SYMBOL_WARNING
static int8_t const HersheyIconWarning[] = {
     1,  1, 11, 21, 21,  1,  1,  1,            // the triangle
    -1, -1,
    11,  6, 11, 14,                            // the stroke
    -1, -1,
    10,  4, 11,  3, 12,  4, 11,  5, 10,  4,    // the dot
};

// LV_SYMBOL_GPS
static int8_t const HersheyIconGps[] = {
    21, 11, 18, 18, 11, 21,  4, 18,  1, 11,    // the circle
     4,  4, 11,  1, 18,  4, 21, 11,
    -1, -1,
    11, 21, 11, 15,                            // the four ticks
    -1, -1,
    11,  7, 11,  1,
    -1, -1,
     1, 11,  7, 11,
    -1, -1,
    15, 11, 21, 11,
};

// LV_SYMBOL_EYE_OPEN
static int8_t const HersheyIconEye[] = {
     1, 11,  6, 16, 16, 16, 21, 11,            // the lids
    16,  6,  6,  6,  1, 11,
    -1, -1,
    15, 11, 14, 14, 11, 15,  8, 14,  7, 11,    // the pupil
     8,  8, 11,  7, 14,  8, 15, 11,
};

// LV_SYMBOL_OK
static int8_t const HersheyIconOk[] = {
     2, 10,  8,  3, 20, 19,
};

// LV_SYMBOL_BELL
static int8_t const HersheyIconBell[] = {
     3,  4,  3, 12,  5, 17,  9, 19, 13, 19,    // the body
    17, 17, 19, 12, 19,  4,
    -1, -1,
     1,  4, 21,  4,                            // the rim
    -1, -1,
    10,  2, 11,  1, 12,  2, 11,  3, 10,  2,    // the clapper
};

#define HERSHEY_ICON_COUNT 7

static hershey_icon_t const HersheyIcons[HERSHEY_ICON_COUNT] = {
    { 0xF00C, 22, HersheyIconOk,      3  },  // LV_SYMBOL_OK
    { 0xF013, 22, HersheyIconGear,    27 },  // LV_SYMBOL_SETTINGS
    { 0xF06E, 22, HersheyIconEye,     17 },  // LV_SYMBOL_EYE_OPEN
    { 0xF071, 22, HersheyIconWarning, 13 },  // LV_SYMBOL_WARNING
    { 0xF0F3, 22, HersheyIconBell,    17 },  // LV_SYMBOL_BELL
    { 0xF124, 22, HersheyIconGps,     21 },  // LV_SYMBOL_GPS
    { 0xF1EB, 22, HersheyIconWifi,    17 },  // LV_SYMBOL_WIFI
};

static inline hershey_icon_t const* hershey_icon_lookup(uint32_t cp) {
    for(int i = 0; i < HERSHEY_ICON_COUNT; i++) {
        if(HersheyIcons[i].cp == cp) return &HersheyIcons[i];
    }
    return NULL;
}

#endif // HERSHEY_ICONS_H
