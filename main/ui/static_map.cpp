// Map backgrounds come from a file, not from the app binary.
//
// The original adsb project generated src/ui/static_map_data.h and compiled the
// pixels in: a new location meant a rebuild, and ~1.7 MB of the app partition
// was nothing but map tiles. Here the generator writes a flat binary that the
// app reads off the SD card into PSRAM at startup, so a map is just a file and
// the app partition stays code.
//
// File layout (little endian, written by tools/generate_static_map.py):
//
//   char    magic[8]      "ADSBMAP1"
//   uint32  version       1
//   int32   lat_e6        centre latitude  * 1e6
//   int32   lon_e6        centre longitude * 1e6
//   uint32  width
//   uint32  height
//   uint32  levels
//   then for each level:
//       int32   radius_nm   the range level this image belongs to
//       int32   osm_zoom    the OSM zoom it was rendered at (informational)
//       uint32  data_size   width * height * 2
//       uint8   pixels[data_size]   RGB565, little endian, stride = width * 2

#include "static_map.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "../config.h"
#include "geo.h"

static char const TAG[] = "adsb map";

#define MAP_PATH_SD "/sd/adsb/map.bin"
#define MAP_PATH_INT "/int/adsb/map.bin"

#define MAP_MAGIC "ADSBMAP1"
#define MAP_VERSION 1u
#define MAP_MAX_LEVELS 8
// Guards against a truncated or hand-edited file eating all of PSRAM.
#define MAP_MAX_LEVEL_BYTES (8u * 1024 * 1024)

// The file is a fixed viewport around one location. Once HOME has moved further
// than this the pixels no longer describe what is on screen, so they are not
// drawn at all rather than drawn wrongly.
#define MAP_MAX_CENTER_DEG 0.5f
// Below this the map is close enough to HOME to be worth drawing, but worth
// mentioning in the log.
#define MAP_WARN_CENTER_DEG 0.01f

struct MapFileHeader {
    char magic[8];
    uint32_t version;
    int32_t lat_e6;
    int32_t lon_e6;
    uint32_t width;
    uint32_t height;
    uint32_t levels;
};
static_assert(sizeof(MapFileHeader) == 32, "map file header layout");

struct MapLevelHeader {
    int32_t radius_nm;
    int32_t osm_zoom;
    uint32_t data_size;
};
static_assert(sizeof(MapLevelHeader) == 12, "map level header layout");

struct MapLevel {
    int radius_nm;
    lv_image_dsc_t dsc;
};

static MapLevel _levels[MAP_MAX_LEVELS];
static int _map_w = 0;
static int _map_h = 0;
// Levels that are completely read. Published with release semantics after each
// level is filled in, so the LVGL task never sees a half-written descriptor.
static volatile int _level_count = 0;
static bool _started = false;

static void *map_malloc(size_t size) {
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static void publish_level(int index, int radius_nm, uint8_t *pixels, size_t size) {
    lv_image_dsc_t *dsc = &_levels[index].dsc;
    memset(dsc, 0, sizeof(*dsc));
    _levels[index].radius_nm = radius_nm;

    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.w = (uint32_t)_map_w;
    dsc->header.h = (uint32_t)_map_h;
    dsc->header.stride = (uint32_t)_map_w * 2;
    dsc->data_size = (uint32_t)size;
    dsc->data = pixels;

    __atomic_store_n(&_level_count, index + 1, __ATOMIC_RELEASE);
}

static bool load_levels(FILE *file, MapFileHeader const &hdr, size_t level_bytes) {
    for (uint32_t i = 0; i < hdr.levels; i++) {
        MapLevelHeader level;
        if (fread(&level, sizeof(level), 1, file) != 1) {
            ESP_LOGE(TAG, "Level %u header truncated", (unsigned)i);
            return false;
        }
        if (level.data_size != level_bytes) {
            ESP_LOGE(TAG, "Level %u is %lu bytes, expected %lu", (unsigned)i,
                     (unsigned long)level.data_size, (unsigned long)level_bytes);
            return false;
        }

        uint8_t *pixels = (uint8_t *)map_malloc(level_bytes);
        if (!pixels) {
            ESP_LOGE(TAG, "No memory for map level %u (%lu bytes)", (unsigned)i,
                     (unsigned long)level_bytes);
            return false;
        }
        if (fread(pixels, 1, level_bytes, file) != level_bytes) {
            ESP_LOGE(TAG, "Level %u pixels truncated", (unsigned)i);
            heap_caps_free(pixels);
            return false;
        }

        publish_level((int)i, level.radius_nm, pixels, level_bytes);
        ESP_LOGI(TAG, "Map level %d nm ready (%lu bytes, %d levels so far)", level.radius_nm,
                 (unsigned long)level_bytes, _level_count);
    }
    return true;
}

static bool check_center(MapFileHeader const &hdr) {
    float const lat = (float)hdr.lat_e6 / 1000000.0f;
    float const lon = (float)hdr.lon_e6 / 1000000.0f;
    float const dlat = fabsf(lat - (float)HOME_LAT);
    float const dlon = fabsf(lon - (float)HOME_LON);

    if (dlat > MAP_MAX_CENTER_DEG || dlon > MAP_MAX_CENTER_DEG) {
        ESP_LOGW(TAG,
                 "Map is centred on %.4f/%.4f but HOME is %.4f/%.4f, not using it. "
                 "Regenerate it for the current location.",
                 lat, lon, (float)HOME_LAT, (float)HOME_LON);
        return false;
    }
    if (dlat > MAP_WARN_CENTER_DEG || dlon > MAP_WARN_CENTER_DEG) {
        ESP_LOGW(TAG, "Map is centred %.1f nm from HOME, it will not line up exactly",
                 MapProjection::distance_nm((float)HOME_LAT, (float)HOME_LON, lat, lon));
    }
    return true;
}

static void map_load_task(void *param) {
    char const *path = MAP_PATH_SD;
    FILE *file = fopen(path, "rb");
    if (!file) {
        path = MAP_PATH_INT;
        file = fopen(path, "rb");
    }
    if (!file) {
        ESP_LOGI(TAG, "No map file at %s or %s, map view stays plain", MAP_PATH_SD, MAP_PATH_INT);
        vTaskDelete(nullptr);
    }

    MapFileHeader hdr = {};
    bool ok = fread(&hdr, sizeof(hdr), 1, file) == 1;
    if (!ok || memcmp(hdr.magic, MAP_MAGIC, sizeof(hdr.magic)) != 0) {
        ESP_LOGE(TAG, "%s is not an ADS-B map file", path);
        ok = false;
    } else if (hdr.version != MAP_VERSION) {
        ESP_LOGE(TAG, "%s is version %lu, this app understands version %u", path,
                 (unsigned long)hdr.version, (unsigned)MAP_VERSION);
        ok = false;
    } else if (hdr.levels == 0 || hdr.levels > MAP_MAX_LEVELS) {
        ESP_LOGE(TAG, "%s has %lu zoom levels", path, (unsigned long)hdr.levels);
        ok = false;
    } else if (hdr.width == 0 || hdr.height == 0 || hdr.width > 4096 || hdr.height > 4096) {
        ESP_LOGE(TAG, "%s has a silly size %lux%lu", path, (unsigned long)hdr.width,
                 (unsigned long)hdr.height);
        ok = false;
    }

    size_t const level_bytes = (size_t)hdr.width * (size_t)hdr.height * 2;
    if (ok && level_bytes > MAP_MAX_LEVEL_BYTES) {
        ESP_LOGE(TAG, "%s wants %lu bytes per level, too much", path, (unsigned long)level_bytes);
        ok = false;
    }
    if (ok) {
        _map_w = (int)hdr.width;
        _map_h = (int)hdr.height;
        ESP_LOGI(TAG, "Loading %s: %dx%d, %lu zoom levels", path, _map_w, _map_h,
                 (unsigned long)hdr.levels);
        ok = check_center(hdr) && load_levels(file, hdr, level_bytes);
    }

    fclose(file);

    if (!ok) {
        // Free whatever made it in so a broken file does not leak PSRAM.
        for (int i = 0; i < _level_count; i++) {
            heap_caps_free((void *)_levels[i].dsc.data);
            _levels[i].dsc.data = nullptr;
        }
        __atomic_store_n(&_level_count, 0, __ATOMIC_RELEASE);
    } else {
        ESP_LOGI(TAG, "Map loaded: %d zoom levels, PSRAM %lu bytes free", _level_count,
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }

    vTaskDelete(nullptr);
}

void static_map_start(void) {
    if (_started) return;
    _started = true;

    if (xTaskCreate(map_load_task, "adsb_map", 8192, nullptr, 1, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create the map loader task");
        _started = false;
    }
}

int static_map_ready_count(void) {
    return __atomic_load_n(&_level_count, __ATOMIC_ACQUIRE);
}

const lv_image_dsc_t *static_map_for_radius(int radius_nm) {
    int const count = static_map_ready_count();
    for (int i = 0; i < count; i++) {
        if (_levels[i].radius_nm == radius_nm) return &_levels[i].dsc;
    }
    return nullptr;
}

void static_map_size(int &out_w, int &out_h) {
    out_w = _map_w;
    out_h = _map_h;
}
