#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FM_ICAO_LEN 5
#define FM_METAR_RAW_MAX 256
#define FM_METAR_SUMMARY_MAX 128
#define FM_VISIBILITY_MAX 20
#define FM_MAX_FLIGHTS 16
#define FM_MAX_LOGS 16
#define FM_CALLSIGN_LEN 10
#define FM_ICAO24_LEN 7
#define FM_SQUAWK_LEN 5
#define FM_COUNTRY_LEN 28

typedef struct {
    const char *icao;
    const char *name;
    float latitude;
    float longitude;
} fm_airport_t;

typedef struct {
    bool valid;
    char station[FM_ICAO_LEN];
    char observed[7];
    int wind_direction_deg;
    int wind_speed_kt;
    int wind_gust_kt;
    int temperature_c;
    int dewpoint_c;
    int qnh_hpa;
    bool has_temperature;
    bool has_dewpoint;
    bool has_qnh;
    char visibility[FM_VISIBILITY_MAX];
    char summary[FM_METAR_SUMMARY_MAX];
    char raw[FM_METAR_RAW_MAX];
} fm_metar_t;

typedef enum {
    FM_FLIGHT_STATUS_TAKEOFF = 0,
    FM_FLIGHT_STATUS_ENROUTE,
    FM_FLIGHT_STATUS_ARRIVED,
} fm_flight_status_t;

typedef struct {
    char icao24[FM_ICAO24_LEN];
    char callsign[FM_CALLSIGN_LEN];
    char squawk[FM_SQUAWK_LEN];
    char country[FM_COUNTRY_LEN];
    char aircraft_type[12];
    float latitude;
    float longitude;
    float altitude_m;
    float speed_kmh;
    float heading_deg;
    float vertical_rate_mps;
    float distance_km;
    float bearing_deg;
    uint32_t first_seen_utc;
    uint32_t duration_s;
    bool on_ground;
    fm_flight_status_t status;
} fm_flight_t;

typedef struct {
    uint32_t started_utc;
    uint32_t ended_utc;
    char departure_icao[FM_ICAO_LEN];
    char arrival_icao[FM_ICAO_LEN];
    char callsign[FM_CALLSIGN_LEN];
    uint16_t distance_km;
    uint16_t paused_seconds;
} fm_log_record_t;

extern const fm_airport_t FM_AIRPORTS[];
extern const size_t FM_AIRPORT_COUNT;

void fm_copy_text(char *destination, size_t capacity, const char *source);
bool fm_parse_metar(const char *raw, fm_metar_t *out);
float fm_distance_km(float lat1, float lon1, float lat2, float lon2);
float fm_bearing_deg(float lat1, float lon1, float lat2, float lon2);
void fm_sort_flights_by_distance(fm_flight_t *flights, size_t count);
fm_flight_status_t fm_classify_flight(const fm_flight_t *flight);
const char *fm_flight_status_text(fm_flight_status_t status);
void fm_airline_display_name(const char *callsign, const char *fallback,
                             char *out, size_t capacity);
void fm_format_local_hhmm(uint32_t utc_seconds, char *out, size_t capacity);
void fm_format_local_date(uint32_t utc_seconds, char *out, size_t capacity);
void fm_format_duration(uint32_t seconds, char *out, size_t capacity);
const fm_airport_t *fm_airport_by_index(size_t index);
int fm_airport_index_by_icao(const char *icao);