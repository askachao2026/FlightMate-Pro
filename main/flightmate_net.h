#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#include "flightmate_logic.h"

#define FM_AIRCRAFT_TYPE_LEN 20
#define FM_AIRLINE_LEN 40
#define FM_PHOTO_STATUS_LEN 32

typedef struct {
    fm_metar_t metar;
    uint32_t metar_fetched_utc;
    char metar_status[48];
    bool metar_loading;
    fm_flight_t flights[FM_MAX_FLIGHTS];
    size_t flight_count;
    uint32_t radar_fetched_utc;
    char radar_status[48];
    bool radar_loading;
    char aircraft_icao24[FM_ICAO24_LEN];
    char aircraft_type[FM_AIRCRAFT_TYPE_LEN];
    char airline[FM_AIRLINE_LEN];
    char photo_status[FM_PHOTO_STATUS_LEN];
    bool aircraft_loading;
    char route_callsign[FM_CALLSIGN_LEN];
    char route_origin_icao[FM_ICAO_LEN];
    char route_origin_name[24];
    char route_destination_icao[FM_ICAO_LEN];
    char route_destination_name[24];
    char route_status[24];
    uint32_t photo_generation;
    const uint8_t *photo_data;
    size_t photo_size;
    uint16_t photo_width;
    uint16_t photo_height;
} fm_net_snapshot_t;

esp_err_t fm_net_init(void);
void fm_net_start_time_sync(void);
bool fm_net_time_valid(void);
uint32_t fm_net_utc_now(void);
esp_err_t fm_net_request_metar(const char *icao);
esp_err_t fm_net_request_radar(const fm_airport_t *center);
esp_err_t fm_net_request_aircraft(const fm_flight_t *flight);
esp_err_t fm_net_request_route(const char *callsign);
void fm_net_snapshot(fm_net_snapshot_t *snapshot);
void fm_net_release_photo(void);
void fm_net_reclaim_for_tls(void);