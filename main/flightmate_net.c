#include "flightmate_net.h"

#include "flightmate_wifi.h"

#include "cJSON.h"
/* ChaN TJpgDec. The declarations come from our header copy; the symbols are
   resolved from the copy LVGL already compiles, so no extra object is added.
   LVGL builds it with JD_USE_SCALE=0, hence the downscale-by-sampling below. */
#include "tjpgd/tjpgd.h"
#include "esp_crt_bundle.h"

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include <arpa/inet.h>
#include <math.h>
#include <netdb.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *TAG = "fm_net";

static void url_encode(const char *src, char *out, size_t capacity)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0;
    for (; src && *src && used + 4 < capacity; ++src) {
        const unsigned char c = (unsigned char)*src;
        const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (safe) {
            out[used++] = (char)c;
        } else {
            out[used++] = '%';
            out[used++] = hex[c >> 4];
            out[used++] = hex[c & 0x0F];
        }
    }
    out[used] = '\0';
}
#define FM_HTTP_METAR_MAX 8192
#define FM_HTTP_RADAR_MAX (48 * 1024)
#define FM_HTTP_AIRCRAFT_MAX 8192
#define FM_PHOTO_MAX (24 * 1024)
#define FM_PHOTO_DECODED_MAX (8 * 1024)
#define FM_PHOTO_SLOT_COUNT 2
#define FM_NET_QUEUE_DEPTH 8
#define FM_NET_CACHE_SCHEMA 1

typedef enum {
    FM_NET_REQUEST_METAR = 1,
    FM_NET_REQUEST_RADAR,
    FM_NET_REQUEST_AIRCRAFT,
    FM_NET_REQUEST_ROUTE,
} fm_net_request_type_t;

typedef struct {
    fm_net_request_type_t type;
    char icao[FM_ICAO_LEN];
    char icao24[FM_ICAO24_LEN];
    char callsign[FM_CALLSIGN_LEN];
    float latitude;
    float longitude;
} fm_net_request_t;

typedef struct {
    char *buffer;
    size_t length;
    size_t capacity;
    size_t limit;
    bool overflow;
} http_body_t;

typedef struct {
    uint16_t schema;
    uint16_t reserved;
    uint32_t fetched_utc;
    char raw[FM_METAR_RAW_MAX];
} fm_metar_cache_t;

static QueueHandle_t s_request_queue;
static TaskHandle_t s_worker_task;
static SemaphoreHandle_t s_state_mutex;
static fm_net_snapshot_t s_state;
/* 照片缓冲改为按需分配：占 16KB DRAM，电台播放前可以整个还给堆。 */
static uint8_t *s_photo_slots[FM_PHOTO_SLOT_COUNT];
static uint8_t s_photo_slot;
static char s_aircraft_pending_icao24[FM_ICAO24_LEN];

/* TLS 握手要 20KB 以上连续内存。雷达页看久了堆会被照片/图片缓存切碎，
   这里在联网前做一次自愈：连续块不够就先丢弃照片缓冲。 */
void fm_net_reclaim_for_tls(void)
{
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (largest >= 32768) {
        return;
    }
    ESP_LOGW(TAG, "连续内存不足(%u)，先释放照片缓冲再联网", (unsigned)largest);
    fm_net_release_photo();
}

/* 释放照片缓冲与当前照片指针（电台播放前调用，腾出约 16KB）。 */
void fm_net_release_photo(void)
{
    if (s_state_mutex) xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    for (size_t i = 0; i < FM_PHOTO_SLOT_COUNT; ++i) {
        free(s_photo_slots[i]);
        s_photo_slots[i] = NULL;
    }
    s_state.photo_data = NULL;
    s_state.photo_size = 0;
    if (s_state_mutex) xSemaphoreGive(s_state_mutex);
    ESP_LOGI(TAG, "照片缓冲已释放（空闲堆=%u 最大块=%u）",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

static void set_metar_status(const char *status)
{
    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        snprintf(s_state.metar_status, sizeof(s_state.metar_status), "%s", status);
        xSemaphoreGive(s_state_mutex);
    }
}

static void set_radar_status(const char *status)
{
    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        snprintf(s_state.radar_status, sizeof(s_state.radar_status), "%s", status);
        xSemaphoreGive(s_state_mutex);
    }
}

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    http_body_t *body = event->user_data;
    if (!body || event->event_id != HTTP_EVENT_ON_DATA || !event->data || event->data_len <= 0) {
        return ESP_OK;
    }
    const size_t incoming = (size_t)event->data_len;
    if (body->length + incoming + 1 > body->limit) {
        body->overflow = true;
        return ESP_FAIL;
    }
    if (body->length + incoming + 1 > body->capacity) {
        size_t capacity = body->capacity ? body->capacity : 2048;
        while (capacity < body->length + incoming + 1) capacity *= 2;
        char *grown = realloc(body->buffer, capacity);
        if (!grown) return ESP_ERR_NO_MEM;
        body->buffer = grown;
        body->capacity = capacity;
    }
    memcpy(body->buffer + body->length, event->data, incoming);
    body->length += incoming;
    body->buffer[body->length] = '\0';
    return ESP_OK;
}

static esp_err_t http_get_text(const char *url, size_t limit, char **response,
                               size_t *response_length, int *status_code)
{
    if (!url || !response || !status_code) return ESP_ERR_INVALID_ARG;
    *response = NULL;
    if (response_length) *response_length = 0;
    *status_code = 0;
    http_body_t body = {
        .limit = limit,
    };
    const esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &body,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .user_agent = "FlightMate/1.0 (+https://ai-passport.folotoy.cn)",
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_ERR_NO_MEM;
    const esp_err_t err = esp_http_client_perform(client);
    *status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (body.overflow) {
        free(body.buffer);
        return ESP_ERR_INVALID_SIZE;
    }
    if (err != ESP_OK) {
        free(body.buffer);
        return err;
    }
    if (!body.buffer) body.buffer = calloc(1, 1);
    if (!body.buffer) return ESP_ERR_NO_MEM;
    *response = body.buffer;
    if (response_length) *response_length = body.length;
    return ESP_OK;
}

static const char *latest_metar_line(const char *body, char *line, size_t capacity)
{
    if (!body || !line || capacity == 0) return NULL;
    line[0] = '\0';
    const char *cursor = body;
    while (*cursor) {
        const char *end = strchr(cursor, '\n');
        size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
        while (length > 0 && (cursor[length - 1] == '\r' || cursor[length - 1] == ' ')) --length;
        if (length > 0) {
            size_t copy = length < capacity - 1 ? length : capacity - 1;
            memcpy(line, cursor, copy);
            line[copy] = '\0';
            /* The API lists the newest report first. */
            return line;
        }
        if (!end) break;
        cursor = end + 1;
    }
    return NULL;
}

static void save_metar_cache(const fm_metar_t *metar, uint32_t fetched_utc)
{
    if (!metar) return;
    nvs_handle_t handle;
    if (nvs_open("flightmate_net", NVS_READWRITE, &handle) != ESP_OK) return;
    fm_metar_cache_t cache = {
        .schema = FM_NET_CACHE_SCHEMA,
        .fetched_utc = fetched_utc,
    };
    snprintf(cache.raw, sizeof(cache.raw), "%s", metar->raw);
    if (nvs_set_blob(handle, "metar", &cache, sizeof(cache)) == ESP_OK) nvs_commit(handle);
    nvs_close(handle);
}

static void load_metar_cache(void)
{
    nvs_handle_t handle;
    if (nvs_open("flightmate_net", NVS_READONLY, &handle) != ESP_OK) return;
    fm_metar_cache_t cache = { 0 };
    size_t size = sizeof(cache);
    const esp_err_t err = nvs_get_blob(handle, "metar", &cache, &size);
    nvs_close(handle);
    if (err != ESP_OK || size < sizeof(cache) || cache.schema != FM_NET_CACHE_SCHEMA) return;

    fm_metar_t parsed;
    if (fm_parse_metar(cache.raw, &parsed)) {
        s_state.metar = parsed;
        s_state.metar_fetched_utc = cache.fetched_utc;
        snprintf(s_state.metar_status, sizeof(s_state.metar_status), "离线缓存");
    }
}

static void fetch_metar(const char *icao)
{
    if (!icao || strlen(icao) != 4) {
        set_metar_status("机场代码错误");
        return;
    }
    if (!fm_wifi_connected()) {
        set_metar_status("离线，显示缓存");
        return;
    }
    if (!fm_net_time_valid()) {
        set_metar_status("等待时间同步");
        return;
    }

    char url[180];
    snprintf(url, sizeof(url),
             "https://aviationweather.gov/api/data/metar?ids=%s&format=raw&taf=false&hours=2",
             icao);
    set_metar_status("正在刷新...");
    char *body = NULL;
    int status_code = 0;
    const esp_err_t err = http_get_text(url, FM_HTTP_METAR_MAX, &body, NULL, &status_code);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "METAR request failed: %s", esp_err_to_name(err));
        set_metar_status("网络请求失败");
        free(body);
        return;
    }

    char raw[FM_METAR_RAW_MAX];
    if (status_code != 200) {
        snprintf(raw, sizeof(raw), "HTTP %d", status_code);
        set_metar_status(raw);
        free(body);
        return;
    }
    if (!latest_metar_line(body, raw, sizeof(raw))) {
        set_metar_status("暂无气象报文");
        free(body);
        return;
    }

    fm_metar_t parsed;
    if (!fm_parse_metar(raw, &parsed)) {
        set_metar_status("报文解析失败");
        free(body);
        return;
    }
    ESP_LOGI(TAG, "metar %s: %s", parsed.station, parsed.raw);
    const uint32_t now = fm_net_utc_now();
    if (xSemaphoreTake(s_state_mutex, portMAX_DELAY) == pdTRUE) {
        s_state.metar = parsed;
        s_state.metar_fetched_utc = now;
        snprintf(s_state.metar_status, sizeof(s_state.metar_status), "实时");
        xSemaphoreGive(s_state_mutex);
    }
    save_metar_cache(&parsed, now);
    free(body);
}


static void insert_flight(fm_flight_t *flights, size_t *count, const fm_flight_t *candidate)
{
    size_t position = *count;
    for (size_t i = 0; i < *count; ++i) {
        if (candidate->distance_km < flights[i].distance_km) {
            position = i;
            break;
        }
    }
    if (position >= FM_MAX_FLIGHTS) return;
    if (*count < FM_MAX_FLIGHTS) ++(*count);
    for (size_t i = *count - 1; i > position; --i) flights[i] = flights[i - 1];
    flights[position] = *candidate;
}

/* Field helpers that read straight out of the raw JSON text. Building a cJSON
   tree for a 250 NM response needs tens of KB and made cJSON_Parse fail on this
   board, so the radar feed is scanned object by object instead. */
static bool json_field_string(const char *obj, const char *obj_end, const char *key,
                              char *out, size_t capacity)
{
    if (!obj || !out || capacity == 0) return false;
    out[0] = '\0';
    char pattern[24];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *found = strstr(obj, pattern);
    if (!found || found >= obj_end) return false;
    const char *value = found + strlen(pattern);
    const char *end = value;
    while (end < obj_end && *end && *end != '"') ++end;
    if (end >= obj_end || *end != '"') return false;
    size_t length = (size_t)(end - value);
    if (length >= capacity) length = capacity - 1;
    memcpy(out, value, length);
    out[length] = '\0';
    return true;
}

static bool json_field_number(const char *obj, const char *obj_end, const char *key, double *out)
{
    char pattern[24];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *found = strstr(obj, pattern);
    if (!found || found >= obj_end) return false;
    const char *value = found + strlen(pattern);
    if (value >= obj_end || *value == '"' || *value == 'n') return false;
    char *end = NULL;
    const double number = strtod(value, &end);
    if (end == value) return false;
    *out = number;
    return true;
}

static void parse_adsblol(const char *body, float center_lat, float center_lon,
                          fm_flight_t *flights, size_t *count)
{
    if (!body) return;
    const char *cursor = strstr(body, "\"ac\":[");
    if (!cursor) return;
    cursor += 6;

    while (*cursor) {
        const char *open = strchr(cursor, '{');
        if (!open) break;
        const char *close = strchr(open, '}');
        if (!close || close <= open) break;
        cursor = close + 1;

        double lat = 0;
        double lon = 0;
        if (!json_field_number(open, close, "lat", &lat)) continue;
        if (!json_field_number(open, close, "lon", &lon)) continue;

        char icao24[FM_ICAO24_LEN] = { 0 };
        char callsign[FM_CALLSIGN_LEN] = { 0 };
        char squawk[FM_SQUAWK_LEN] = { 0 };
        char type[12] = { 0 };
        json_field_string(open, close, "hex", icao24, sizeof(icao24));
        json_field_string(open, close, "flight", callsign, sizeof(callsign));
        json_field_string(open, close, "squawk", squawk, sizeof(squawk));
        json_field_string(open, close, "t", type, sizeof(type));

        fm_flight_t flight = { 0 };
        snprintf(flight.icao24, sizeof(flight.icao24), "%s", icao24);
        if (!callsign[0]) {
            snprintf(callsign, sizeof(callsign), "%s", icao24[0] ? icao24 : "UNKNOWN");
        }
        snprintf(flight.callsign, sizeof(flight.callsign), "%s", callsign);
        size_t length = strlen(flight.callsign);
        while (length > 0 && flight.callsign[length - 1] == ' ') {
            flight.callsign[--length] = '\0';
        }
        snprintf(flight.squawk, sizeof(flight.squawk), "%s", squawk[0] ? squawk : "----");
        snprintf(flight.aircraft_type, sizeof(flight.aircraft_type), "%s", type);
        flight.latitude = (float)lat;
        flight.longitude = (float)lon;

        char alt_text[16] = { 0 };
        if (json_field_string(open, close, "alt_baro", alt_text, sizeof(alt_text)) &&
            strcmp(alt_text, "ground") == 0) {
            flight.on_ground = true;
        }
        double value = 0;
        if (json_field_number(open, close, "alt_baro", &value)) {
            flight.altitude_m = (float)(value * 0.3048);
        } else if (json_field_number(open, close, "alt_geom", &value)) {
            flight.altitude_m = (float)(value * 0.3048);
        } else {
            flight.altitude_m = -1.0f;
        }
        flight.speed_kmh = json_field_number(open, close, "gs", &value)
                         ? (float)(value * 1.852) : -1.0f;
        flight.heading_deg = json_field_number(open, close, "track", &value)
                           ? (float)value : -1.0f;
        if (json_field_number(open, close, "baro_rate", &value) ||
            json_field_number(open, close, "geom_rate", &value)) {
            flight.vertical_rate_mps = (float)(value * 0.00508);
        }

        flight.distance_km = fm_distance_km(center_lat, center_lon,
                                            flight.latitude, flight.longitude);
        flight.bearing_deg = fm_bearing_deg(center_lat, center_lon,
                                            flight.latitude, flight.longitude);
        flight.status = fm_classify_flight(&flight);
        insert_flight(flights, count, &flight);
    }
}

static void fetch_radar(float center_lat, float center_lon)
{
    if (!fm_wifi_connected()) {
        set_radar_status("离线");
        return;
    }
    if (!fm_net_time_valid()) {
        set_radar_status("等待时间同步");
        return;
    }

    /* adsb.lol is a free community feed; OpenSky's endpoint no longer
       completes a TLS handshake from this target. */
    char url[192];
    /* 80 NM had no coverage around several Chinese airports; 250 NM is the
       API maximum and still fits the response budget. */
    snprintf(url, sizeof(url), "https://api.adsb.lol/v2/point/%.4f/%.4f/%u",
             center_lat, center_lon, 250u);
    set_radar_status("正在扫描...");
    char *body = NULL;
    int status_code = 0;
    const esp_err_t err = http_get_text(url, FM_HTTP_RADAR_MAX, &body, NULL, &status_code);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "radar request failed: %s", esp_err_to_name(err));
        set_radar_status("扫描失败");
        free(body);
        return;
    }
    if (status_code != 200) {
        char status[48];
        snprintf(status, sizeof(status), "HTTP %d", status_code);
        set_radar_status(status);
        free(body);
        return;
    }

    fm_flight_t flights[FM_MAX_FLIGHTS] = { 0 };
    size_t count = 0;
    parse_adsblol(body, center_lat, center_lon, flights, &count);
    ESP_LOGI(TAG, "radar result http=%d bytes=%u flights=%u head=%.80s",
             status_code, (unsigned)(body ? strlen(body) : 0), (unsigned)count,
             body ? body : "");
    const uint32_t now = fm_net_utc_now();
    if (xSemaphoreTake(s_state_mutex, portMAX_DELAY) == pdTRUE) {
        for (size_t i = 0; i < count; ++i) {
            flights[i].first_seen_utc = now;
            for (size_t old = 0; old < s_state.flight_count; ++old) {
                const bool same_icao = flights[i].icao24[0] &&
                    strcmp(flights[i].icao24, s_state.flights[old].icao24) == 0;
                const bool same_callsign = !same_icao &&
                    strcmp(flights[i].callsign, s_state.flights[old].callsign) == 0;
                if (same_icao || same_callsign) {
                    if (s_state.flights[old].first_seen_utc) {
                        flights[i].first_seen_utc = s_state.flights[old].first_seen_utc;
                    }
                    break;
                }
            }
            flights[i].duration_s = now > flights[i].first_seen_utc
                                  ? now - flights[i].first_seen_utc : 0;
        }
        memcpy(s_state.flights, flights, sizeof(flights));
        s_state.flight_count = count;
        s_state.radar_fetched_utc = now;
        snprintf(s_state.radar_status, sizeof(s_state.radar_status),
                 count ? "%u 架航迹" : "附近暂无航迹", (unsigned)count);
        xSemaphoreGive(s_state_mutex);
    }
    free(body);
}

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t pos;
    uint8_t *rgb565;
    int width;      /* downscaled output width */
    int step;       /* 1, 2, 4 or 8 */
} fm_jpeg_t;

static size_t fm_jpeg_input(JDEC *jd, uint8_t *buff, size_t nbyte)
{
    fm_jpeg_t *ctx = (fm_jpeg_t *)jd->device;
    const size_t remain = ctx->pos < ctx->size ? (ctx->size - ctx->pos) : 0;
    if (nbyte > remain) nbyte = remain;
    if (buff && nbyte) memcpy(buff, ctx->data + ctx->pos, nbyte);
    ctx->pos += nbyte;
    return nbyte;
}

/* TJpgDec hands us full-resolution RGB888 rectangles. Sampling every `step`
   pixel produces the small RGB565 frame we actually show, so no rescale
   support (and no full-size frame buffer) is needed. */
static int fm_jpeg_output(JDEC *jd, void *bitmap, JRECT *rect)
{
    fm_jpeg_t *ctx = (fm_jpeg_t *)jd->device;
    const uint8_t *src = (const uint8_t *)bitmap;
    const int step = ctx->step;
    const int block_w = (int)rect->right - (int)rect->left + 1;
    for (int y = (int)rect->top; y <= (int)rect->bottom; ++y) {
        const uint8_t *row = src + (size_t)(y - (int)rect->top) * (size_t)block_w * 3;
        if (step > 1 && (y % step) != 0) continue;
        uint16_t *dst = (uint16_t *)(void *)(ctx->rgb565 +
                         ((size_t)(y / step) * (size_t)ctx->width +
                          (size_t)((int)rect->left / step)) * 2);
        for (int x = (int)rect->left; x <= (int)rect->right; ++x) {
            const uint8_t r = row[0];
            const uint8_t g = row[1];
            const uint8_t b = row[2];
            row += 3;
            if (step > 1 && (x % step) != 0) continue;
            *dst++ = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        }
    }
    return 1;
}

/* Decode a JPEG into `out` as RGB565, downscaling by integer sampling until the
   frame fits max_bytes. Decoding here instead of inside LVGL keeps the image
   off LVGL's 48 KB heap, where a full RGB888 frame would not fit. */
static bool decode_jpeg_rgb565(const uint8_t *data, size_t length, uint8_t *out,
                               size_t max_bytes, uint16_t *width, uint16_t *height)
{
    if (!data || !out || length < 4 || data[0] != 0xFF || data[1] != 0xD8) return false;

    const size_t pool_size = 12 * 1024;
    void *pool = malloc(pool_size);
    if (!pool) {
        ESP_LOGW(TAG, "jpeg pool alloc failed");
        return false;
    }
    JDEC jd;
    fm_jpeg_t ctx = { .data = data, .size = length };
    JRESULT rc = jd_prepare(&jd, fm_jpeg_input, pool, pool_size, &ctx);
    if (rc != JDR_OK || jd.width == 0 || jd.height == 0) {
        ESP_LOGW(TAG, "jd_prepare failed: %d", (int)rc);
        free(pool);
        return false;
    }

    int step = 1;
    while (step < 8) {
        const size_t w = ((size_t)jd.width + step - 1) / step;
        const size_t h = ((size_t)jd.height + step - 1) / step;
        if (w * h * 2 <= max_bytes) break;
        step *= 2;
    }
    const size_t out_w = ((size_t)jd.width + step - 1) / step;
    const size_t out_h = ((size_t)jd.height + step - 1) / step;
    if (out_w == 0 || out_h == 0 || out_w * out_h * 2 > max_bytes) {
        free(pool);
        return false;
    }

    ctx.rgb565 = out;
    ctx.width = (int)out_w;
    ctx.step = step;
    rc = jd_decomp(&jd, fm_jpeg_output, 0);
    free(pool);
    if (rc != JDR_OK) {
        ESP_LOGW(TAG, "jd_decomp failed: %d", (int)rc);
        return false;
    }
    *width = (uint16_t)out_w;
    *height = (uint16_t)out_h;
    return true;
}

static void copy_json_string(cJSON *object, const char *name, char *out, size_t capacity)
{
    cJSON *item = object ? cJSON_GetObjectItemCaseSensitive(object, name) : NULL;
    if (cJSON_IsString(item) && item->valuestring && item->valuestring[0]) {
        snprintf(out, capacity, "%s", item->valuestring);
    }
}

static void fetch_aircraft(const char *icao24, const char *callsign)
{
    char aircraft_type[FM_AIRCRAFT_TYPE_LEN] = "未知";
    char airline[FM_AIRLINE_LEN] = "未知";
    char photo_url[384] = "";
    char photo_status[FM_PHOTO_STATUS_LEN] = "未找到照片";
    char url[224];

    snprintf(url, sizeof(url), "https://api.adsbdb.com/v0/aircraft/%s", icao24);
    char *body = NULL;
    int status_code = 0;
    esp_err_t err = http_get_text(url, FM_HTTP_AIRCRAFT_MAX, &body, NULL, &status_code);
    if (err == ESP_OK && status_code == 200) {
        cJSON *root = cJSON_Parse(body);
        cJSON *response = root ? cJSON_GetObjectItemCaseSensitive(root, "response") : NULL;
        cJSON *aircraft = response ? cJSON_GetObjectItemCaseSensitive(response, "aircraft") : NULL;
        if (cJSON_IsObject(aircraft)) {
            copy_json_string(aircraft, "type", aircraft_type, sizeof(aircraft_type));
            if (strcmp(aircraft_type, "未知") == 0) {
                copy_json_string(aircraft, "icao_type", aircraft_type, sizeof(aircraft_type));
            }
            copy_json_string(aircraft, "registered_owner", airline, sizeof(airline));
            copy_json_string(aircraft, "url_photo_thumbnail", photo_url, sizeof(photo_url));
            if (!photo_url[0]) copy_json_string(aircraft, "url_photo", photo_url, sizeof(photo_url));
        }
        cJSON_Delete(root);
    }
    free(body);
    body = NULL;

    bool photo_ready = false;
    size_t stored_photo_length = 0;
    uint16_t photo_width = 0;
    uint16_t photo_height = 0;
    /* adsbdb often has no photo for a given airframe; planespotters covers far
       more of them, so fall back to it (free, no key, needs a real user agent). */
    if (!photo_url[0] && icao24[0]) {
        char ps_url[128];
        snprintf(ps_url, sizeof(ps_url), "https://api.planespotters.net/pub/photos/hex/%s", icao24);
        char *ps_body = NULL;
        int ps_status = 0;
        const esp_err_t ps_err = http_get_text(ps_url, FM_HTTP_AIRCRAFT_MAX, &ps_body, NULL, &ps_status);
        ESP_LOGI(TAG, "planespotters %s: err=%s http=%d", icao24,
                 esp_err_to_name(ps_err), ps_status);
        if (ps_err == ESP_OK && ps_status == 200) {
            cJSON *ps_root = cJSON_Parse(ps_body);
            cJSON *photos = ps_root ? cJSON_GetObjectItemCaseSensitive(ps_root, "photos") : NULL;
            cJSON *first = cJSON_IsArray(photos) ? cJSON_GetArrayItem(photos, 0) : NULL;
            cJSON *thumb = first ? cJSON_GetObjectItemCaseSensitive(first, "thumbnail") : NULL;
            cJSON *src = thumb ? cJSON_GetObjectItemCaseSensitive(thumb, "src") : NULL;
            if (cJSON_IsString(src) && src->valuestring[0]) {
                snprintf(photo_url, sizeof(photo_url), "%s", src->valuestring);
            }
            cJSON_Delete(ps_root);
        }
        free(ps_body);
    }
    /* 只有 Planespotters 需要代理：它只给渐进式 JPEG，本机解码器不支持。
       airport-data 的缩略图本来就是基线 JPEG，直连即可（现场实测 weserv 代理
       在这台设备所在网络里经常连不上，会把本来能下的照片也拖挂）。 */
    if (photo_url[0] && strstr(photo_url, "plnspttrs.net")) {
        char encoded[256];
        char proxied[320];
        url_encode(photo_url, encoded, sizeof(encoded));
        snprintf(proxied, sizeof(proxied),
                 "https://images.weserv.nl/?url=%s&w=150&output=jpg", encoded);
        snprintf(photo_url, sizeof(photo_url), "%s", proxied);
    }
    ESP_LOGI(TAG, "aircraft %s lookup: http=%d type=%s photo_url=%s",
             icao24, status_code, aircraft_type, photo_url[0] ? photo_url : "(none)");
    if (photo_url[0]) {
        snprintf(photo_status, sizeof(photo_status), "正在下载照片");
        size_t photo_length = 0;
        err = http_get_text(photo_url, FM_PHOTO_MAX, &body, &photo_length, &status_code);
        if ((err != ESP_OK || status_code != 200 || photo_length == 0) &&
            !strstr(photo_url, "images.weserv.nl")) {
            /* 直连失败常见于对端重置/证书链异常：换 weserv 代理再试一次。 */
            char encoded[256];
            char proxied[320];
            url_encode(photo_url, encoded, sizeof(encoded));
            snprintf(proxied, sizeof(proxied),
                     "https://images.weserv.nl/?url=%s&w=150&output=jpg", encoded);
            ESP_LOGW(TAG, "照片直连失败(%s http=%d)，改走代理重试",
                     esp_err_to_name(err), status_code);
            err = http_get_text(proxied, FM_PHOTO_MAX, &body, &photo_length, &status_code);
        }
        ESP_LOGI(TAG, "photo fetch: err=%s http=%d len=%u",
                 esp_err_to_name(err), status_code, (unsigned)photo_length);
        if (err == ESP_OK && status_code == 200 && photo_length > 0) {
            const uint8_t next_slot = s_photo_slot ^ 1U;
            if (!s_photo_slots[next_slot]) {
                s_photo_slots[next_slot] = malloc(FM_PHOTO_DECODED_MAX);
            }
            if (s_photo_slots[next_slot] &&
                decode_jpeg_rgb565((const uint8_t *)body, photo_length, s_photo_slots[next_slot],
                                   FM_PHOTO_DECODED_MAX, &photo_width, &photo_height)) {
                s_photo_slot = next_slot;
                photo_ready = true;
                stored_photo_length = (size_t)photo_width * photo_height * 2;
                snprintf(photo_status, sizeof(photo_status), "真实飞机照片");
                ESP_LOGI(TAG, "photo decoded %ux%u (%u bytes) heap=%u",
                         photo_width, photo_height, (unsigned)stored_photo_length,
                         (unsigned)esp_get_free_heap_size());
            } else {
                snprintf(photo_status, sizeof(photo_status), "照片解码失败");
            }
        } else {
            snprintf(photo_status, sizeof(photo_status), "照片下载失败");
        }
        free(body);
    }

    if (xSemaphoreTake(s_state_mutex, portMAX_DELAY) == pdTRUE) {
        snprintf(s_state.aircraft_icao24, sizeof(s_state.aircraft_icao24), "%s", icao24);
        snprintf(s_state.aircraft_type, sizeof(s_state.aircraft_type), "%s", aircraft_type);
        fm_airline_display_name(callsign, airline, s_state.airline, sizeof(s_state.airline));
        snprintf(s_state.photo_status, sizeof(s_state.photo_status), "%s", photo_status);
        s_state.aircraft_loading = false;
        s_state.photo_generation++;
        if (photo_ready) {
            s_state.photo_data = s_photo_slots[s_photo_slot];
            s_state.photo_size = stored_photo_length;
            s_state.photo_width = photo_width;
            s_state.photo_height = photo_height;
        } else {
            s_state.photo_data = NULL;
            s_state.photo_size = 0;
            s_state.photo_width = 0;
            s_state.photo_height = 0;
        }
        xSemaphoreGive(s_state_mutex);
    }
    ESP_LOGI(TAG, "aircraft %s type=%s owner=%s photo=%s",
             icao24, aircraft_type, airline, photo_ready ? "ready" : "unavailable");
}

static void fetch_route(const char *callsign)
{
    if (!callsign || !callsign[0]) return;
    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        snprintf(s_state.route_callsign, sizeof(s_state.route_callsign), "%s", callsign);
        snprintf(s_state.route_status, sizeof(s_state.route_status), "查询航线中");
        s_state.route_origin_icao[0] = '\0';
        s_state.route_destination_icao[0] = '\0';
        s_state.route_origin_name[0] = '\0';
        s_state.route_destination_name[0] = '\0';
        xSemaphoreGive(s_state_mutex);
    }

    char url[160];
    snprintf(url, sizeof(url), "https://api.adsbdb.com/v0/callsign/%s", callsign);
    char *body = NULL;
    int status_code = 0;
    const esp_err_t err = http_get_text(url, FM_HTTP_AIRCRAFT_MAX, &body, NULL, &status_code);

    char origin_icao[FM_ICAO_LEN] = { 0 };
    char origin_name[24] = { 0 };
    char destination_icao[FM_ICAO_LEN] = { 0 };
    char destination_name[24] = { 0 };
    if (err == ESP_OK && status_code == 200) {
        cJSON *root = cJSON_Parse(body);
        cJSON *response = root ? cJSON_GetObjectItemCaseSensitive(root, "response") : NULL;
        cJSON *route = response ? cJSON_GetObjectItemCaseSensitive(response, "flightroute") : NULL;
        cJSON *origin = route ? cJSON_GetObjectItemCaseSensitive(route, "origin") : NULL;
        cJSON *destination = route ? cJSON_GetObjectItemCaseSensitive(route, "destination") : NULL;
        if (cJSON_IsObject(origin)) {
            copy_json_string(origin, "icao_code", origin_icao, sizeof(origin_icao));
            copy_json_string(origin, "municipality", origin_name, sizeof(origin_name));
            if (!origin_name[0]) copy_json_string(origin, "name", origin_name, sizeof(origin_name));
        }
        if (cJSON_IsObject(destination)) {
            copy_json_string(destination, "icao_code", destination_icao, sizeof(destination_icao));
            copy_json_string(destination, "municipality", destination_name, sizeof(destination_name));
            if (!destination_name[0]) {
                copy_json_string(destination, "name", destination_name, sizeof(destination_name));
            }
        }
        cJSON_Delete(root);
    }
    free(body);

    if (xSemaphoreTake(s_state_mutex, portMAX_DELAY) == pdTRUE) {
        snprintf(s_state.route_origin_icao, sizeof(s_state.route_origin_icao), "%s", origin_icao);
        snprintf(s_state.route_origin_name, sizeof(s_state.route_origin_name), "%s", origin_name);
        snprintf(s_state.route_destination_icao, sizeof(s_state.route_destination_icao), "%s", destination_icao);
        snprintf(s_state.route_destination_name, sizeof(s_state.route_destination_name), "%s", destination_name);
        snprintf(s_state.route_status, sizeof(s_state.route_status), "%s",
                 origin_icao[0] && destination_icao[0] ? "航线已找到" : "未找到航线");
        xSemaphoreGive(s_state_mutex);
    }
    ESP_LOGI(TAG, "route %s: %s -> %s", callsign, origin_icao, destination_icao);
}

static void network_task(void *arg)
{
    (void)arg;
    fm_net_request_t request;
    while (xQueueReceive(s_request_queue, &request, portMAX_DELAY) == pdTRUE) {
        if (request.type == FM_NET_REQUEST_METAR) {
            fetch_metar(request.icao);
        } else if (request.type == FM_NET_REQUEST_RADAR) {
            fetch_radar(request.latitude, request.longitude);
        } else if (request.type == FM_NET_REQUEST_AIRCRAFT) {
            fetch_aircraft(request.icao24, request.callsign);
        } else if (request.type == FM_NET_REQUEST_ROUTE) {
            fetch_route(request.callsign);
        }
        if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (request.type == FM_NET_REQUEST_METAR) s_state.metar_loading = false;
            else if (request.type == FM_NET_REQUEST_RADAR) s_state.radar_loading = false;
            xSemaphoreGive(s_state_mutex);
        }
        /* Let the idle task run between requests so the task watchdog never
           reports this worker as starved. */
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelete(NULL);
}

esp_err_t fm_net_init(void)
{
    s_state_mutex = xSemaphoreCreateMutex();
    if (!s_state_mutex) return ESP_ERR_NO_MEM;
    s_request_queue = xQueueCreate(FM_NET_QUEUE_DEPTH, sizeof(fm_net_request_t));
    if (!s_request_queue) return ESP_ERR_NO_MEM;
    /* TLS handshakes plus the JPEG decode need well over 8 KB of stack. */
    if (xTaskCreate(network_task, "fm_net", 16384, NULL, 4, &s_worker_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(s_state.metar_status, sizeof(s_state.metar_status), "等待刷新");
    snprintf(s_state.radar_status, sizeof(s_state.radar_status), "等待扫描");
    load_metar_cache();
    return ESP_OK;
}

void fm_net_start_time_sync(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();
    if (!esp_sntp_enabled()) {
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_setservername(1, "time.cloudflare.com");
        esp_sntp_init();
    }
}

bool fm_net_time_valid(void)
{
    return time(NULL) >= 1700000000;
}

uint32_t fm_net_utc_now(void)
{
    const time_t now = time(NULL);
    return now > 0 ? (uint32_t)now : 0;
}

esp_err_t fm_net_request_metar(const char *icao)
{
    fm_net_reclaim_for_tls();
    ESP_LOGI(TAG, "metar request: free=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    if (!icao || strlen(icao) != 4) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (s_state.metar_loading) {
        xSemaphoreGive(s_state_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    s_state.metar_loading = true;
    snprintf(s_state.metar_status, sizeof(s_state.metar_status), "排队中");
    xSemaphoreGive(s_state_mutex);

    fm_net_request_t request = {
        .type = FM_NET_REQUEST_METAR,
    };
    snprintf(request.icao, sizeof(request.icao), "%s", icao);
    const bool queued = xQueueSend(s_request_queue, &request, 0) == pdTRUE;
    if (!queued) {
        s_state.metar_loading = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t fm_net_request_aircraft(const fm_flight_t *flight)
{
    if (!flight || !flight->icao24[0]) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (s_state.aircraft_loading &&
        strcmp(s_aircraft_pending_icao24, flight->icao24) == 0) {
        xSemaphoreGive(s_state_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_state.aircraft_loading) {
        xSemaphoreGive(s_state_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    s_state.aircraft_loading = true;
    snprintf(s_aircraft_pending_icao24, sizeof(s_aircraft_pending_icao24), "%s", flight->icao24);
    snprintf(s_state.photo_status, sizeof(s_state.photo_status), "查询机型与照片");
    xSemaphoreGive(s_state_mutex);

    fm_net_request_t request = {
        .type = FM_NET_REQUEST_AIRCRAFT,
        .icao24 = { 0 },
        .callsign = { 0 },
    };
    snprintf(request.icao24, sizeof(request.icao24), "%s", flight->icao24);
    snprintf(request.callsign, sizeof(request.callsign), "%s", flight->callsign);
    const bool queued = xQueueSend(s_request_queue, &request, 0) == pdTRUE;
    if (queued) ESP_LOGI(TAG, "queue aircraft lookup %s (%s)", flight->icao24, flight->callsign);
    if (!queued) {
        if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            s_state.aircraft_loading = false;
            xSemaphoreGive(s_state_mutex);
        }
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t fm_net_request_route(const char *callsign)
{
    if (!s_request_queue || !callsign || !callsign[0]) return ESP_ERR_INVALID_ARG;
    fm_net_request_t request = { 0 };
    request.type = FM_NET_REQUEST_ROUTE;
    snprintf(request.callsign, sizeof(request.callsign), "%s", callsign);
    return xQueueSend(s_request_queue, &request, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t fm_net_request_radar(const fm_airport_t *center)
{
    fm_net_reclaim_for_tls();
    if (!center) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (s_state.radar_loading) {
        xSemaphoreGive(s_state_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    s_state.radar_loading = true;
    snprintf(s_state.radar_status, sizeof(s_state.radar_status), "排队中");
    xSemaphoreGive(s_state_mutex);

    const fm_net_request_t request = {
        .type = FM_NET_REQUEST_RADAR,
        .latitude = center->latitude,
        .longitude = center->longitude,
    };
    const bool queued = xQueueSend(s_request_queue, &request, 0) == pdTRUE;
    if (!queued) {
        s_state.radar_loading = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void fm_net_snapshot(fm_net_snapshot_t *snapshot)
{
    if (!snapshot) return;
    if (s_state_mutex && xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        *snapshot = s_state;
        xSemaphoreGive(s_state_mutex);
    } else {
        memset(snapshot, 0, sizeof(*snapshot));
    }
}