#include "storage.h"
#include "config.h"
#include "arduino_compat.h"

#include <cstring>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static char const TAG[] = "adsb storage";

#define NVS_NAMESPACE "adsb"

// NVS has no native float type, coordinates are stored as micro-degrees.
#define COORD_SCALE 1000000.0f

UserConfig g_config = {};

static bool nvs_get_int(nvs_handle_t handle, const char* key, int* out) {
    int32_t value = 0;
    if (nvs_get_i32(handle, key, &value) != ESP_OK) {
        return false;
    }
    *out = (int)value;
    return true;
}

static bool nvs_get_coord(nvs_handle_t handle, const char* key, float* out) {
    int32_t value = 0;
    if (nvs_get_i32(handle, key, &value) != ESP_OK) {
        return false;
    }
    *out = (float)value / COORD_SCALE;
    return true;
}

// Booleans are stored as 0/1 so the whole config lives in one key type.
static bool nvs_get_bool(nvs_handle_t handle, const char* key, bool* out) {
    int32_t value = 0;
    if (nvs_get_i32(handle, key, &value) != ESP_OK) {
        return false;
    }
    *out = value != 0;
    return true;
}

UserConfig storage_load_config() {
    UserConfig cfg = {};

    // Compiled defaults
    cfg.home_lat          = HOME_LAT;
    cfg.home_lon          = HOME_LON;
    cfg.radius_nm         = ADSB_RADIUS_NM;
    cfg.use_metric        = false;
    cfg.watchlist_count   = 0;
    cfg.alert_military    = true;
    cfg.alert_emergency   = true;
    cfg.alert_autofocus   = true;
    cfg.cycle_enabled     = true;
    cfg.cycle_interval_s  = 15;
    cfg.cycle_inactivity_s = 60;
    cfg.trails_enabled    = true;
    cfg.trail_max_points  = 30;
    cfg.trail_style       = 0;
    cfg.map_zoom_idx      = 1;      // 50nm default
    cfg.radar_zoom_idx    = 0;      // 100nm default
    cfg.arrivals_filter_idx = 4;    // ALL default

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "NVS namespace %s not available, using defaults", NVS_NAMESPACE);
        return cfg;
    }

    // Override with NVS values where they exist
    nvs_get_coord(handle, "lat", &cfg.home_lat);
    nvs_get_coord(handle, "lon", &cfg.home_lon);
    nvs_get_int(handle, "radius", &cfg.radius_nm);
    nvs_get_bool(handle, "metric", &cfg.use_metric);
    nvs_get_bool(handle, "alrt_mil", &cfg.alert_military);
    nvs_get_bool(handle, "alrt_emg", &cfg.alert_emergency);
    nvs_get_bool(handle, "alrt_af", &cfg.alert_autofocus);
    nvs_get_bool(handle, "cyc_on", &cfg.cycle_enabled);
    nvs_get_int(handle, "cyc_int", &cfg.cycle_interval_s);
    nvs_get_int(handle, "cyc_idle", &cfg.cycle_inactivity_s);
    nvs_get_bool(handle, "trail_on", &cfg.trails_enabled);
    nvs_get_int(handle, "trail_pts", &cfg.trail_max_points);
    nvs_get_int(handle, "trail_sty", &cfg.trail_style);
    nvs_get_int(handle, "map_zoom", &cfg.map_zoom_idx);
    nvs_get_int(handle, "rdr_zoom", &cfg.radar_zoom_idx);
    nvs_get_int(handle, "arr_filt", &cfg.arrivals_filter_idx);

    nvs_close(handle);
    ESP_LOGI(TAG, "Config loaded from NVS");
    return cfg;
}

void storage_save_config(const UserConfig& cfg) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace %s for writing", NVS_NAMESPACE);
        return;
    }

    nvs_set_i32(handle, "lat", (int32_t)(cfg.home_lat * COORD_SCALE));
    nvs_set_i32(handle, "lon", (int32_t)(cfg.home_lon * COORD_SCALE));
    nvs_set_i32(handle, "radius", cfg.radius_nm);
    nvs_set_i32(handle, "metric", cfg.use_metric ? 1 : 0);
    nvs_set_i32(handle, "alrt_mil", cfg.alert_military ? 1 : 0);
    nvs_set_i32(handle, "alrt_emg", cfg.alert_emergency ? 1 : 0);
    nvs_set_i32(handle, "alrt_af", cfg.alert_autofocus ? 1 : 0);
    nvs_set_i32(handle, "cyc_on", cfg.cycle_enabled ? 1 : 0);
    nvs_set_i32(handle, "cyc_int", cfg.cycle_interval_s);
    nvs_set_i32(handle, "cyc_idle", cfg.cycle_inactivity_s);
    nvs_set_i32(handle, "trail_on", cfg.trails_enabled ? 1 : 0);
    nvs_set_i32(handle, "trail_pts", cfg.trail_max_points);
    nvs_set_i32(handle, "trail_sty", cfg.trail_style);
    nvs_set_i32(handle, "map_zoom", cfg.map_zoom_idx);
    nvs_set_i32(handle, "rdr_zoom", cfg.radar_zoom_idx);
    nvs_set_i32(handle, "arr_filt", cfg.arrivals_filter_idx);

    esp_err_t res = nvs_commit(handle);
    nvs_close(handle);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit config: %s", esp_err_to_name(res));
    } else {
        ESP_LOGI(TAG, "Config saved to NVS");
    }
}
