#include "arduino_compat.h"
#include "settings.h"
#include "display_config.h"
#include "../data/storage.h"
#include <cstdio>
#include <cstdlib>

static lv_obj_t *_overlay = nullptr;
static lv_obj_t *_panel = nullptr;
static lv_group_t *_group = nullptr;
static bool _visible = false;

// Text areas
static lv_obj_t *_ta_lat = nullptr;
static lv_obj_t *_ta_lon = nullptr;

// Controls
static lv_obj_t *_slider_radius = nullptr;
static lv_obj_t *_radius_label = nullptr;
static lv_obj_t *_sw_metric = nullptr;
static lv_obj_t *_sw_alert_mil = nullptr;
static lv_obj_t *_sw_alert_emg = nullptr;
static lv_obj_t *_sw_autofocus = nullptr;
static lv_obj_t *_sw_trails = nullptr;
static lv_obj_t *_slider_trail_len = nullptr;
static lv_obj_t *_trail_len_label = nullptr;
static lv_obj_t *_sw_cycle = nullptr;
static lv_obj_t *_slider_cycle_int = nullptr;
static lv_obj_t *_cycle_int_label = nullptr;
static lv_obj_t *_save_btn = nullptr;

static UserConfig _cfg;

// Callback for config changes (set by main)
static settings_changed_cb_t _on_change = nullptr;

#define PANEL_W 500
#define PANEL_H (LCD_V_RES - 40)
#define FIELD_W 200
#define LABEL_COLOR lv_color_hex(0x8888aa)
#define BG_COLOR lv_color_hex(0x12122a)
#define ACCENT_COLOR lv_color_hex(0x00cc66)

static lv_obj_t *create_label(lv_obj_t *parent, const char *text, int x, int y) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, LABEL_COLOR, 0);
    lv_obj_set_style_text_font(lbl, HERSHEY_14, 0);
    lv_obj_set_pos(lbl, x, y);
    return lbl;
}

// Keyboard focus is the only way to see which widget is active, so make it loud.
static void style_focus(lv_obj_t *obj) {
    lv_obj_set_style_outline_color(obj, ACCENT_COLOR, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(obj, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(obj, 3, LV_STATE_FOCUSED);
}

static lv_obj_t *create_textarea(lv_obj_t *parent, const char *placeholder, const char *value, int x, int y) {
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_size(ta, FIELD_W, 36);
    lv_obj_set_pos(ta, x, y);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_textarea_set_text(ta, value);

    lv_obj_set_style_bg_color(ta, lv_color_hex(0x1a1a3a), 0);
    lv_obj_set_style_text_color(ta, lv_color_white(), 0);
    lv_obj_set_style_text_font(ta, HERSHEY_14, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(0x333366), 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_border_color(ta, ACCENT_COLOR, LV_STATE_FOCUSED);
    style_focus(ta);
    return ta;
}

static lv_obj_t *create_switch(lv_obj_t *parent, int x, int y, bool checked) {
    lv_obj_t *sw = lv_switch_create(parent);
    lv_obj_set_pos(sw, x, y);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x333366), 0);
    lv_obj_set_style_bg_color(sw, ACCENT_COLOR,
                              (lv_style_selector_t)LV_PART_INDICATOR | (lv_style_selector_t)LV_STATE_CHECKED);
    style_focus(sw);
    if (checked) lv_obj_add_state(sw, LV_STATE_CHECKED);
    return sw;
}

static lv_obj_t *create_slider(lv_obj_t *parent, int x, int y, int min, int max, int value) {
    lv_obj_t *slider = lv_slider_create(parent);
    lv_obj_set_size(slider, 150, 10);
    lv_obj_set_pos(slider, x, y);
    lv_slider_set_range(slider, min, max);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x333366), 0);
    lv_obj_set_style_bg_color(slider, ACCENT_COLOR, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, ACCENT_COLOR, LV_PART_KNOB);
    style_focus(slider);
    return slider;
}

static lv_obj_t *create_value_label(lv_obj_t *parent, const char *fmt, int value, int x, int y) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text_fmt(lbl, fmt, value);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, HERSHEY_14, 0);
    lv_obj_set_pos(lbl, x, y);
    return lbl;
}

static void save_and_close(lv_event_t *e) {
    (void)e;

    _cfg.home_lat = strtof(lv_textarea_get_text(_ta_lat), nullptr);
    _cfg.home_lon = strtof(lv_textarea_get_text(_ta_lon), nullptr);
    _cfg.radius_nm = lv_slider_get_value(_slider_radius);
    _cfg.use_metric = lv_obj_has_state(_sw_metric, LV_STATE_CHECKED);
    _cfg.alert_military = lv_obj_has_state(_sw_alert_mil, LV_STATE_CHECKED);
    _cfg.alert_emergency = lv_obj_has_state(_sw_alert_emg, LV_STATE_CHECKED);
    _cfg.alert_autofocus = lv_obj_has_state(_sw_autofocus, LV_STATE_CHECKED);
    _cfg.trails_enabled = lv_obj_has_state(_sw_trails, LV_STATE_CHECKED);
    _cfg.trail_max_points = lv_slider_get_value(_slider_trail_len);
    _cfg.cycle_enabled = lv_obj_has_state(_sw_cycle, LV_STATE_CHECKED);
    _cfg.cycle_interval_s = lv_slider_get_value(_slider_cycle_int);

    storage_save_config(_cfg);
    Serial.println("Config saved to NVS");

    if (_on_change) _on_change(&_cfg);

    settings_hide();
}

void settings_init(lv_obj_t *parent) {
    // Semi-transparent overlay
    _overlay = lv_obj_create(parent);
    lv_obj_set_size(_overlay, LCD_H_RES, LCD_V_RES);
    lv_obj_set_pos(_overlay, 0, 0);
    lv_obj_set_style_bg_color(_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(_overlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(_overlay, 0, 0);
    lv_obj_set_style_radius(_overlay, 0, 0);
    lv_obj_clear_flag(_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_overlay, LV_OBJ_FLAG_HIDDEN);

    // Tap overlay background to close
    lv_obj_add_event_cb(_overlay, [](lv_event_t *e) {
        if (lv_event_get_target_obj(e) == _overlay) settings_hide();
    }, LV_EVENT_CLICKED, nullptr);

    // Settings panel (centered)
    _panel = lv_obj_create(_overlay);
    lv_obj_set_size(_panel, PANEL_W, PANEL_H);
    lv_obj_align(_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(_panel, BG_COLOR, 0);
    lv_obj_set_style_bg_opa(_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(_panel, 12, 0);
    lv_obj_set_style_border_color(_panel, lv_color_hex(0x333366), 0);
    lv_obj_set_style_border_width(_panel, 1, 0);
    lv_obj_set_style_pad_all(_panel, 20, 0);
    lv_obj_clear_flag(_panel, LV_OBJ_FLAG_SCROLLABLE);

    // Title
    lv_obj_t *title = lv_label_create(_panel);
    lv_label_set_text(title, LV_SYMBOL_SETTINGS "  Settings");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, HERSHEY_20, 0);
    lv_obj_set_pos(title, 0, 0);

    // Load current config
    _cfg = storage_load_config();

    _group = lv_group_create();

    // === LEFT SIDE (x=0) ===

    // Radius
    create_label(_panel, "Default Radius", 0, 36);
    _slider_radius = create_slider(_panel, 0, 56, 5, 150, _cfg.radius_nm);
    _radius_label = create_value_label(_panel, "%d nm", _cfg.radius_nm, 160, 52);
    lv_obj_add_event_cb(_slider_radius, [](lv_event_t *e) {
        int val = lv_slider_get_value(lv_event_get_target_obj(e));
        lv_label_set_text_fmt(_radius_label, "%d nm", val);
    }, LV_EVENT_VALUE_CHANGED, nullptr);

    // Metric
    create_label(_panel, "Metric Units", 0, 100);
    _sw_metric = create_switch(_panel, 110, 98, _cfg.use_metric);

    // Alert toggles
    create_label(_panel, "Mil Alerts", 0, 134);
    _sw_alert_mil = create_switch(_panel, 110, 132, _cfg.alert_military);

    create_label(_panel, "Emg Alerts", 0, 166);
    _sw_alert_emg = create_switch(_panel, 110, 164, _cfg.alert_emergency);

    create_label(_panel, "Alert Focus", 0, 198);
    _sw_autofocus = create_switch(_panel, 110, 196, _cfg.alert_autofocus);

    // Key hints
    lv_obj_t *hint = lv_label_create(_panel);
    lv_label_set_text(hint, "Tab: next field\nEnter: activate\nEsc: close");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x666688), 0);
    lv_obj_set_style_text_font(hint, HERSHEY_14, 0);
    lv_obj_set_pos(hint, 0, 250);

    // === RIGHT SIDE (x=240) ===
    int rx = 240;

    // Location
    char lat_str[16], lon_str[16];
    snprintf(lat_str, sizeof(lat_str), "%.4f", _cfg.home_lat);
    snprintf(lon_str, sizeof(lon_str), "%.4f", _cfg.home_lon);

    create_label(_panel, "Home Latitude", rx, 36);
    _ta_lat = create_textarea(_panel, "40.7128", lat_str, rx, 54);

    create_label(_panel, "Home Longitude", rx, 96);
    _ta_lon = create_textarea(_panel, "-74.0060", lon_str, rx, 114);

    // Trails
    create_label(_panel, "Aircraft Trails", rx, 158);
    _sw_trails = create_switch(_panel, rx + 120, 156, _cfg.trails_enabled);

    create_label(_panel, "Trail Length", rx, 190);
    _slider_trail_len = create_slider(_panel, rx, 210, 10, 60, _cfg.trail_max_points);
    _trail_len_label = create_value_label(_panel, "%d pts", _cfg.trail_max_points, rx + 160, 206);
    lv_obj_add_event_cb(_slider_trail_len, [](lv_event_t *e) {
        int val = lv_slider_get_value(lv_event_get_target_obj(e));
        lv_label_set_text_fmt(_trail_len_label, "%d pts", val);
    }, LV_EVENT_VALUE_CHANGED, nullptr);

    // Auto-cycle
    create_label(_panel, "Auto-Cycle Views", rx, 240);
    _sw_cycle = create_switch(_panel, rx + 140, 238, _cfg.cycle_enabled);

    create_label(_panel, "Cycle Interval", rx, 272);
    _slider_cycle_int = create_slider(_panel, rx, 292, 15, 120, _cfg.cycle_interval_s);
    _cycle_int_label = create_value_label(_panel, "%ds", _cfg.cycle_interval_s, rx + 160, 288);
    lv_obj_add_event_cb(_slider_cycle_int, [](lv_event_t *e) {
        int val = lv_slider_get_value(lv_event_get_target_obj(e));
        lv_label_set_text_fmt(_cycle_int_label, "%ds", val);
    }, LV_EVENT_VALUE_CHANGED, nullptr);

    // === Save button (centered at bottom) ===
    _save_btn = lv_button_create(_panel);
    lv_obj_set_size(_save_btn, 120, 40);
    lv_obj_align(_save_btn, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(_save_btn, ACCENT_COLOR, 0);
    lv_obj_set_style_radius(_save_btn, 8, 0);
    style_focus(_save_btn);

    lv_obj_t *save_label = lv_label_create(_save_btn);
    lv_label_set_text(save_label, "Save");
    lv_obj_set_style_text_color(save_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(save_label, HERSHEY_16, 0);
    lv_obj_center(save_label);

    lv_obj_add_event_cb(_save_btn, save_and_close, LV_EVENT_CLICKED, nullptr);

    // Tab order: left column top-to-bottom, then right column, then Save
    lv_group_add_obj(_group, _slider_radius);
    lv_group_add_obj(_group, _sw_metric);
    lv_group_add_obj(_group, _sw_alert_mil);
    lv_group_add_obj(_group, _sw_alert_emg);
    lv_group_add_obj(_group, _sw_autofocus);
    lv_group_add_obj(_group, _ta_lat);
    lv_group_add_obj(_group, _ta_lon);
    lv_group_add_obj(_group, _sw_trails);
    lv_group_add_obj(_group, _slider_trail_len);
    lv_group_add_obj(_group, _sw_cycle);
    lv_group_add_obj(_group, _slider_cycle_int);
    lv_group_add_obj(_group, _save_btn);
}

void settings_show() {
    if (_visible) return;
    _visible = true;

    // Reload config in case it changed
    _cfg = storage_load_config();

    char lat_str[16], lon_str[16];
    snprintf(lat_str, sizeof(lat_str), "%.4f", _cfg.home_lat);
    snprintf(lon_str, sizeof(lon_str), "%.4f", _cfg.home_lon);
    lv_textarea_set_text(_ta_lat, lat_str);
    lv_textarea_set_text(_ta_lon, lon_str);

    lv_slider_set_value(_slider_radius, _cfg.radius_nm, LV_ANIM_OFF);
    lv_label_set_text_fmt(_radius_label, "%d nm", _cfg.radius_nm);

    if (_cfg.use_metric) lv_obj_add_state(_sw_metric, LV_STATE_CHECKED);
    else lv_obj_clear_state(_sw_metric, LV_STATE_CHECKED);

    if (_cfg.alert_military) lv_obj_add_state(_sw_alert_mil, LV_STATE_CHECKED);
    else lv_obj_clear_state(_sw_alert_mil, LV_STATE_CHECKED);

    if (_cfg.alert_emergency) lv_obj_add_state(_sw_alert_emg, LV_STATE_CHECKED);
    else lv_obj_clear_state(_sw_alert_emg, LV_STATE_CHECKED);

    if (_cfg.alert_autofocus) lv_obj_add_state(_sw_autofocus, LV_STATE_CHECKED);
    else lv_obj_clear_state(_sw_autofocus, LV_STATE_CHECKED);

    if (_cfg.trails_enabled) lv_obj_add_state(_sw_trails, LV_STATE_CHECKED);
    else lv_obj_clear_state(_sw_trails, LV_STATE_CHECKED);

    lv_slider_set_value(_slider_trail_len, _cfg.trail_max_points, LV_ANIM_OFF);
    lv_label_set_text_fmt(_trail_len_label, "%d pts", _cfg.trail_max_points);

    if (_cfg.cycle_enabled) lv_obj_add_state(_sw_cycle, LV_STATE_CHECKED);
    else lv_obj_clear_state(_sw_cycle, LV_STATE_CHECKED);

    lv_slider_set_value(_slider_cycle_int, _cfg.cycle_interval_s, LV_ANIM_OFF);
    lv_label_set_text_fmt(_cycle_int_label, "%ds", _cfg.cycle_interval_s);

    lv_obj_clear_flag(_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_group_focus_obj(_slider_radius);
}

void settings_hide() {
    if (!_visible) return;
    _visible = false;
    lv_obj_add_flag(_overlay, LV_OBJ_FLAG_HIDDEN);
}

bool settings_is_visible() {
    return _visible;
}

lv_group_t *settings_get_group() {
    return _group;
}

void settings_set_change_callback(settings_changed_cb_t cb) {
    _on_change = cb;
}
