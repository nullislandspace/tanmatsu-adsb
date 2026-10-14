#include "fetcher.h"
#include "arduino_compat.h"
#include "config.h"
#include "error_log.h"
#include "http_get.h"
#include "http_mutex.h"
#include "storage.h"
#include "../ui/alerts.h"

#include <cstdlib>
#include <cstring>

#include "cJSON.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// wifi-manager is plain C with no __cplusplus guard.
extern "C" {
#include "wifi_connection.h"
}

static char const TAG[] = "adsb fetcher";

#define WIFI_CONNECT_TIMEOUT_MS 30000
#define FETCH_FAIL_RESET_THRESHOLD 10  // consecutive fails before reconnecting
#define MAX_RESPONSE_BYTES (512 * 1024)
// esp-hosted, lwIP and the task stacks all need contiguous internal RAM; warn
// before running out of it rather than after.
#define INTERNAL_HEAP_WARN_BYTES 81920
#define DMA_BLOCK_WARN_BYTES 24576

static NetType _active_net = NET_NONE;

static AircraftList* _aircraft_list = nullptr;
static uint32_t      _last_update   = 0;
static TaskHandle_t  _fetch_task_handle  = nullptr;
static TaskHandle_t  _route_task_handle  = nullptr;
static FetcherStats  _fstats = {};

// Military alert dedup — circular buffer of already-alerted ICAO hexes
#define ALERTED_MAX 64
static char _alerted_hexes[ALERTED_MAX][7];
static int  _alerted_count = 0;
static int  _alerted_write = 0;

static bool already_alerted(const char* hex) {
    for (int i = 0; i < _alerted_count; i++) {
        if (strcmp(_alerted_hexes[i], hex) == 0) return true;
    }
    return false;
}

static void mark_alerted(const char* hex) {
    strlcpy(_alerted_hexes[_alerted_write], hex, 7);
    _alerted_write = (_alerted_write + 1) % ALERTED_MAX;
    if (_alerted_count < ALERTED_MAX) _alerted_count++;
}

// Check if ICAO hex is in known military ranges
static bool check_military(const char* hex) {
    uint32_t h = strtoul(hex, nullptr, 16);
    // US military (DoD, Army, Navy, USAF, Coast Guard)
    if (h >= 0xADF7C8 && h <= 0xAFFFFF) return true;
    // US Coast Guard
    if (h >= 0xA00001 && h <= 0xA00FFF) return true;
    // UK military (RAF, Royal Navy, Army Air Corps)
    if (h >= 0x43C000 && h <= 0x43CFFF) return true;
    // France military
    if (h >= 0x3B0000 && h <= 0x3BFFFF) return true;
    // Germany military
    if (h >= 0x3F4000 && h <= 0x3F7FFF) return true;
    // Canada military
    if (h >= 0xC0CDF9 && h <= 0xC0FFFF) return true;
    // Australia military
    if (h >= 0x7C8000 && h <= 0x7CBFFF) return true;
    // NATO/international
    if (h >= 0x0A4000 && h <= 0x0A4FFF) return true;
    return false;
}

static bool check_emergency(uint16_t squawk) {
    return squawk == 7500 || squawk == 7600 || squawk == 7700;
}

// Find existing aircraft by ICAO hex, returns index or -1
static int find_aircraft(const char* hex) {
    for (int i = 0; i < _aircraft_list->count; i++) {
        if (strcmp(_aircraft_list->aircraft[i].icao_hex, hex) == 0) return i;
    }
    return -1;
}

// Pre-parsed aircraft entry — extracted from JSON without holding the lock
struct ParsedEntry {
    char   hex[7];
    char   callsign[9];
    char   registration[9];
    char   type_code[5];
    char   category[3];
    char   desc[40];
    char   owner_op[32];
    float  lat, lon;
    int32_t altitude;
    int16_t speed, heading, vert_rate;
    uint16_t squawk;
    bool   on_ground;
    float  mach;
    int16_t ias, tas;
    int32_t nav_altitude;
    float  roll;
    float  nav_qnh;
};

// Apply a pre-parsed entry to an Aircraft in the main list
static void apply_parsed(Aircraft& a, const ParsedEntry& p, bool is_new) {
    strlcpy(a.icao_hex, p.hex, sizeof(a.icao_hex));
    strlcpy(a.callsign, p.callsign, sizeof(a.callsign));
    strlcpy(a.registration, p.registration, sizeof(a.registration));
    strlcpy(a.type_code, p.type_code, sizeof(a.type_code));
    strlcpy(a.category, p.category, sizeof(a.category));
    strlcpy(a.desc, p.desc, sizeof(a.desc));
    strlcpy(a.owner_op, p.owner_op, sizeof(a.owner_op));
    // Don't overwrite enriched route data (origin/dest set by route_enrich_task)
    a.lat         = p.lat;
    a.lon         = p.lon;
    a.altitude    = p.altitude;
    a.speed       = p.speed;
    a.heading     = p.heading;
    a.vert_rate   = p.vert_rate;
    a.squawk      = p.squawk;
    a.on_ground   = p.on_ground;
    a.mach        = p.mach;
    a.ias         = p.ias;
    a.tas         = p.tas;
    a.nav_altitude = p.nav_altitude;
    a.roll        = p.roll;
    a.nav_qnh     = p.nav_qnh;
    a.is_military  = check_military(a.icao_hex);
    a.is_emergency = check_emergency(a.squawk);
    a.is_watched   = false;
    a.last_seen    = millis();
    a.stale_since  = 0;

    if (is_new) a.trail_count = 0;

    if (a.lat != 0.0f || a.lon != 0.0f) {
        if (a.trail_count < TRAIL_LENGTH) {
            a.trail[a.trail_count] = {a.lat, a.lon, a.altitude, a.last_seen};
            a.trail_count++;
        } else {
            memmove(&a.trail[0], &a.trail[1], (TRAIL_LENGTH - 1) * sizeof(TrailPoint));
            a.trail[TRAIL_LENGTH - 1] = {a.lat, a.lon, a.altitude, a.last_seen};
        }
    }
}

static float json_number(const cJSON* obj, const char* key, float fallback) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item)) {
        return (float)item->valuedouble;
    }
    return fallback;
}

static int32_t json_int(const cJSON* obj, const char* key, int32_t fallback) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item)) {
        return (int32_t)item->valueint;
    }
    return fallback;
}

static void json_string(const cJSON* obj, const char* key, char* out, size_t out_len) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring != nullptr) {
        strlcpy(out, item->valuestring, out_len);
    } else {
        out[0] = '\0';
    }
}

static void parse_aircraft_json(cJSON* doc) {
    cJSON* ac = cJSON_GetObjectItemCaseSensitive(doc, "ac");

    // Phase 1: Parse JSON into flat array — no lock needed.
    // 30 kB of scratch, so it lives in PSRAM: internal RAM is needed by the
    // SDIO link to the WiFi coprocessor.
    static EXT_RAM_BSS_ATTR ParsedEntry parsed[MAX_AIRCRAFT];
    int parsed_count = 0;

    cJSON* obj = nullptr;
    cJSON_ArrayForEach(obj, ac) {
        if (parsed_count >= MAX_AIRCRAFT) break;
        float lat = json_number(obj, "lat", 0.0f);
        float lon = json_number(obj, "lon", 0.0f);
        if (lat == 0.0f && lon == 0.0f) continue;

        ParsedEntry& p = parsed[parsed_count];
        json_string(obj, "hex", p.hex, sizeof(p.hex));
        json_string(obj, "flight", p.callsign, sizeof(p.callsign));
        for (int i = (int)strlen(p.callsign) - 1; i >= 0 && p.callsign[i] == ' '; i--) {
            p.callsign[i] = '\0';
        }
        json_string(obj, "r", p.registration, sizeof(p.registration));
        json_string(obj, "t", p.type_code, sizeof(p.type_code));
        json_string(obj, "category", p.category, sizeof(p.category));
        json_string(obj, "desc", p.desc, sizeof(p.desc));
        json_string(obj, "ownOp", p.owner_op, sizeof(p.owner_op));
        p.lat = lat;
        p.lon = lon;
        p.altitude   = json_int(obj, "alt_baro", 0);
        p.speed      = (int16_t)json_number(obj, "gs", 0.0f);
        p.heading    = (int16_t)json_number(obj, "track", 0.0f);
        p.vert_rate  = (int16_t)json_number(obj, "baro_rate", 0.0f);
        char squawk[8];
        json_string(obj, "squawk", squawk, sizeof(squawk));
        p.squawk = (uint16_t)strtoul(squawk, nullptr, 10);
        cJSON* alt = cJSON_GetObjectItemCaseSensitive(obj, "alt_baro");
        p.on_ground = cJSON_IsString(alt) && strcmp(alt->valuestring, "ground") == 0;
        p.mach         = json_number(obj, "mach", 0.0f);
        p.ias          = (int16_t)json_number(obj, "ias", 0.0f);
        p.tas          = (int16_t)json_number(obj, "tas", 0.0f);
        p.nav_altitude = json_int(obj, "nav_altitude_mcp", 0);
        p.roll         = json_number(obj, "roll", 0.0f);
        p.nav_qnh      = json_number(obj, "nav_qnh", 0.0f);
        parsed_count++;
    }

    // Phase 2: Brief lock to merge parsed data into aircraft list
    if (!_aircraft_list->lock()) return;

    uint32_t now = millis();
    bool seen[MAX_AIRCRAFT] = {};

    for (int p = 0; p < parsed_count; p++) {
        int idx = find_aircraft(parsed[p].hex);
        if (idx >= 0) {
            apply_parsed(_aircraft_list->aircraft[idx], parsed[p], false);
            seen[idx] = true;
        } else if (_aircraft_list->count < MAX_AIRCRAFT) {
            int new_idx = _aircraft_list->count;
            _aircraft_list->aircraft[new_idx].clear();
            apply_parsed(_aircraft_list->aircraft[new_idx], parsed[p], true);
            _aircraft_list->count++;
            seen[new_idx] = true;
        }
    }

    // Mark unseen aircraft as stale, remove expired ghosts
    int write = 0;
    for (int i = 0; i < _aircraft_list->count; i++) {
        Aircraft& a = _aircraft_list->aircraft[i];
        if (!seen[i]) {
            if (a.stale_since == 0) a.stale_since = now;
            if (now - a.stale_since > GHOST_TIMEOUT_MS) continue;
        }
        if (write != i) _aircraft_list->aircraft[write] = _aircraft_list->aircraft[i];
        write++;
    }
    _aircraft_list->count = write;

    // Check for alerts
    for (int i = 0; i < _aircraft_list->count; i++) {
        Aircraft& a = _aircraft_list->aircraft[i];
        if (a.stale_since != 0) continue;
        if (a.is_emergency && g_config.alert_emergency) {
            char msg[48];
            snprintf(msg, sizeof(msg), "Squawk %04d - %s", a.squawk,
                     a.squawk == 7500 ? "HIJACK" : a.squawk == 7600 ? "COMMS FAIL" : "EMERGENCY");
            alerts_queue(ALERT_EMERGENCY, a.callsign[0] ? a.callsign : a.icao_hex, msg, a.icao_hex);
        } else if (a.is_military && g_config.alert_military && !already_alerted(a.icao_hex)) {
            mark_alerted(a.icao_hex);
            alerts_queue(ALERT_MILITARY, a.callsign[0] ? a.callsign : a.icao_hex, a.type_code, a.icao_hex);
        }
    }

    _aircraft_list->unlock();
    _last_update = millis();
}

static bool network_connected() {
    if (wifi_connection_is_connected()) {
        _active_net = NET_WIFI;
        return true;
    }
    _active_net = NET_NONE;
    return false;
}

static void update_ip_addr() {
    esp_netif_ip_info_t* info = wifi_get_ip_info();
    if (info && info->ip.addr != 0) {
        snprintf(_fstats.ip_addr, sizeof(_fstats.ip_addr), IPSTR, IP2STR(&info->ip));
    } else {
        strlcpy(_fstats.ip_addr, "N/A", sizeof(_fstats.ip_addr));
    }
}

static void fetch_task(void* param) {
    // Wait for the radio to associate with a configured network
    int retries = 0;
    while (!network_connected()) {
        ESP_LOGI(TAG, "WiFi attempt %d", retries + 1);
        wifi_connect_try_all();
        retries++;
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    update_ip_addr();
    ESP_LOGI(TAG, "WiFi connected, IP: %s", _fstats.ip_addr);

    // Main fetch loop
    int consecutive_fails = 0;
    while (true) {
        if (network_connected()) {
            if (http_mutex_acquire(pdMS_TO_TICKS(15000))) {
                // Rebuilt every poll so changes to home position/radius take effect immediately
                char url[128];
                snprintf(url, sizeof(url), "https://api.adsb.lol/v2/point/%.4f/%.4f/%d", g_config.home_lat,
                         g_config.home_lon, g_config.radius_nm);

                uint32_t t0  = millis();
                char*    buf = nullptr;
                size_t   len = 0;
                int      status = http_get_body(url, &buf, &len, MAX_RESPONSE_BYTES);
                _fstats.last_fetch_ms = millis() - t0;

                if (status == 200 && buf && len > 0) {
                    _fstats.bytes_received += len;
                    cJSON* doc = cJSON_ParseWithLength(buf, len);
                    heap_caps_free(buf);
                    if (doc) {
                        parse_aircraft_json(doc);
                        cJSON_Delete(doc);
                        _fstats.fetch_ok++;
                        consecutive_fails = 0;
                        ESP_LOGI(TAG, "Fetched %d aircraft in %lums", _aircraft_list->count,
                                 (unsigned long)_fstats.last_fetch_ms);
                    } else {
                        _fstats.fetch_fail++;
                        consecutive_fails++;
                        error_log_add("JSON parse failed (%uB)", (unsigned)len);
                        ESP_LOGE(TAG, "JSON error (%u bytes)", (unsigned)len);
                    }
                } else {
                    if (buf) heap_caps_free(buf);
                    _fstats.fetch_fail++;
                    consecutive_fails++;
                    if (status > 0) {
                        error_log_add("HTTP %d", status);
                        ESP_LOGE(TAG, "HTTP error: %d", status);
                    } else {
                        error_log_add("Fetch failed");
                    }
                }
                http_mutex_release();
            }
        } else {
            consecutive_fails++;
            error_log_add("Network down");
            ESP_LOGW(TAG, "Network down, waiting...");
            update_ip_addr();
        }

        // Watchdog: too many consecutive failures → re-associate
        if (consecutive_fails >= FETCH_FAIL_RESET_THRESHOLD) {
            ESP_LOGW(TAG, "Watchdog: %d consecutive fails, reconnecting", consecutive_fails);
            error_log_add("Watchdog: %d fails, reconnect", consecutive_fails);
            wifi_connection_disconnect();
            vTaskDelay(pdMS_TO_TICKS(1000));
            wifi_connect_try_all();
            update_ip_addr();
            consecutive_fails = 0;
        }

        {
            size_t const internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            size_t const dma_largest   = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
            if (internal_free < INTERNAL_HEAP_WARN_BYTES || dma_largest < DMA_BLOCK_WARN_BYTES) {
                ESP_LOGW(TAG,
                         "Low internal RAM: %lu bytes free, largest DMA block %lu (PSRAM %lu free)",
                         (unsigned long)internal_free, (unsigned long)dma_largest,
                         (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(ADSB_POLL_INTERVAL_MS));
    }
}

// Background task: fetch route (origin/dest) for aircraft with callsigns
static void route_enrich_task(void* param) {
    // Wait for network
    while (!network_connected()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    vTaskDelay(pdMS_TO_TICKS(5000));  // let main fetcher populate list first

    while (true) {
        // Find next aircraft that has a callsign but no route data
        char callsign[9] = {};
        char icao_hex[7] = {};
        bool found       = false;

        if (_aircraft_list->lock(pdMS_TO_TICKS(100))) {
            for (int i = 0; i < _aircraft_list->count; i++) {
                Aircraft& a = _aircraft_list->aircraft[i];
                if (a.callsign[0] && !a.origin[0] && a.stale_since == 0) {
                    strlcpy(callsign, a.callsign, sizeof(callsign));
                    strlcpy(icao_hex, a.icao_hex, sizeof(icao_hex));
                    found = true;
                    break;
                }
            }
            _aircraft_list->unlock();
        }

        if (!found || !network_connected()) {
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        // Fetch route from adsbdb
        char origin[5] = {};
        char dest[5]   = {};

        if (http_mutex_acquire(pdMS_TO_TICKS(12000))) {
            char url[128];
            snprintf(url, sizeof(url), "https://api.adsbdb.com/v0/callsign/%s", callsign);

            char*  buf    = nullptr;
            size_t len    = 0;
            int    status = http_get_body(url, &buf, &len, 8192);

            if (status == 200 && buf && len > 0) {
                cJSON* doc = cJSON_ParseWithLength(buf, len);
                if (doc) {
                    cJSON* response = cJSON_GetObjectItemCaseSensitive(doc, "response");
                    cJSON* route    = cJSON_GetObjectItemCaseSensitive(response, "flightroute");
                    cJSON* orig     = cJSON_GetObjectItemCaseSensitive(route, "origin");
                    cJSON* dest_obj = cJSON_GetObjectItemCaseSensitive(route, "destination");
                    json_string(orig, "iata_code", origin, sizeof(origin));
                    json_string(dest_obj, "iata_code", dest, sizeof(dest));
                    cJSON_Delete(doc);
                    _fstats.enrich_ok++;
                } else {
                    _fstats.enrich_fail++;
                    error_log_add("Route JSON fail: %s", callsign);
                }
                heap_caps_free(buf);
            } else {
                if (buf) heap_caps_free(buf);
                _fstats.enrich_fail++;
                // 404 = no route data (normal for GA/private) — don't log
                if (status > 0 && status != 404) {
                    error_log_add("Route HTTP %d: %s", status, callsign);
                }
            }
            http_mutex_release();
        }

        // Write results back (even empty — marks as "tried" so we don't re-fetch)
        if (_aircraft_list->lock(pdMS_TO_TICKS(100))) {
            int idx = find_aircraft(icao_hex);
            if (idx >= 0) {
                Aircraft& a = _aircraft_list->aircraft[idx];
                if (origin[0]) strlcpy(a.origin, origin, sizeof(a.origin));
                else strlcpy(a.origin, "-", sizeof(a.origin));  // mark as tried
                if (dest[0]) strlcpy(a.dest, dest, sizeof(a.dest));
                else strlcpy(a.dest, "-", sizeof(a.dest));
                ESP_LOGI(TAG, "Route: %s %s->%s", callsign, a.origin, a.dest);
            }
            _aircraft_list->unlock();
        }

        vTaskDelay(pdMS_TO_TICKS(1500));  // rate limit: ~1 req per 1.5s
    }
}

void fetcher_init(AircraftList* list) {
    _aircraft_list = list;
    http_mutex_init();

    // Task stacks come out of internal RAM, so keep them as small as the
    // TLS handshake comfortably fits into.
    xTaskCreatePinnedToCore(fetch_task, "adsb_fetch", 16384, nullptr, 1, &_fetch_task_handle, 1);
    xTaskCreatePinnedToCore(route_enrich_task, "route_enrich", 16384, nullptr, 0, &_route_task_handle, 1);
}

bool fetcher_wifi_connected() {
    return network_connected();
}

NetType fetcher_connection_type() {
    return _active_net;
}

uint32_t fetcher_last_update() {
    return _last_update;
}

const FetcherStats* fetcher_get_stats() {
    return &_fstats;
}
