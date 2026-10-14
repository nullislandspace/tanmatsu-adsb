#include "enrichment.h"
#include "arduino_compat.h"
#include "http_get.h"
#include "http_mutex.h"

#include <cstdlib>
#include <cstring>

#include "cJSON.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

static char const TAG[] = "adsb enrich";

#define MAX_CACHE 20

// 12 kB of lookup table — PSRAM, internal RAM is needed by the SDIO link.
static EXT_RAM_BSS_ATTR AircraftEnrichment _cache[MAX_CACHE];
static EXT_RAM_BSS_ATTR char               _cache_keys[MAX_CACHE][7];
static int                _cache_count = 0;

static void (*_pending_callback)(AircraftEnrichment*) = nullptr;
static volatile bool _task_running = false;

// Deferred callback — set by task, delivered by LVGL timer
static volatile AircraftEnrichment* _deferred_entry = nullptr;
static volatile bool                _deferred_ready = false;

struct EnrichParams {
    char icao_hex[7];
    char callsign[9];
};

static void json_string(const cJSON* obj, const char* key, char* out, size_t out_len) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring != nullptr) {
        strlcpy(out, item->valuestring, out_len);
    }
}

static int json_int(const cJSON* obj, const char* key, int fallback) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item)) {
        return item->valueint;
    }
    return fallback;
}

AircraftEnrichment* enrichment_get_cached(const char* icao_hex) {
    for (int i = 0; i < _cache_count; i++) {
        if (strcmp(_cache_keys[i], icao_hex) == 0 && _cache[i].loaded) {
            return &_cache[i];
        }
    }
    return nullptr;
}

static AircraftEnrichment* get_or_create_cache_entry(const char* icao_hex) {
    for (int i = 0; i < _cache_count; i++) {
        if (strcmp(_cache_keys[i], icao_hex) == 0) return &_cache[i];
    }
    int idx = _cache_count < MAX_CACHE ? _cache_count++ : 0;
    memset(&_cache[idx], 0, sizeof(AircraftEnrichment));
    strlcpy(_cache_keys[idx], icao_hex, 7);
    return &_cache[idx];
}

// Signal LVGL context to call the callback (thread-safe)
static void notify_callback(AircraftEnrichment* entry) {
    _deferred_entry = entry;
    _deferred_ready = true;
}

// Fetch a JSON document into PSRAM and hand it over to the parser callback
static bool fetch_json(const char* url, void (*apply)(cJSON*, AircraftEnrichment*)) {
    char*  buf    = nullptr;
    size_t len    = 0;
    int    status = http_get_body(url, &buf, &len, 16384);
    if (status != 200 || !buf || len == 0) {
        if (buf) heap_caps_free(buf);
        return false;
    }

    cJSON* doc = cJSON_ParseWithLength(buf, len);
    heap_caps_free(buf);
    if (!doc) {
        return false;
    }
    apply(doc, nullptr);
    cJSON_Delete(doc);
    return true;
}

static void apply_callsign(cJSON* doc, AircraftEnrichment* entry) {
    cJSON* response = cJSON_GetObjectItemCaseSensitive(doc, "response");
    cJSON* route    = cJSON_GetObjectItemCaseSensitive(response, "flightroute");
    cJSON* orig     = cJSON_GetObjectItemCaseSensitive(route, "origin");
    cJSON* dest     = cJSON_GetObjectItemCaseSensitive(route, "destination");
    cJSON* airline  = cJSON_GetObjectItemCaseSensitive(route, "airline");
    json_string(orig, "name", entry->origin_airport, sizeof(entry->origin_airport));
    json_string(dest, "name", entry->destination_airport, sizeof(entry->destination_airport));
    json_string(airline, "name", entry->airline, sizeof(entry->airline));
}

static void apply_aircraft(cJSON* doc, AircraftEnrichment* entry) {
    cJSON* response = cJSON_GetObjectItemCaseSensitive(doc, "response");
    cJSON* ac       = cJSON_GetObjectItemCaseSensitive(response, "aircraft");
    json_string(ac, "manufacturer", entry->manufacturer, sizeof(entry->manufacturer));
    json_string(ac, "type", entry->model, sizeof(entry->model));
    json_string(ac, "registered_owner", entry->owner, sizeof(entry->owner));
    json_string(ac, "registered_owner_country_name", entry->registered_country, sizeof(entry->registered_country));
    entry->engine_count = (uint8_t)json_int(ac, "engine_count", 0);
    json_string(ac, "engine_type", entry->engine_type, sizeof(entry->engine_type));
    entry->year_built = (uint16_t)json_int(ac, "year_built", 0);
}

static void apply_photos(cJSON* doc, AircraftEnrichment* entry) {
    cJSON* photos = cJSON_GetObjectItemCaseSensitive(doc, "photos");
    cJSON* first  = cJSON_GetArrayItem(photos, 0);
    if (!first) return;
    cJSON* thumb = cJSON_GetObjectItemCaseSensitive(first, "thumbnail_large");
    json_string(thumb, "src", entry->photo_url, sizeof(entry->photo_url));
    json_string(first, "photographer", entry->photo_photographer, sizeof(entry->photo_photographer));
}

// Stage helpers need the entry, so bind it through a small indirection
static AircraftEnrichment* _stage_entry = nullptr;

static void stage_callsign(cJSON* doc, AircraftEnrichment*) { apply_callsign(doc, _stage_entry); }
static void stage_aircraft(cJSON* doc, AircraftEnrichment*) { apply_aircraft(doc, _stage_entry); }
static void stage_photos(cJSON* doc, AircraftEnrichment*) { apply_photos(doc, _stage_entry); }

static void fetch_task(void* param) {
    _task_running = true;
    EnrichParams* params = (EnrichParams*)param;
    AircraftEnrichment* entry = get_or_create_cache_entry(params->icao_hex);
    entry->loading = true;
    _stage_entry   = entry;

    char url[128];

    // Stage 1: Full airport names from adsbdb callsign API
    if (params->callsign[0] && http_mutex_acquire(pdMS_TO_TICKS(8000))) {
        snprintf(url, sizeof(url), "https://api.adsbdb.com/v0/callsign/%s", params->callsign);
        if (fetch_json(url, stage_callsign)) {
            notify_callback(entry);
        }
        http_mutex_release();
    }

    // Stage 2: Aircraft details from adsbdb
    if (http_mutex_acquire(pdMS_TO_TICKS(8000))) {
        snprintf(url, sizeof(url), "https://api.adsbdb.com/v0/aircraft/%s", params->icao_hex);
        fetch_json(url, stage_aircraft);
        http_mutex_release();
        notify_callback(entry);
    }

    // Stage 3: Photo from planespotters.net
    vTaskDelay(pdMS_TO_TICKS(200));
    if (http_mutex_acquire(pdMS_TO_TICKS(8000))) {
        snprintf(url, sizeof(url), "https://api.planespotters.net/pub/photos/hex/%s", params->icao_hex);
        fetch_json(url, stage_photos);
        http_mutex_release();
    }

    entry->loaded  = true;
    entry->loading = false;
    notify_callback(entry);

    free(params);
    _task_running = false;
    vTaskDelete(nullptr);
}

void enrichment_fetch(const char* icao_hex, const char* registration, const char* callsign,
                      void (*callback)(AircraftEnrichment* data)) {
    // Check cache first
    AircraftEnrichment* cached = enrichment_get_cached(icao_hex);
    if (cached) {
        callback(cached);
        return;
    }

    // Only one enrichment task at a time
    if (_task_running) {
        ESP_LOGD(TAG, "Skipping enrichment for %s, task already running", icao_hex);
        return;
    }

    _pending_callback = callback;
    _deferred_ready   = false;

    EnrichParams* params = (EnrichParams*)malloc(sizeof(EnrichParams));
    if (!params) return;
    strlcpy(params->icao_hex, icao_hex, sizeof(params->icao_hex));
    strlcpy(params->callsign, callsign ? callsign : "", sizeof(params->callsign));
    xTaskCreatePinnedToCore(fetch_task, "enrich", 12288, params, 0, nullptr, 1);
}

// Call from LVGL context (app startup) to install the deferred callback timer
void enrichment_init() {
    lv_timer_create([](lv_timer_t* t) {
        if (_deferred_ready && _pending_callback && _deferred_entry) {
            _deferred_ready = false;
            _pending_callback((AircraftEnrichment*)_deferred_entry);
        }
    }, 200, nullptr);
}
