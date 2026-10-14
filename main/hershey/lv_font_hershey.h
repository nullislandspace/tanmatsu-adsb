// =====================================================================
//  Hershey vector font as an LVGL font
// ---------------------------------------------------------------------
//  Four sizes, named the way LVGL's built-in fonts are so they can be
//  declared with LV_FONT_DECLARE and used as LV_FONT_DEFAULT.
//
//  This header is also the header named by CONFIG_LV_FONT_CUSTOM_INCLUDE,
//  so it is pulled in by lv_conf_internal.h in every LVGL translation
//  unit. That means it may only define macros here -- no types, no
//  declarations -- and LV_FONT_CUSTOM_DECLARE is what declares the fonts
//  once lv_font.h has defined lv_font_t.
// =====================================================================

#ifndef LV_FONT_HERSHEY_H
#define LV_FONT_HERSHEY_H

#define LV_FONT_CUSTOM_DECLARE        \
    LV_FONT_DECLARE(lv_font_hershey_14)  \
    LV_FONT_DECLARE(lv_font_hershey_16)  \
    LV_FONT_DECLARE(lv_font_hershey_20)  \
    LV_FONT_DECLARE(lv_font_hershey_28)

#define HERSHEY_14 (&lv_font_hershey_14)
#define HERSHEY_16 (&lv_font_hershey_16)
#define HERSHEY_20 (&lv_font_hershey_20)
#define HERSHEY_28 (&lv_font_hershey_28)

// lv_conf_internal.h has already picked a built-in default font by the time
// it includes this header, so override it here rather than from the command
// line. Widgets that do not name a font get Hershey too.
#undef LV_FONT_DEFAULT
#define LV_FONT_DEFAULT HERSHEY_14

#endif // LV_FONT_HERSHEY_H
