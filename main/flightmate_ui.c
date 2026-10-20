#include "flightmate_ui.h"

#include "bsp_battery.h"
#include "bsp_display.h"
#include "flightmate_logic.h"
#include "flightmate_net.h"
#include "flightmate_wifi.h"
#include "airband_audio.h"
#include "airband_logic.h"
#include "airband_stations.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lvgl.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

LV_FONT_DECLARE(flightmate_ui_16);
/* 12 px subset compiled into this unit so the CMake source list stays as is. */
#include "../assets/fonts/flightmate_ui_12.c"
#include "../assets/fonts/flightmate_ui_14.c"
/* 240x320 RGB565 boot backdrop (generated). */
#include "../assets/splash/splash_bg.c"

#define FM_COLOR_BG 0x050C15
#define FM_COLOR_BG_BOTTOM 0x0B1D2C
#define FM_COLOR_PANEL 0x0A1B29
#define FM_COLOR_PANEL_TOP 0x122C44
#define FM_COLOR_PANEL_SELECT 0x10364C
#define FM_COLOR_PANEL_SELECT_TOP 0x1A5069
#define FM_COLOR_TEXT 0xEAF7FF
#define FM_COLOR_MUTED 0x86A7BC
#define FM_COLOR_CYAN 0x35E0D0
#define FM_COLOR_BLUE 0x4EA8FF
#define FM_COLOR_AMBER 0xFFB648
#define FM_COLOR_RED 0xFF667A
#define FM_COLOR_LINE 0x1D4560
#define FM_COLOR_HAIR 0x18374E
#define FM_COLOR_RADAR_BG 0x061A12
#define FM_COLOR_RADAR_GRID 0x165B36
#define FM_COLOR_RADAR 0x2CFF8A
#define FM_COLOR_RADAR_DIM 0x0E8F4F
#define FM_RADAR_SWEEP_LINES 8
#define FM_RADIO_ROWS 6

typedef enum {
    FM_PAGE_HOME = 0,
    FM_PAGE_WEATHER,
    FM_PAGE_RADAR,
    FM_PAGE_RADIO,
    FM_PAGE_LOG,
    FM_PAGE_SETTINGS,
} fm_page_t;

typedef struct {
    bsp_btn_t button;
    bsp_btn_ev_t event;
} fm_key_event_t;

static fm_store_t *s_store;
static QueueHandle_t s_key_queue;
static lv_timer_t *s_timer;
static lv_obj_t *s_screen;
static fm_page_t s_page;
static uint8_t s_home_index;
static uint8_t s_settings_index;
static uint8_t s_radio_index;
static uint8_t s_radio_offset;
static uint8_t s_radio_current;
static bool s_radio_have_current;
static bool s_radio_synced;
static const char *s_radio_hint;
static lv_obj_t *s_radio_row_panels[FM_RADIO_ROWS];
static lv_obj_t *s_radio_row_titles[FM_RADIO_ROWS];
static lv_obj_t *s_radio_row_metas[FM_RADIO_ROWS];
static lv_obj_t *s_radio_now_title;
static lv_obj_t *s_radio_now_meta;
static lv_obj_t *s_radio_now_status;
static uint8_t s_radar_index;
static uint32_t s_last_weather_request_ms;
static lv_obj_t *s_splash_screen;
static lv_obj_t *s_splash_sweep;
static lv_obj_t *s_splash_fill;
static lv_image_dsc_t s_splash_bg_dsc;
static lv_timer_t *s_splash_timer;
static lv_timer_t *s_splash_sweep_timer;
static lv_point_precise_t s_splash_sweep_pts[2];
static uint16_t s_splash_angle_deg;
static uint32_t s_splash_started_ms;
#define FM_SPLASH_MS 1700U
static uint32_t s_last_radar_request_ms;

static lv_obj_t *s_battery_label;
static lv_obj_t *s_network_label;
static lv_obj_t *s_network_dot;
static lv_obj_t *s_home_time_label;
static lv_obj_t *s_home_weather_label;
static lv_obj_t *s_home_preview_labels[5];
static char s_home_preview_cache[5][96];
static lv_obj_t *s_weather_title_label;
static lv_obj_t *s_weather_status_label;
static lv_obj_t *s_weather_summary_label;
static lv_obj_t *s_weather_raw_label;
static lv_obj_t *s_radar_status_label;
static lv_obj_t *s_radar_detail_label;
static lv_obj_t *s_radar_dots[FM_MAX_FLIGHTS];
static lv_obj_t *s_radar_callsign_labels[FM_MAX_FLIGHTS];
static lv_obj_t *s_radar_sweep_lines[FM_RADAR_SWEEP_LINES];
static lv_obj_t *s_radar_sweep_tail_dots[FM_RADAR_SWEEP_LINES - 1];
static lv_obj_t *s_radar_sweep_glow;
static lv_obj_t *s_radar_sweep_tip;
static lv_obj_t *s_radar_photo;
static lv_obj_t *s_radar_photo_placeholder;
static lv_obj_t *s_radar_type_label;
static lv_obj_t *s_radar_airline_label;
static lv_image_dsc_t s_radar_photo_dsc;
static uint32_t s_radar_photo_generation;
static char s_radar_requested_icao24[FM_ICAO24_LEN];
static lv_point_precise_t s_radar_sweep_points[FM_RADAR_SWEEP_LINES][2];
static uint16_t s_radar_sweep_angle_deg;
static lv_obj_t *s_log_status_label;
static lv_obj_t *s_log_route_label;
static lv_obj_t *s_log_timer_label;
static lv_obj_t *s_log_recent_labels[3];
static lv_obj_t *s_log_recent_title;
static lv_obj_t *s_setting_rows[4];
static lv_obj_t *s_setting_values[4];
static lv_obj_t *s_wifi_detail_label;

static bool s_recording;
static char s_recording_callsign[FM_CALLSIGN_LEN];
static uint32_t s_recording_started_utc;
static uint8_t s_recording_arrival_index;
static uint8_t s_log_flight_index;
static uint8_t s_log_flight_offset;
static uint8_t s_log_record_index;
static bool s_log_history_mode;
static bool s_recording_paused;
static uint32_t s_recording_resume_utc;
static uint32_t s_recording_active_s;

static uint32_t monotonic_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void clear_page_refs(void)
{
    s_battery_label = NULL;
    s_network_label = NULL;
    s_network_dot = NULL;
    s_home_time_label = NULL;
    s_home_weather_label = NULL;
    memset(s_home_preview_labels, 0, sizeof(s_home_preview_labels));
    memset(s_home_preview_cache, 0, sizeof(s_home_preview_cache));
    s_weather_title_label = NULL;
    s_weather_status_label = NULL;
    s_weather_summary_label = NULL;
    s_weather_raw_label = NULL;
    s_radar_status_label = NULL;
    s_radar_detail_label = NULL;
    memset(s_radar_dots, 0, sizeof(s_radar_dots));
    memset(s_radar_callsign_labels, 0, sizeof(s_radar_callsign_labels));
    memset(s_radar_sweep_lines, 0, sizeof(s_radar_sweep_lines));
    memset(s_radar_sweep_tail_dots, 0, sizeof(s_radar_sweep_tail_dots));
    s_radar_sweep_glow = NULL;
    s_radar_sweep_tip = NULL;
    s_radar_photo = NULL;
    s_radar_photo_placeholder = NULL;
    s_radar_type_label = NULL;
    s_radar_airline_label = NULL;
    s_radar_photo_generation = 0;
    s_radar_requested_icao24[0] = '\0';
    s_log_status_label = NULL;
    s_log_route_label = NULL;
    s_log_timer_label = NULL;
    memset(s_log_recent_labels, 0, sizeof(s_log_recent_labels));
    s_log_recent_title = NULL;
    memset(s_setting_rows, 0, sizeof(s_setting_rows));
    memset(s_setting_values, 0, sizeof(s_setting_values));
    s_wifi_detail_label = NULL;
    memset(s_radio_row_panels, 0, sizeof(s_radio_row_panels));
    memset(s_radio_row_titles, 0, sizeof(s_radio_row_titles));
    memset(s_radio_row_metas, 0, sizeof(s_radio_row_metas));
    s_radio_now_title = NULL;
    s_radio_now_meta = NULL;
    s_radio_now_status = NULL;
}

static lv_obj_t *make_panel(lv_obj_t *parent, int x, int y, int width, int height,
                            uint32_t color, bool selected)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, width, height);
    lv_obj_set_style_radius(panel, 11, 0);
    /* Top-lit gradient gives the flat panels some depth. */
    lv_obj_set_style_bg_color(panel, lv_color_hex(selected ? FM_COLOR_PANEL_SELECT_TOP : FM_COLOR_PANEL_TOP), 0);
    lv_obj_set_style_bg_grad_color(panel, lv_color_hex(selected ? FM_COLOR_PANEL_SELECT : FM_COLOR_PANEL), 0);
    lv_obj_set_style_bg_grad_dir(panel, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(selected ? FM_COLOR_CYAN : FM_COLOR_HAIR), 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    return panel;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, int x, int y, int width,
                            uint32_t color, lv_text_align_t align)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_font(label, &flightmate_ui_16, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(label, align, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, text);
    return label;
}

static void set_selected(lv_obj_t *panel, bool selected)
{
    if (!panel) return;
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(selected ? FM_COLOR_CYAN : FM_COLOR_HAIR), 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(selected ? FM_COLOR_PANEL_SELECT_TOP : FM_COLOR_PANEL_TOP), 0);
    lv_obj_set_style_bg_grad_color(panel, lv_color_hex(selected ? FM_COLOR_PANEL_SELECT : FM_COLOR_PANEL), 0);
}

/* Small L-shaped bracket used to frame the radar scope. */
static void make_hud_corner(lv_obj_t *parent, int x, int y, lv_border_side_t sides)
{
    lv_obj_t *corner = lv_obj_create(parent);
    lv_obj_set_pos(corner, x, y);
    lv_obj_set_size(corner, 11, 11);
    lv_obj_set_style_radius(corner, 2, 0);
    lv_obj_set_style_bg_opa(corner, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(corner, 0, 0);
    lv_obj_set_style_border_width(corner, 2, 0);
    lv_obj_set_style_border_color(corner, lv_color_hex(FM_COLOR_CYAN), 0);
    lv_obj_set_style_border_opa(corner, LV_OPA_50, 0);
    lv_obj_set_style_border_side(corner, sides, 0);
    lv_obj_clear_flag(corner, LV_OBJ_FLAG_SCROLLABLE);
}

static void make_topbar(const char *title)
{
    lv_obj_t *bar = lv_obj_create(s_screen);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, 240, 32);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(FM_COLOR_PANEL_TOP), 0);
    lv_obj_set_style_bg_grad_color(bar, lv_color_hex(FM_COLOR_BG), 0);
    lv_obj_set_style_bg_grad_dir(bar, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    make_label(bar, title, 10, 8, 108, FM_COLOR_TEXT, LV_TEXT_ALIGN_LEFT);
    s_network_dot = lv_obj_create(bar);
    lv_obj_set_pos(s_network_dot, 120, 13);
    lv_obj_set_size(s_network_dot, 6, 6);
    lv_obj_set_style_radius(s_network_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_network_dot, 0, 0);
    lv_obj_set_style_bg_opa(s_network_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_network_dot, lv_color_hex(FM_COLOR_MUTED), 0);
    lv_obj_clear_flag(s_network_dot, LV_OBJ_FLAG_SCROLLABLE);
    s_network_label = make_label(bar, "离线", 128, 8, 48, FM_COLOR_MUTED, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_style_text_font(s_network_label, &flightmate_ui_12, 0);
    s_battery_label = make_label(bar, "--%", 181, 8, 52, FM_COLOR_CYAN, LV_TEXT_ALIGN_RIGHT);
    lv_obj_set_style_text_font(s_battery_label, &flightmate_ui_12, 0);
    /* hairline accents under the bar */
    lv_obj_t *glow = lv_obj_create(bar);
    lv_obj_set_pos(glow, 0, 31);
    lv_obj_set_size(glow, 240, 1);
    lv_obj_set_style_radius(glow, 0, 0);
    lv_obj_set_style_border_width(glow, 0, 0);
    lv_obj_set_style_bg_color(glow, lv_color_hex(FM_COLOR_CYAN), 0);
    lv_obj_set_style_bg_opa(glow, LV_OPA_30, 0);
    lv_obj_clear_flag(glow, LV_OBJ_FLAG_SCROLLABLE);
}

static void make_footer(const char *hint)
{
    lv_obj_t *line = lv_obj_create(s_screen);
    lv_obj_set_pos(line, 0, 295);
    lv_obj_set_size(line, 240, 1);
    lv_obj_set_style_radius(line, 0, 0);
    lv_obj_set_style_bg_color(line, lv_color_hex(FM_COLOR_HAIR), 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_t *hint_label = make_label(s_screen, hint, 7, 301, 226, FM_COLOR_MUTED, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_text_font(hint_label, &flightmate_ui_12, 0);
}

static const char *wifi_text(void)
{
    if (fm_wifi_connected()) return "已连接";
    switch (fm_wifi_state()) {
    case FM_WIFI_CONNECTED: return "已连接";
    case FM_WIFI_CONNECTING: return "连接中";
    case FM_WIFI_PROVISIONING: return "配网中";
    case FM_WIFI_FAILED: return "连接失败";
    default: return fm_wifi_has_credentials() ? "未连接" : "未配网";
    }
}

static void refresh_chrome(void)
{
    static uint32_t last_battery_ms;
    static int battery = -1;
    const uint32_t now = monotonic_ms();
    if (!last_battery_ms || now - last_battery_ms >= 5000) {
        battery = bsp_battery_soc();
        last_battery_ms = now;
    }
    if (s_battery_label) {
        if (battery >= 0) lv_label_set_text_fmt(s_battery_label, "%d%%", battery);
        else lv_label_set_text(s_battery_label, "--%");
        uint32_t tint = FM_COLOR_CYAN;
        if (battery >= 0 && battery < 20) tint = FM_COLOR_RED;
        else if (battery >= 0 && battery < 50) tint = FM_COLOR_AMBER;
        lv_obj_set_style_text_color(s_battery_label, lv_color_hex(tint), 0);
    }
    if (s_network_label) {
        lv_label_set_text(s_network_label, wifi_text());
        const bool online = fm_wifi_connected();
        lv_obj_set_style_text_color(s_network_label,
            lv_color_hex(online ? FM_COLOR_CYAN : FM_COLOR_MUTED), 0);
        if (s_network_dot) {
            lv_obj_set_style_bg_color(s_network_dot,
                lv_color_hex(online ? FM_COLOR_CYAN : (fm_wifi_state() == FM_WIFI_FAILED ? FM_COLOR_RED : FM_COLOR_MUTED)), 0);
            lv_obj_set_style_bg_opa(s_network_dot, online ? LV_OPA_COVER : LV_OPA_60, 0);
        }
    }
}

static const struct {
    const char *icao;
    const char *name;
} FM_EXTRA_AIRPORT_NAMES[] = {
    { "VHHH", "香港" }, { "VMMC", "澳门" }, { "ZUCK", "重庆" }, { "ZHHH", "武汉" },
    { "ZSNJ", "南京" }, { "ZJHK", "海口" }, { "ZLXY", "西安" }, { "ZYTX", "沈阳" },
    { "ZUGY", "贵阳" }, { "ZLLL", "兰州" }, { "ZWWW", "乌鲁木齐" }, { "ZJSY", "三亚" },
    { "ZSHC", "杭州萧山" }, { "ZSAM", "厦门高崎" }, { "ZGSZ", "深圳宝安" },
};

static void airport_display_name(const char *icao, const char *api_name,
                                 char *out, size_t capacity)
{
    if (!icao || !icao[0]) {
        snprintf(out, capacity, "%s", api_name && api_name[0] ? api_name : "----");
        return;
    }
    const int index = fm_airport_index_by_icao(icao);
    if (index >= 0) {
        snprintf(out, capacity, "%s", fm_airport_by_index((size_t)index)->name);
        return;
    }
    for (size_t i = 0; i < sizeof(FM_EXTRA_AIRPORT_NAMES) / sizeof(FM_EXTRA_AIRPORT_NAMES[0]); ++i) {
        if (strcmp(FM_EXTRA_AIRPORT_NAMES[i].icao, icao) == 0) {
            snprintf(out, capacity, "%s", FM_EXTRA_AIRPORT_NAMES[i].name);
            return;
        }
    }
    snprintf(out, capacity, "%s", api_name && api_name[0] ? api_name : icao);
}

static const char *airport_name_or_icao(const char *icao)
{
    if (!icao) return "----";
    const int index = fm_airport_index_by_icao(icao);
    if (index >= 0) return fm_airport_by_index((size_t)index)->name;
    for (size_t i = 0; i < sizeof(FM_EXTRA_AIRPORT_NAMES) / sizeof(FM_EXTRA_AIRPORT_NAMES[0]); ++i) {
        if (strcmp(FM_EXTRA_AIRPORT_NAMES[i].icao, icao) == 0) return FM_EXTRA_AIRPORT_NAMES[i].name;
    }
    return icao;
}

static void home_preview_text(size_t index, const fm_net_snapshot_t *snapshot,
                              const fm_airport_t *airport, char *out, size_t capacity)
{
    switch (index) {
    case 0:
        if (snapshot->metar.valid) {
            snprintf(out, capacity, "%s  %s", snapshot->metar.station, snapshot->metar.summary);
        } else {
            snprintf(out, capacity, "%s  等待刷新",
                     airport ? airport->name : "机场气象");
        }
        break;
    case 1:
        if (snapshot->flight_count > 0) {
            snprintf(out, capacity, "%u 架航迹  最近 %s  %.1fkm",
                     (unsigned)snapshot->flight_count,
                     snapshot->flights[0].callsign,
                     snapshot->flights[0].distance_km);
        } else {
            snprintf(out, capacity, "暂无航迹  确认进入扫描");
        }
        break;
    case 2:
        if (airband_audio_is_active()) {
            const airband_station_t *live = &airband_stations()[s_radio_current];
            snprintf(out, capacity, "正在收听 %s %s", live->name_zh, live->kind_zh);
        } else {
            snprintf(out, capacity, "%u 个机场电台  确认播放",
                     (unsigned)airband_station_count());
        }
        break;
    case 3:
        if (s_store->log_count > 0) {
            char duration[24];
            const fm_log_record_t *record = &s_store->logs[0];
            fm_format_duration(record->ended_utc - record->started_utc,
                               duration, sizeof(duration));
            snprintf(out, capacity, "最近 %s→%s %s %s",
                     airport_name_or_icao(record->departure_icao),
                     airport_name_or_icao(record->arrival_icao),
                     record->callsign[0] ? record->callsign : "",
                     duration);
        } else {
            snprintf(out, capacity, "暂无记录  确认开始记录");
        }
        break;
    case 4:
    default:
        if (fm_wifi_provisioning_active()) {
            snprintf(out, capacity, "配网热点 FlightMate-Setup  192.168.4.1");
        } else if (fm_wifi_connected()) {
            snprintf(out, capacity, "在线  %s  亮度 %u%%",
                     fm_wifi_ssid(), s_store->brightness_percent);
        } else if (fm_wifi_state() == FM_WIFI_FAILED) {
            snprintf(out, capacity, "连接失败  确认进入重新配网");
        } else if (!fm_wifi_has_credentials()) {
            snprintf(out, capacity, "未配网  确认进入设置");
        } else {
            snprintf(out, capacity, "连接中  确认进入设置");
        }
        break;
    }
}

static void refresh_home_previews(void)
{
    fm_net_snapshot_t snapshot;
    fm_net_snapshot(&snapshot);
    const fm_airport_t *airport = fm_airport_by_index(s_store->airport_index);
    for (size_t i = 0; i < 5; ++i) {
        char value[96];
        home_preview_text(i, &snapshot, airport, value, sizeof(value));
        if (strcmp(value, s_home_preview_cache[i]) != 0) {
            snprintf(s_home_preview_cache[i], sizeof(s_home_preview_cache[i]), "%s", value);
            if (s_home_preview_labels[i]) lv_label_set_text(s_home_preview_labels[i], value);
        }
    }
}

static void build_home(void)
{
    make_topbar("FlightMate");
    static const char *titles[] = { "机场气象", "航迹雷达", "航空电台", "飞行日志", "设置" };
    static const char *subtitles[] = { "实时机场天气报文", "周边航班实时追踪", "机场实况语音直播", "记录起降与时长", "Wi-Fi、亮度与机场" };
    for (uint8_t i = 0; i < 5; ++i) {
        const int y = 36 + i * 41;
        lv_obj_t *panel = make_panel(s_screen, 6, y, 228, 39,
                                     FM_COLOR_PANEL, i == s_home_index);
        lv_obj_t *accent = lv_obj_create(panel);
        lv_obj_set_pos(accent, 7, 11);
        lv_obj_set_size(accent, 3, 17);
        lv_obj_set_style_radius(accent, 2, 0);
        lv_obj_set_style_border_width(accent, 0, 0);
        lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(accent, lv_color_hex(i == s_home_index ? FM_COLOR_CYAN : FM_COLOR_HAIR), 0);
        lv_obj_clear_flag(accent, LV_OBJ_FLAG_SCROLLABLE);

        make_label(panel, titles[i], 17, 3, 96,
                   i == s_home_index ? FM_COLOR_CYAN : FM_COLOR_TEXT,
                   LV_TEXT_ALIGN_LEFT);
        lv_obj_t *sub = make_label(panel, subtitles[i], 17, 21, 100, FM_COLOR_MUTED, LV_TEXT_ALIGN_LEFT);
        lv_obj_set_style_text_font(sub, &flightmate_ui_12, 0);
        lv_obj_set_height(sub, 16);

        s_home_preview_labels[i] = make_label(panel, "", 118, 21, 86,
                                              FM_COLOR_BLUE, LV_TEXT_ALIGN_RIGHT);
        lv_obj_set_style_text_font(s_home_preview_labels[i], &flightmate_ui_12, 0);
        lv_obj_set_height(s_home_preview_labels[i], 16);
        lv_label_set_long_mode(s_home_preview_labels[i], LV_LABEL_LONG_SCROLL_CIRCULAR);

        lv_obj_t *chev = make_label(panel, "›", 208, 4, 14, FM_COLOR_MUTED, LV_TEXT_ALIGN_RIGHT);
        lv_obj_set_style_text_font(chev, &flightmate_ui_14, 0);
        lv_obj_set_height(chev, 18);
    }

    char now_text[40] = "等待时间同步";
    if (fm_net_time_valid()) {
        char date[16];
        char clock[16];
        fm_format_local_date(fm_net_utc_now(), date, sizeof(date));
        fm_format_local_hhmm(fm_net_utc_now(), clock, sizeof(clock));
        snprintf(now_text, sizeof(now_text), "%s  %s", date, clock);
    }
    lv_obj_t *status = make_panel(s_screen, 6, 242, 228, 46, FM_COLOR_PANEL, false);
    lv_obj_t *tag = make_label(status, "本场", 17, 7, 40, FM_COLOR_MUTED, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_style_text_font(tag, &flightmate_ui_12, 0);
    lv_obj_set_height(tag, 16);
    s_home_time_label = make_label(status, now_text, 17, 23, 150, FM_COLOR_TEXT, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_style_text_font(s_home_time_label, &flightmate_ui_14, 0);
    lv_obj_set_height(s_home_time_label, 18);
    s_home_weather_label = make_label(status, "", 120, 6, 92, FM_COLOR_CYAN, LV_TEXT_ALIGN_RIGHT);
    lv_obj_set_style_text_font(s_home_weather_label, &flightmate_ui_14, 0);
    const fm_airport_t *airport = fm_airport_by_index(s_store->airport_index);
    lv_label_set_text(s_home_weather_label, airport ? airport->icao : "----");
    refresh_home_previews();
    make_footer("上/下选择  确认进入  长按返回");
}

static void refresh_home(void)
{
    char date[16] = "等待同步";
    char clock[16] = "--:--";
    if (fm_net_time_valid()) {
        const uint32_t now = fm_net_utc_now();
        fm_format_local_date(now, date, sizeof(date));
        fm_format_local_hhmm(now, clock, sizeof(clock));
    }
    if (s_home_time_label) lv_label_set_text_fmt(s_home_time_label, "%s  %s", date, clock);
    const fm_airport_t *airport = fm_airport_by_index(s_store->airport_index);
    if (s_home_weather_label) {
        lv_label_set_text(s_home_weather_label, airport ? airport->icao : "----");
    }
    refresh_home_previews();
}

static void build_weather(void)
{
    make_topbar("机场气象");
    lv_obj_t *panel = make_panel(s_screen, 7, 37, 226, 78, FM_COLOR_PANEL, false);
    s_weather_title_label = make_label(panel, "", 10, 11, 206, FM_COLOR_CYAN, LV_TEXT_ALIGN_LEFT);
    s_weather_status_label = make_label(panel, "", 10, 41, 206, FM_COLOR_MUTED, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_style_text_font(s_weather_status_label, &flightmate_ui_12, 0);
    lv_obj_set_height(s_weather_status_label, 16);
    s_weather_summary_label = make_label(s_screen, "", 9, 123, 222, FM_COLOR_TEXT, LV_TEXT_ALIGN_LEFT);

    lv_obj_t *raw_panel = make_panel(s_screen, 7, 173, 226, 118, FM_COLOR_PANEL, false);
    lv_obj_t *raw_tag = make_label(raw_panel, "METAR 原始报文", 10, 8, 206, FM_COLOR_MUTED, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_style_text_font(raw_tag, &flightmate_ui_12, 0);
    lv_obj_set_height(raw_tag, 16);
    s_weather_raw_label = make_label(raw_panel, "暂无缓存", 10, 30, 206, FM_COLOR_TEXT, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_style_text_font(s_weather_raw_label, &flightmate_ui_12, 0);
    make_footer("上/下切换机场  确认刷新  长按返回");
}

static void refresh_weather(void)
{
    const fm_airport_t *airport = fm_airport_by_index(s_store->airport_index);
    if (!airport) return;
    fm_net_snapshot_t snapshot;
    fm_net_snapshot(&snapshot);
    if (s_weather_title_label) {
        char title[80];
        snprintf(title, sizeof(title), "%s  %s", airport->name, airport->icao);
        lv_label_set_text(s_weather_title_label, title);
    }
    const bool same_station = snapshot.metar.valid &&
                              strcmp(snapshot.metar.station, airport->icao) == 0;
    if (s_weather_status_label) {
        char status[72];
        snprintf(status, sizeof(status), "%s  本地 %s", snapshot.metar_status,
                 fm_net_time_valid() ? "已同步" : "未同步");
        lv_label_set_text(s_weather_status_label, status);
    }
    if (s_weather_summary_label) {
        lv_label_set_text(s_weather_summary_label,
                          same_station ? snapshot.metar.summary : "该机场暂无缓存，确认刷新");
    }
    if (s_weather_raw_label) {
        lv_label_set_text(s_weather_raw_label, same_station ? snapshot.metar.raw : "暂无缓存");
    }
}

static void refresh_radar_sweep(void)
{
    if (!s_radar_sweep_lines[0]) return;
    for (size_t i = 0; i < FM_RADAR_SWEEP_LINES; ++i) {
        if (!s_radar_sweep_lines[i]) continue;
        const int angle_deg = (int)s_radar_sweep_angle_deg - (int)i * 3;
        const float radians = angle_deg * (float)M_PI / 180.0f;
        const int end_x = 113 + (int)(cosf(radians) * 58.0f);
        const int end_y = 67 + (int)(sinf(radians) * 58.0f);
        s_radar_sweep_points[i][0].x = 113;
        s_radar_sweep_points[i][0].y = 67;
        s_radar_sweep_points[i][1].x = end_x;
        s_radar_sweep_points[i][1].y = end_y;
        lv_line_set_points(s_radar_sweep_lines[i], s_radar_sweep_points[i], 2);
        if (i == 0) {
            if (s_radar_sweep_glow) {
                lv_line_set_points(s_radar_sweep_glow, s_radar_sweep_points[i], 2);
            }
            if (s_radar_sweep_tip) lv_obj_set_pos(s_radar_sweep_tip, end_x - 2, end_y - 2);
        } else if (s_radar_sweep_tail_dots[i - 1]) {
            lv_obj_set_pos(s_radar_sweep_tail_dots[i - 1], end_x - 1, end_y - 1);
        }
    }
    s_radar_sweep_angle_deg = (s_radar_sweep_angle_deg + 2) % 360;
}

static void build_radar(void)
{
    make_topbar("航迹雷达");
    /* keep the status line clear of the scope that starts at y=45 */
    s_radar_status_label = make_label(s_screen, "等待扫描", 8, 32, 224, FM_COLOR_MUTED, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_style_text_font(s_radar_status_label, &flightmate_ui_12, 0);
    lv_obj_set_height(s_radar_status_label, 13);

    lv_obj_t *scope = make_panel(s_screen, 7, 45, 226, 132, FM_COLOR_RADAR_BG, false);
    lv_obj_set_style_radius(scope, 10, 0);
    make_hud_corner(scope, 6, 6, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT);
    make_hud_corner(scope, 209, 6, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_RIGHT);
    make_hud_corner(scope, 6, 115, LV_BORDER_SIDE_BOTTOM | LV_BORDER_SIDE_LEFT);
    make_hud_corner(scope, 209, 115, LV_BORDER_SIDE_BOTTOM | LV_BORDER_SIDE_RIGHT);
    lv_obj_t *outer = lv_obj_create(scope);
    lv_obj_set_pos(outer, 51, 5);
    lv_obj_set_size(outer, 124, 124);
    lv_obj_set_style_radius(outer, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(outer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(outer, 1, 0);
    lv_obj_set_style_border_color(outer, lv_color_hex(FM_COLOR_RADAR_GRID), 0);
    lv_obj_t *middle = lv_obj_create(scope);
    lv_obj_set_pos(middle, 72, 26);
    lv_obj_set_size(middle, 82, 82);
    lv_obj_set_style_radius(middle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(middle, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(middle, 1, 0);
    lv_obj_set_style_border_color(middle, lv_color_hex(FM_COLOR_RADAR_GRID), 0);
    lv_obj_t *inner = lv_obj_create(scope);
    lv_obj_set_pos(inner, 93, 47);
    lv_obj_set_size(inner, 40, 40);
    lv_obj_set_style_radius(inner, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(inner, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(inner, 1, 0);
    lv_obj_set_style_border_color(inner, lv_color_hex(FM_COLOR_RADAR_GRID), 0);
    lv_obj_t *cross_h = lv_obj_create(scope);
    lv_obj_set_pos(cross_h, 55, 66);
    lv_obj_set_size(cross_h, 116, 1);
    lv_obj_set_style_bg_color(cross_h, lv_color_hex(FM_COLOR_RADAR_GRID), 0);
    lv_obj_set_style_border_width(cross_h, 0, 0);
    lv_obj_t *cross_v = lv_obj_create(scope);
    lv_obj_set_pos(cross_v, 112, 9);
    lv_obj_set_size(cross_v, 1, 116);
    lv_obj_set_style_bg_color(cross_v, lv_color_hex(FM_COLOR_RADAR_GRID), 0);
    lv_obj_set_style_border_width(cross_v, 0, 0);
    lv_obj_t *center = lv_obj_create(scope);
    lv_obj_set_pos(center, 110, 64);
    lv_obj_set_size(center, 6, 6);
    lv_obj_set_style_radius(center, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(center, lv_color_hex(FM_COLOR_RADAR), 0);
    lv_obj_set_style_border_width(center, 0, 0);

    s_radar_sweep_glow = lv_line_create(scope);
    lv_obj_set_pos(s_radar_sweep_glow, 0, 0);
    lv_obj_set_size(s_radar_sweep_glow, 226, 132);
    lv_obj_set_style_line_width(s_radar_sweep_glow, 7, 0);
    lv_obj_set_style_line_color(s_radar_sweep_glow, lv_color_hex(FM_COLOR_RADAR), 0);
    lv_obj_set_style_line_rounded(s_radar_sweep_glow, true, 0);
    lv_obj_set_style_opa(s_radar_sweep_glow, LV_OPA_20, 0);

    for (size_t i = 0; i < FM_RADAR_SWEEP_LINES; ++i) {
        lv_obj_t *line = lv_line_create(scope);
        lv_obj_set_pos(line, 0, 0);
        lv_obj_set_size(line, 226, 132);
        lv_obj_set_style_line_width(line, i == 0 ? 2 : 1, 0);
        lv_obj_set_style_line_color(line, lv_color_hex(i == 0 ? FM_COLOR_RADAR : FM_COLOR_RADAR_DIM), 0);
        lv_obj_set_style_line_rounded(line, true, 0);
        lv_obj_set_style_opa(line, i == 0 ? LV_OPA_COVER : (lv_opa_t)(150 - i * 16), 0);
        s_radar_sweep_lines[i] = line;
    }
    for (size_t i = 1; i < FM_RADAR_SWEEP_LINES; ++i) {
        lv_obj_t *dot = lv_obj_create(scope);
        lv_obj_set_size(dot, 3, 3);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(FM_COLOR_RADAR), 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_opa(dot, (lv_opa_t)(150 - i * 16), 0);
        s_radar_sweep_tail_dots[i - 1] = dot;
    }
    s_radar_sweep_points[0][0].x = 113;
    s_radar_sweep_points[0][0].y = 67;
    s_radar_sweep_points[0][1].x = 171;
    s_radar_sweep_points[0][1].y = 67;
    lv_line_set_points(s_radar_sweep_lines[0], s_radar_sweep_points[0], 2);
    s_radar_sweep_tip = lv_obj_create(scope);
    lv_obj_set_size(s_radar_sweep_tip, 5, 5);
    lv_obj_set_style_radius(s_radar_sweep_tip, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_radar_sweep_tip, lv_color_hex(FM_COLOR_RADAR), 0);
    lv_obj_set_style_border_width(s_radar_sweep_tip, 0, 0);
    lv_obj_set_pos(s_radar_sweep_tip, 169, 65);
    s_radar_sweep_angle_deg = 0;

    for (size_t i = 0; i < FM_MAX_FLIGHTS; ++i) {
        lv_obj_t *dot = lv_obj_create(scope);
        lv_obj_set_size(dot, 6, 6);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(FM_COLOR_RADAR), 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
        s_radar_dots[i] = dot;
    }
    for (size_t i = 0; i < FM_MAX_FLIGHTS; ++i) {
        lv_obj_t *label = make_label(scope, "", 0, 0, 52, FM_COLOR_RADAR, LV_TEXT_ALIGN_LEFT);
        /* The 16 px default made the callsign larger than its marker and clip. */
        lv_obj_set_style_text_font(label, &flightmate_ui_12, 0);
        lv_obj_set_height(label, 13);
        lv_obj_set_style_bg_color(label, lv_color_hex(FM_COLOR_RADAR_BG), 0);
        lv_obj_set_style_bg_opa(label, LV_OPA_70, 0);
        lv_obj_set_style_pad_left(label, 2, 0);
        lv_obj_set_style_pad_right(label, 2, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
        s_radar_callsign_labels[i] = label;
    }

    lv_obj_t *info_panel = make_panel(s_screen, 7, 181, 130, 111, FM_COLOR_PANEL, false);
    s_radar_detail_label = make_label(info_panel, "确认键开始扫描", 10, 8, 110, FM_COLOR_TEXT, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_style_text_font(s_radar_detail_label, &flightmate_ui_12, 0);
    lv_obj_set_style_text_line_space(s_radar_detail_label, -1, 0);
    lv_obj_set_height(s_radar_detail_label, 96);

    lv_obj_t *photo_panel = make_panel(s_screen, 142, 181, 91, 111, FM_COLOR_PANEL, false);
    s_radar_photo = lv_image_create(photo_panel);
    lv_obj_set_pos(s_radar_photo, 4, 4);
    lv_obj_set_size(s_radar_photo, 83, 60);
    lv_obj_set_style_radius(s_radar_photo, 4, 0);
    lv_obj_add_flag(s_radar_photo, LV_OBJ_FLAG_HIDDEN);
    s_radar_photo_placeholder = make_label(photo_panel, "暂无照片", 4, 4, 83, FM_COLOR_MUTED, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_height(s_radar_photo_placeholder, 60);
    s_radar_type_label = make_label(photo_panel, "机型 --", 4, 67, 83, FM_COLOR_RADAR, LV_TEXT_ALIGN_CENTER);
    s_radar_airline_label = make_label(photo_panel, "航空公司 --", 4, 88, 83, FM_COLOR_TEXT, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_text_font(s_radar_type_label, &flightmate_ui_12, 0);
    lv_obj_set_style_text_font(s_radar_airline_label, &flightmate_ui_12, 0);
    lv_obj_set_height(s_radar_type_label, 16);
    lv_obj_set_height(s_radar_airline_label, 16);
    lv_label_set_long_mode(s_radar_type_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_long_mode(s_radar_airline_label, LV_LABEL_LONG_SCROLL_CIRCULAR);

    make_footer("上/下选择航迹  确认扫描  长按返回");
}

static void refresh_radar(void)
{
    fm_net_snapshot_t snapshot;
    fm_net_snapshot(&snapshot);
    const fm_airport_t *airport = fm_airport_by_index(s_store->airport_index);
    if (s_radar_status_label && airport) {
        char status[80];
        snprintf(status, sizeof(status), "%s / %s", airport->icao, snapshot.radar_status);
        lv_label_set_text(s_radar_status_label, status);
    }
    if (snapshot.flight_count == 0) {
        for (size_t i = 0; i < FM_MAX_FLIGHTS; ++i) {
            if (s_radar_dots[i]) lv_obj_add_flag(s_radar_dots[i], LV_OBJ_FLAG_HIDDEN);
            if (s_radar_callsign_labels[i]) lv_obj_add_flag(s_radar_callsign_labels[i], LV_OBJ_FLAG_HIDDEN);
        }
        if (s_radar_detail_label) {
            const char *message = "附近暂无航班数据";
            if (snapshot.radar_loading) message = "正在扫描，请稍候...";
            else if (snapshot.radar_status[0] && strcmp(snapshot.radar_status, "等待扫描") != 0) {
                message = snapshot.radar_status;
            }
            lv_label_set_text(s_radar_detail_label, message);
        }
        return;
    }
    if (s_radar_index >= snapshot.flight_count) s_radar_index = 0;
    float max_distance = 20.0f;
    for (size_t i = 0; i < snapshot.flight_count; ++i) {
        if (snapshot.flights[i].distance_km > max_distance) max_distance = snapshot.flights[i].distance_km;
    }
    for (size_t i = 0; i < FM_MAX_FLIGHTS && s_radar_dots[i]; ++i) {
        if (i >= snapshot.flight_count) {
            lv_obj_add_flag(s_radar_dots[i], LV_OBJ_FLAG_HIDDEN);
            if (s_radar_callsign_labels[i]) lv_obj_add_flag(s_radar_callsign_labels[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const float radius = fminf(1.0f, snapshot.flights[i].distance_km / max_distance) * 58.0f;
        const float angle = snapshot.flights[i].bearing_deg * (float)M_PI / 180.0f;
        const int x = 113 + (int)(cosf(angle) * radius);
        const int y = 67 - (int)(sinf(angle) * radius);
        lv_obj_set_pos(s_radar_dots[i], x, y);
        lv_obj_clear_flag(s_radar_dots[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(s_radar_dots[i],
            lv_color_hex(i == s_radar_index ? FM_COLOR_AMBER : FM_COLOR_RADAR), 0);

        if (s_radar_callsign_labels[i]) {
            int label_x = x < 96 ? x - 48 : x + 7;
            if (label_x < 0) label_x = 0;
            if (label_x > 178) label_x = 178;
            int label_y = y - 7;
            if (label_y < 0) label_y = 0;
            if (label_y > 118) label_y = 118;
            lv_label_set_text(s_radar_callsign_labels[i], snapshot.flights[i].callsign);
            lv_obj_set_style_text_color(s_radar_callsign_labels[i],
                lv_color_hex(i == s_radar_index ? FM_COLOR_AMBER : FM_COLOR_RADAR), 0);
            lv_obj_set_pos(s_radar_callsign_labels[i], label_x, label_y);
            lv_obj_clear_flag(s_radar_callsign_labels[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    const fm_flight_t *flight = &snapshot.flights[s_radar_index];
    if (flight->icao24[0] && strcmp(flight->icao24, s_radar_requested_icao24) != 0 &&
        fm_net_request_aircraft(flight) == ESP_OK) {
        snprintf(s_radar_requested_icao24, sizeof(s_radar_requested_icao24), "%s", flight->icao24);
    }

    const bool details_match = flight->icao24[0] &&
                               strcmp(snapshot.aircraft_icao24, flight->icao24) == 0;
    if (s_radar_photo && s_radar_photo_placeholder && s_radar_type_label && s_radar_airline_label) {
        if (details_match && snapshot.photo_data && snapshot.photo_width && snapshot.photo_height &&
            snapshot.photo_generation != s_radar_photo_generation) {
            lv_image_cache_drop(&s_radar_photo_dsc);
            memset(&s_radar_photo_dsc, 0, sizeof(s_radar_photo_dsc));
            s_radar_photo_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
            /* The network task decodes the thumbnail to RGB565 already. */
            s_radar_photo_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
            s_radar_photo_dsc.header.w = snapshot.photo_width;
            s_radar_photo_dsc.header.h = snapshot.photo_height;
            s_radar_photo_dsc.header.stride = (uint32_t)snapshot.photo_width * 2U;
            s_radar_photo_dsc.data_size = (uint32_t)snapshot.photo_size;
            s_radar_photo_dsc.data = snapshot.photo_data;
            lv_image_set_src(s_radar_photo, &s_radar_photo_dsc);
            /* Blit 1:1 and centre it; a fractional scale sends LVGL through the
               software transform path, which needs far more stack and RAM than
               this board has. */
            lv_image_set_scale(s_radar_photo, 256);
            lv_obj_set_size(s_radar_photo, snapshot.photo_width, snapshot.photo_height);
            lv_obj_set_pos(s_radar_photo,
                           4 + (83 - (int)snapshot.photo_width) / 2,
                           4 + (60 - (int)snapshot.photo_height) / 2);
            lv_obj_clear_flag(s_radar_photo, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_radar_photo_placeholder, LV_OBJ_FLAG_HIDDEN);
            s_radar_photo_generation = snapshot.photo_generation;
        } else if (!details_match || !snapshot.photo_data) {
            lv_obj_add_flag(s_radar_photo, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_radar_photo_placeholder, LV_OBJ_FLAG_HIDDEN);
            const char *placeholder = "暂无照片";
            if (snapshot.aircraft_loading) placeholder = "加载中...";
            else if (details_match && snapshot.photo_status[0]) placeholder = snapshot.photo_status;
            lv_label_set_text(s_radar_photo_placeholder, placeholder);
            s_radar_photo_generation = 0;
        }

        const char *type = details_match && snapshot.aircraft_type[0] ? snapshot.aircraft_type
                          : (flight->aircraft_type[0] ? flight->aircraft_type : "未知");
        char airline[FM_AIRLINE_LEN];
        fm_airline_display_name(flight->callsign,
                                details_match ? snapshot.airline : NULL,
                                airline, sizeof(airline));
        char type_text[32];
        snprintf(type_text, sizeof(type_text), "机型 %s", type);
        char airline_text[64];
        snprintf(airline_text, sizeof(airline_text), "航空公司 %s", airline);
        lv_label_set_text(s_radar_type_label, type_text);
        lv_label_set_text(s_radar_airline_label, airline_text);
    }

    if (s_radar_detail_label) {
        char detail[192];
        snprintf(detail, sizeof(detail),
                 "呼号 %s\n应答机 %s\n距离 %.1fkm/%.0f°\n高度 %.0fm\n地速 %.0fkm/h\n状况 %s",
                 flight->callsign,
                 flight->squawk,
                 flight->distance_km,
                 flight->bearing_deg,
                 flight->altitude_m,
                 flight->speed_kmh,
                 fm_flight_status_text(flight->status));
        lv_label_set_text(s_radar_detail_label, detail);
    }
}

static void refresh_logbook(void)
{
    static fm_net_snapshot_t snapshot;
    fm_net_snapshot(&snapshot);
    char title[80];
    const fm_airport_t *departure = fm_airport_by_index(s_store->airport_index);
    const fm_airport_t *arrival = fm_airport_by_index(s_recording_arrival_index);

    if (s_log_status_label) {
        char status[48];
        if (s_recording) {
            snprintf(status, sizeof(status), "%s %s",
                     s_recording_paused ? "已暂停" : "记录中",
                     s_recording_callsign[0] ? s_recording_callsign : "");
        } else if (s_log_history_mode) {
            snprintf(status, sizeof(status), "已保存记录");
        } else {
            snprintf(status, sizeof(status), "选择航班");
        }
        lv_label_set_text(s_log_status_label, status);
        lv_obj_set_style_text_color(s_log_status_label,
            lv_color_hex(s_recording ? (s_recording_paused ? FM_COLOR_AMBER : FM_COLOR_RED) : FM_COLOR_CYAN), 0);
    }

    if (s_log_route_label && departure) {
        char route[96];
        const bool have_route = s_recording && s_recording_callsign[0] &&
            strcmp(snapshot.route_callsign, s_recording_callsign) == 0 &&
            snapshot.route_origin_icao[0] && snapshot.route_destination_icao[0];
        if (have_route) {
            char from[24];
            char to[24];
            airport_display_name(snapshot.route_origin_icao, snapshot.route_origin_name,
                                 from, sizeof(from));
            airport_display_name(snapshot.route_destination_icao, snapshot.route_destination_name,
                                 to, sizeof(to));
            snprintf(route, sizeof(route), "%s %s\n→ %s %s",
                     from, snapshot.route_origin_icao, to, snapshot.route_destination_icao);
        } else if (s_recording && arrival) {
            snprintf(route, sizeof(route), "%s %s\n→ %s %s",
                     departure->name, departure->icao, arrival->name, arrival->icao);
        } else if (s_log_history_mode) {
            snprintf(route, sizeof(route), "%s %s\n长按上删除 长按下返回", departure->name, departure->icao);
        } else {
            char clock[8] = "--:--";
            if (fm_net_time_valid()) fm_format_local_hhmm(fm_net_utc_now(), clock, sizeof(clock));
            snprintf(route, sizeof(route), "%s %s\n现在 %s  上/下选航班", departure->name, departure->icao, clock);
        }
        lv_label_set_text(s_log_route_label, route);
    }

    if (s_log_timer_label) {
        if (s_recording) {
            if (!fm_net_time_valid()) {
                lv_label_set_text(s_log_timer_label, "等待时间同步");
            } else {
                uint32_t elapsed = s_recording_active_s;
                if (!s_recording_paused) {
                    const uint32_t now = fm_net_utc_now();
                    if (now > s_recording_resume_utc) elapsed += now - s_recording_resume_utc;
                }
                char duration[32];
                fm_format_duration(elapsed, duration, sizeof(duration));
                char text[48];
                snprintf(text, sizeof(text), "%s%s", duration, s_recording_paused ? "  已暂停" : "");
                lv_label_set_text(s_log_timer_label, text);
            }
        } else if (s_log_history_mode) {
            snprintf(title, sizeof(title), "共 %u 条", (unsigned)s_store->log_count);
            lv_label_set_text(s_log_timer_label, title);
        } else if (snapshot.flight_count > 0) {
            const size_t index = s_log_flight_index < snapshot.flight_count ? s_log_flight_index : 0;
            const fm_flight_t *flight = &snapshot.flights[index];
            char text[64];
            snprintf(text, sizeof(text), "%s %s %s",
                     flight->callsign[0] ? flight->callsign : flight->icao24,
                     flight->aircraft_type[0] ? flight->aircraft_type : "",
                     fm_flight_status_text(flight->status));
            lv_label_set_text(s_log_timer_label, text);
        } else {
            lv_label_set_text(s_log_timer_label, "暂无航班可选");
        }
    }

    if (snapshot.flight_count > 0) {
        if (s_log_flight_index >= snapshot.flight_count) s_log_flight_index = 0;
        if (s_log_flight_index < s_log_flight_offset) s_log_flight_offset = s_log_flight_index;
        if (s_log_flight_index >= s_log_flight_offset + 3) {
            s_log_flight_offset = (uint8_t)(s_log_flight_index - 2);
        }
    } else {
        s_log_flight_index = 0;
        s_log_flight_offset = 0;
    }
    if (s_store->log_count > 0) {
        if (s_log_record_index >= s_store->log_count) s_log_record_index = (uint8_t)(s_store->log_count - 1);
    } else {
        s_log_record_index = 0;
    }

    if (s_log_recent_title) {
        if (s_recording) {
            snprintf(title, sizeof(title), "最近记录");
        } else if (s_log_history_mode) {
            snprintf(title, sizeof(title), "已保存记录 %u/%u  长按上删除",
                     (unsigned)(s_log_record_index + 1), (unsigned)s_store->log_count);
        } else if (snapshot.flight_count > 0) {
            snprintf(title, sizeof(title), "今日航班 %u/%u  上/下选 确认记",
                     (unsigned)(s_log_flight_index + 1), (unsigned)snapshot.flight_count);
        } else {
            snprintf(title, sizeof(title), "今日航班");
        }
        lv_label_set_text(s_log_recent_title, title);
    }

    for (uint8_t i = 0; i < 3; ++i) {
        if (!s_log_recent_labels[i]) continue;
        char line[128];
        line[0] = '\0';

        if (!s_recording && s_log_history_mode) {
            if (i >= s_store->log_count) {
                lv_label_set_text(s_log_recent_labels[i], "");
                continue;
            }
            const fm_log_record_t *record = &s_store->logs[i];
            const uint32_t wall = record->ended_utc - record->started_utc;
            const uint32_t active = wall > record->paused_seconds ? wall - record->paused_seconds : wall;
            char duration[24];
            char takeoff_time[8];
            char arrival_time[8];
            char date[16];
            fm_format_duration(active, duration, sizeof(duration));
            fm_format_local_hhmm(record->started_utc, takeoff_time, sizeof(takeoff_time));
            fm_format_local_hhmm(record->ended_utc, arrival_time, sizeof(arrival_time));
            fm_format_local_date(record->started_utc, date, sizeof(date));
            snprintf(line, sizeof(line), "%s%s %s %s→%s\n  %s→%s %s",
                     i == s_log_record_index ? "> " : "  ",
                     date + 5,
                     record->callsign[0] ? record->callsign : "----",
                     takeoff_time,
                     arrival_time,
                     airport_name_or_icao(record->departure_icao),
                     airport_name_or_icao(record->arrival_icao),
                     duration);
            lv_label_set_text(s_log_recent_labels[i], line);
            lv_obj_set_style_text_color(s_log_recent_labels[i],
                lv_color_hex(i == s_log_record_index ? FM_COLOR_CYAN : FM_COLOR_TEXT), 0);
            continue;
        }

        if (!s_recording) {
            if (snapshot.flight_count == 0) {
                lv_label_set_text(s_log_recent_labels[i],
                                  i == 0 ? "暂无航班\n请先在雷达页扫描" : "");
                lv_obj_set_style_text_color(s_log_recent_labels[i], lv_color_hex(FM_COLOR_MUTED), 0);
                continue;
            }
            const size_t index = (size_t)s_log_flight_offset + i;
            if (index >= snapshot.flight_count) {
                lv_label_set_text(s_log_recent_labels[i], "");
                continue;
            }
            const fm_flight_t *flight = &snapshot.flights[index];
            char airline[FM_AIRLINE_LEN];
            fm_airline_display_name(flight->callsign, NULL, airline, sizeof(airline));
            snprintf(line, sizeof(line), "%s%s %s\n  %s %.0fm",
                     index == s_log_flight_index ? "> " : "  ",
                     flight->callsign[0] ? flight->callsign : flight->icao24,
                     airline,
                     fm_flight_status_text(flight->status),
                     flight->altitude_m);
            lv_label_set_text(s_log_recent_labels[i], line);
            lv_obj_set_style_text_color(s_log_recent_labels[i],
                lv_color_hex(index == s_log_flight_index ? FM_COLOR_CYAN : FM_COLOR_TEXT), 0);
            continue;
        }

        if (i >= s_store->log_count) {
            lv_label_set_text(s_log_recent_labels[i], "--");
            continue;
        }
        const fm_log_record_t *record = &s_store->logs[i];
        const uint32_t wall = record->ended_utc - record->started_utc;
        const uint32_t active = wall > record->paused_seconds ? wall - record->paused_seconds : wall;
        char date[16];
        char duration[24];
        char takeoff_time[8];
        char arrival_time[8];
        fm_format_local_date(record->started_utc, date, sizeof(date));
        fm_format_duration(active, duration, sizeof(duration));
        fm_format_local_hhmm(record->started_utc, takeoff_time, sizeof(takeoff_time));
        fm_format_local_hhmm(record->ended_utc, arrival_time, sizeof(arrival_time));
        snprintf(line, sizeof(line), "%.5s %s %s→%s\n%s→%s %s",
                 date + 5,
                 record->callsign[0] ? record->callsign : "----",
                 takeoff_time,
                 arrival_time,
                 airport_name_or_icao(record->departure_icao),
                 airport_name_or_icao(record->arrival_icao),
                 duration);
        lv_label_set_text(s_log_recent_labels[i], line);
        lv_obj_set_style_text_color(s_log_recent_labels[i], lv_color_hex(FM_COLOR_TEXT), 0);
    }
}

static void build_logbook(void)
{
    make_topbar("飞行日志");
    lv_obj_t *panel = make_panel(s_screen, 7, 38, 226, 128, FM_COLOR_PANEL, false);
    s_log_status_label = make_label(panel, "选择航班", 10, 10, 206, FM_COLOR_CYAN, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_height(s_log_status_label, 24);
    s_log_route_label = make_label(panel, "", 10, 42, 206, FM_COLOR_TEXT, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_text_font(s_log_route_label, &flightmate_ui_14, 0);
    lv_obj_set_style_text_line_space(s_log_route_label, 0, 0);
    lv_obj_set_height(s_log_route_label, 44);
    s_log_timer_label = make_label(panel, "", 10, 92, 206, FM_COLOR_AMBER, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_text_font(s_log_timer_label, &flightmate_ui_14, 0);
    lv_obj_set_height(s_log_timer_label, 24);

    lv_obj_t *recent = make_panel(s_screen, 7, 176, 226, 115, FM_COLOR_PANEL, false);
    s_log_recent_title = make_label(recent, "今日航班", 8, 3, 210, FM_COLOR_MUTED, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_style_text_font(s_log_recent_title, &flightmate_ui_14, 0);
    lv_obj_set_height(s_log_recent_title, 17);
    lv_label_set_long_mode(s_log_recent_title, LV_LABEL_LONG_DOT);
    for (uint8_t i = 0; i < 3; ++i) {
        s_log_recent_labels[i] = make_label(recent, "--", 8, 21 + i * 31, 210, FM_COLOR_TEXT,
                                            LV_TEXT_ALIGN_LEFT);
        lv_obj_set_style_text_font(s_log_recent_labels[i], &flightmate_ui_14, 0);
        lv_obj_set_style_text_line_space(s_log_recent_labels[i], -3, 0);
        lv_obj_set_height(s_log_recent_labels[i], 31);
        lv_label_set_long_mode(s_log_recent_labels[i], LV_LABEL_LONG_WRAP);
    }
    make_footer(s_recording ? "确认结束  长按上暂停  长按下取消"
                            : (s_log_history_mode ? "上/下选记录  长按上删除  长按下返回"
                                                  : "上/下选航班  确认开始  长按下历史"));
}

/* ---------------- 航空电台（网络语音流） ---------------- */

static const char *radio_status_text(const airband_audio_status_t *status)
{
    switch (status->state) {
    case AIRBAND_AUDIO_CONNECTING: return "连接中…";
    case AIRBAND_AUDIO_PLAYING:    return "播放中";
    case AIRBAND_AUDIO_RETRYING:   return "语音流中断，重连中…";
    case AIRBAND_AUDIO_ERROR:      return status->detail[0] ? status->detail : "播放失败";
    default:                       return s_radio_hint ? s_radio_hint : "未播放";
    }
}

static uint32_t radio_status_color(const airband_audio_status_t *status)
{
    switch (status->state) {
    case AIRBAND_AUDIO_PLAYING:  return FM_COLOR_CYAN;
    case AIRBAND_AUDIO_CONNECTING:
    case AIRBAND_AUDIO_RETRYING: return FM_COLOR_AMBER;
    case AIRBAND_AUDIO_ERROR:    return FM_COLOR_RED;
    default:                     return FM_COLOR_MUTED;
    }
}

static void radio_frequency_text(const char *freq_text, const airband_audio_status_t *status,
                                 char *out, size_t capacity)
{
    char freq[16];
    if (freq_text && !airband_freq_is_unknown(freq_text)) {
        snprintf(out, capacity, "%sMHz", freq_text);
    } else if (airband_extract_frequency(status->stream_name, freq, sizeof(freq))) {
        snprintf(out, capacity, "%sMHz", freq);
    } else {
        snprintf(out, capacity, "--.---MHz");
    }
}

static void refresh_radio(void)
{
    const airband_station_t *stations = airband_stations();
    const size_t count = airband_station_count();
    if (count == 0) return;
    if (s_radio_index >= count) s_radio_index = 0;
    if (s_radio_index < s_radio_offset) s_radio_offset = s_radio_index;
    if (s_radio_index >= (size_t)s_radio_offset + FM_RADIO_ROWS) {
        s_radio_offset = (uint8_t)(s_radio_index - FM_RADIO_ROWS + 1);
    }

    airband_audio_status_t status;
    airband_audio_get_status(&status);
    const bool active = airband_audio_is_active();

    if (s_radio_now_title) {
        const airband_station_t *selected = &stations[s_radio_index];
        char freq[20];
        radio_frequency_text(selected->freq_text, &status, freq, sizeof(freq));
        lv_label_set_text(s_radio_now_title, selected->name_zh);
        lv_label_set_text_fmt(s_radio_now_meta, "%s · %s · %s",
                              selected->icao, selected->kind_zh, freq);
        const bool synced = airband_audio_is_active() && s_radio_have_current &&
                            s_radio_current == s_radio_index && s_radio_synced;
        lv_label_set_text_fmt(s_radio_now_status, "%s · 音量 %u%%%s",
                              radio_status_text(&status),
                              (unsigned)airband_audio_volume(),
                              synced ? " · 本场已同步" : "");
        lv_obj_set_style_text_color(s_radio_now_status,
                                    lv_color_hex(radio_status_color(&status)), 0);
    }

    for (uint8_t i = 0; i < FM_RADIO_ROWS; ++i) {
        if (!s_radio_row_titles[i]) continue;
        const size_t index = (size_t)s_radio_offset + i;
        if (index >= count) {
            lv_label_set_text(s_radio_row_titles[i], "");
            lv_label_set_text(s_radio_row_metas[i], "");
            set_selected(s_radio_row_panels[i], false);
            continue;
        }
        const airband_station_t *station = &stations[index];
        const bool is_live = active && s_radio_have_current && s_radio_current == index;
        lv_label_set_text_fmt(s_radio_row_titles[i], "%s%s",
                              is_live ? "▶ " : "", station->name_zh);
        lv_label_set_text_fmt(s_radio_row_metas[i], "%s · %s", station->icao, station->kind_zh);
        lv_obj_set_style_text_color(s_radio_row_titles[i],
            lv_color_hex(is_live ? FM_COLOR_CYAN
                                 : (index == s_radio_index ? FM_COLOR_TEXT : FM_COLOR_MUTED)), 0);
        lv_obj_set_style_text_color(s_radio_row_metas[i],
            lv_color_hex(is_live ? FM_COLOR_CYAN : FM_COLOR_MUTED), 0);
        set_selected(s_radio_row_panels[i], index == s_radio_index);
    }
}

static void build_radio(void)
{
    make_topbar("航空电台");

    lv_obj_t *now = make_panel(s_screen, 7, 36, 226, 62, FM_COLOR_PANEL, false);
    s_radio_now_title = make_label(now, "", 10, 6, 206, FM_COLOR_CYAN, LV_TEXT_ALIGN_LEFT);
    s_radio_now_meta = make_label(now, "", 10, 27, 206, FM_COLOR_MUTED, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_style_text_font(s_radio_now_meta, &flightmate_ui_12, 0);
    lv_obj_set_height(s_radio_now_meta, 16);
    s_radio_now_status = make_label(now, "", 10, 43, 206, FM_COLOR_MUTED, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_style_text_font(s_radio_now_status, &flightmate_ui_12, 0);
    lv_obj_set_height(s_radio_now_status, 16);

    lv_obj_t *list = make_panel(s_screen, 7, 104, 226, 184, FM_COLOR_PANEL, false);
    for (uint8_t i = 0; i < FM_RADIO_ROWS; ++i) {
        lv_obj_t *row = make_panel(list, 4, 4 + i * 30, 218, 28, FM_COLOR_PANEL, false);
        s_radio_row_panels[i] = row;
        s_radio_row_titles[i] = make_label(row, "", 8, 4, 118, FM_COLOR_TEXT, LV_TEXT_ALIGN_LEFT);
        s_radio_row_metas[i] = make_label(row, "", 126, 6, 86, FM_COLOR_MUTED, LV_TEXT_ALIGN_RIGHT);
        lv_obj_set_style_text_font(s_radio_row_metas[i], &flightmate_ui_12, 0);
        lv_obj_set_height(s_radio_row_metas[i], 16);
    }

    make_footer("上/下 选台  确定 播放  长按返回");
    refresh_radio();
}

static void handle_radio_key(bsp_btn_t button, bsp_btn_ev_t event)
{
    const size_t count = airband_station_count();
    if (count == 0) return;

    if (event == BSP_BTN_CLICK) {
        if (button == BSP_BTN_UP) {
            s_radio_index = (uint8_t)((s_radio_index + count - 1) % count);
            s_radio_hint = NULL;
        } else if (button == BSP_BTN_DOWN) {
            s_radio_index = (uint8_t)((s_radio_index + 1) % count);
            s_radio_hint = NULL;
        } else if (button == BSP_BTN_OK) {
            if (airband_audio_is_active() && s_radio_have_current && s_radio_current == s_radio_index) {
                airband_audio_stop();
                s_radio_have_current = false;
                s_radio_synced = false;
                s_radio_hint = "已停止";
            } else if (!fm_wifi_connected()) {
                s_radio_hint = "未联网：请先在设置里配网";
            } else {
                const airband_station_t *station = &airband_stations()[s_radio_index];
                /* 先把照片缓冲与 LVGL 图片缓存还回去，给 Helix 解码器腾内存 */
                fm_net_release_photo();
                lv_image_cache_drop(NULL);
                /* 电台选定的机场就是本场机场：航迹雷达与机场气象同步切换 */
                s_radio_synced = false;
                const int airport_index = fm_airport_index_by_icao(station->icao);
                if (airport_index >= 0) {
                    s_radio_synced = true;
                    if ((size_t)airport_index != s_store->airport_index) {
                        s_store->airport_index = (size_t)airport_index;
                        fm_store_save(s_store);
                    }
                }
                if (airband_audio_play(station->url) == ESP_OK) {
                    s_radio_current = s_radio_index;
                    s_radio_have_current = true;
                    s_radio_hint = NULL;
                } else {
                    s_radio_hint = "音频初始化失败";
                }
            }
        } else {
            return;
        }
    } else if (event == BSP_BTN_LONG) {
        uint8_t volume = airband_audio_volume();
        if (button == BSP_BTN_UP) {
            volume = volume >= 10 ? (uint8_t)(volume - 10) : 0;
        } else if (button == BSP_BTN_DOWN) {
            volume = volume <= 90 ? (uint8_t)(volume + 10) : 100;
        } else {
            return;
        }
        airband_audio_set_volume(volume);
        s_radio_hint = NULL;
    } else {
        return;
    }
    refresh_radio();
}

static void build_settings(void)
{
    make_topbar("设置");
    static const char *names[] = { "机场", "屏幕亮度", "Wi-Fi 设置", "清除 Wi-Fi" };
    for (uint8_t i = 0; i < 4; ++i) {
        const int y = 40 + i * 45;
        lv_obj_t *panel = make_panel(s_screen, 7, y, 226, 39, FM_COLOR_PANEL, i == s_settings_index);
        make_label(panel, names[i], 10, 11, 100, FM_COLOR_TEXT, LV_TEXT_ALIGN_LEFT);
        s_setting_rows[i] = panel;
        s_setting_values[i] = make_label(panel, "", 105, 12, 111, FM_COLOR_CYAN, LV_TEXT_ALIGN_RIGHT);
        lv_obj_set_style_text_font(s_setting_values[i], &flightmate_ui_14, 0);
        lv_obj_set_height(s_setting_values[i], 18);
    }
    s_wifi_detail_label = make_label(s_screen, "", 8, 226, 224, FM_COLOR_MUTED, LV_TEXT_ALIGN_LEFT);
    make_footer(fm_wifi_provisioning_active()
        ? "手机连 FlightMate-Setup  密码 flightmate"
        : "上/下选择  确认执行  长按返回");
}

static void refresh_settings(void)
{
    if (!s_store) return;
    const fm_airport_t *airport = fm_airport_by_index(s_store->airport_index);
    char brightness[16];
    snprintf(brightness, sizeof(brightness), "%u%%", s_store->brightness_percent);
    if (s_setting_values[0]) lv_label_set_text(s_setting_values[0], airport ? airport->icao : "----");
    if (s_setting_values[1]) lv_label_set_text(s_setting_values[1], brightness);
    if (s_setting_values[2]) lv_label_set_text(s_setting_values[2], wifi_text());
    if (s_setting_values[3]) lv_label_set_text(s_setting_values[3], fm_wifi_has_credentials() ? "可清除" : "--");
    if (s_wifi_detail_label) {
        char detail[192];
        if (fm_wifi_connected()) {
            snprintf(detail, sizeof(detail), "已连接 SSID %s\nIP %s", fm_wifi_ssid(), fm_wifi_ip());
        } else if (fm_wifi_state() == FM_WIFI_CONNECTING) {
            snprintf(detail, sizeof(detail), "正在连接 %s\n等待路由器响应，失败自动重试 3 次", fm_wifi_ssid());
        } else if (fm_wifi_state() == FM_WIFI_FAILED) {
            snprintf(detail, sizeof(detail), "连接失败：%s\n确认键重试，下键清除重配", fm_wifi_last_error());
        } else if (fm_wifi_provisioning_active()) {
            snprintf(detail, sizeof(detail),
                     "热点 FlightMate-Setup  密码 flightmate\n手机打开 192.168.4.1 配置 Wi-Fi\n确认重连，下键清除");
        } else if (!fm_wifi_has_credentials()) {
            snprintf(detail, sizeof(detail),
                     "未配网：确认键开启 FlightMate-Setup\n手机打开 192.168.4.1 配置 Wi-Fi");
        } else {
            snprintf(detail, sizeof(detail), "未连接：确认键重新连接，下键清除");
        }
        lv_label_set_text(s_wifi_detail_label, detail);
    }
    for (uint8_t i = 0; i < 4; ++i) set_selected(s_setting_rows[i], i == s_settings_index);
}

static void rebuild_page(void)
{
    clear_page_refs();
    lv_obj_clean(s_screen);
    refresh_chrome();
    switch (s_page) {
    case FM_PAGE_HOME: build_home(); break;
    case FM_PAGE_WEATHER: build_weather(); break;
    case FM_PAGE_RADAR: build_radar(); break;
    case FM_PAGE_RADIO: build_radio(); break;
    case FM_PAGE_LOG: build_logbook(); break;
    case FM_PAGE_SETTINGS: build_settings(); break;
    }
}

static void enter_page(fm_page_t page)
{
    if (s_page == FM_PAGE_SETTINGS && page != FM_PAGE_SETTINGS && fm_wifi_provisioning_active()) {
        fm_wifi_provisioning_stop();
    }
    if (s_page == FM_PAGE_RADIO && page != FM_PAGE_RADIO) {
        /* 电台解码器（约 19KB）与气象/雷达的 TLS 抢内存：离开电台页就停播并立刻归还，
           否则马上进雷达/气象会因连续内存不足而连接失败。 */
        airband_audio_stop();
        airband_audio_release_decoder();
        s_radio_have_current = false;
        s_radio_synced = false;
    }
    s_page = page;
    if (page == FM_PAGE_HOME) s_home_index = 0;
    if (page == FM_PAGE_SETTINGS) s_settings_index = 0;
    rebuild_page();
}

static void request_current_weather(bool force)
{
    const uint32_t now = monotonic_ms();
    if (!force && s_last_weather_request_ms && now - s_last_weather_request_ms < 30000) return;
    airband_audio_release_decoder();   /* 联网前把 Helix 还给堆 */
    const fm_airport_t *airport = fm_airport_by_index(s_store->airport_index);
    if (airport && fm_net_request_metar(airport->icao) == ESP_OK) {
        s_last_weather_request_ms = now;
    }
}

static void request_current_radar(bool force)
{
    const uint32_t now = monotonic_ms();
    /* Even a manual scan keeps a minimum gap, otherwise the API rate-limits us. */
    const uint32_t minimum_ms = force ? 15000 : 30000;
    if (s_last_radar_request_ms && now - s_last_radar_request_ms < minimum_ms) return;
    airband_audio_release_decoder();   /* 联网前把 Helix 还给堆 */
    const fm_airport_t *airport = fm_airport_by_index(s_store->airport_index);
    if (airport && fm_net_request_radar(airport) == ESP_OK) {
        s_last_radar_request_ms = now;
    }
}

static void handle_home_key(bsp_btn_t button, bsp_btn_ev_t event)
{
    if (event != BSP_BTN_CLICK) return;
    if (button == BSP_BTN_UP) s_home_index = (uint8_t)((s_home_index + 4) % 5);
    else if (button == BSP_BTN_DOWN) s_home_index = (uint8_t)((s_home_index + 1) % 5);
    else if (button == BSP_BTN_OK) {
        enter_page((fm_page_t)(s_home_index + 1));
        if (s_page == FM_PAGE_WEATHER && fm_wifi_connected()) request_current_weather(false);
        if (s_page == FM_PAGE_RADAR && fm_wifi_connected()) request_current_radar(false);
        return;
    } else {
        return;
    }
    rebuild_page();
}

static void handle_weather_key(bsp_btn_t button, bsp_btn_ev_t event)
{
    if (event == BSP_BTN_CLICK && button == BSP_BTN_UP) {
        s_store->airport_index = (s_store->airport_index + FM_AIRPORT_COUNT - 1) % FM_AIRPORT_COUNT;
    } else if (event == BSP_BTN_CLICK && button == BSP_BTN_DOWN) {
        s_store->airport_index = (s_store->airport_index + 1) % FM_AIRPORT_COUNT;
    } else if (event == BSP_BTN_CLICK && button == BSP_BTN_OK) {
        request_current_weather(true);
    } else {
        return;
    }
    fm_store_save(s_store);
    refresh_weather();
}

static void handle_radar_key(bsp_btn_t button, bsp_btn_ev_t event)
{
    if (event == BSP_BTN_CLICK && button == BSP_BTN_UP) {
        if (s_radar_index > 0) --s_radar_index;
    } else if (event == BSP_BTN_CLICK && button == BSP_BTN_DOWN) {
        ++s_radar_index;
    } else if (event == BSP_BTN_CLICK && button == BSP_BTN_OK) {
        s_radar_index = 0;
        request_current_radar(true);
    } else {
        return;
    }
    refresh_radar();
}

static void handle_log_key(bsp_btn_t button, bsp_btn_ev_t event)
{
    /* static so the snapshot never rides on the button task stack */
    static fm_net_snapshot_t snapshot;

    if (event == BSP_BTN_LONG) {
        if (s_recording) {
            if (button == BSP_BTN_UP) {
                /* pause / resume */
                if (!s_recording_paused) {
                    const uint32_t now = fm_net_utc_now();
                    if (now > s_recording_resume_utc) {
                        s_recording_active_s += now - s_recording_resume_utc;
                    }
                    s_recording_paused = true;
                } else {
                    s_recording_resume_utc = fm_net_utc_now();
                    s_recording_paused = false;
                }
            } else if (button == BSP_BTN_DOWN) {
                /* cancel: throw the recording away */
                s_recording = false;
                s_recording_paused = false;
                s_recording_active_s = 0;
                s_recording_callsign[0] = '\0';
                rebuild_page();
                return;
            }
        } else if (button == BSP_BTN_DOWN) {
            s_log_history_mode = !s_log_history_mode;
            s_log_record_index = 0;
            rebuild_page();
            return;
        } else if (button == BSP_BTN_UP && s_log_history_mode && s_store->log_count > 0) {
            fm_store_remove_log(s_store, s_log_record_index);
        }
        refresh_logbook();
        return;
    }
    if (event != BSP_BTN_CLICK) return;
    fm_net_snapshot(&snapshot);

    if (button == BSP_BTN_UP || button == BSP_BTN_DOWN) {
        const int step = button == BSP_BTN_UP ? -1 : 1;
        if (s_recording) {
            s_recording_arrival_index = (uint8_t)((s_recording_arrival_index + FM_AIRPORT_COUNT + step) %
                                                  FM_AIRPORT_COUNT);
        } else if (s_log_history_mode) {
            if (s_store->log_count > 0) {
                s_log_record_index = (uint8_t)((s_log_record_index + s_store->log_count + step) %
                                               s_store->log_count);
            }
        } else if (snapshot.flight_count > 0) {
            s_log_flight_index = (uint8_t)((s_log_flight_index + snapshot.flight_count + step) %
                                           snapshot.flight_count);
        }
    } else if (button == BSP_BTN_OK) {
        if (!s_recording && s_log_history_mode) {
            s_log_history_mode = false;
            rebuild_page();
            return;
        }
        if (!s_recording) {
            if (!fm_net_time_valid()) {
                if (s_log_timer_label) lv_label_set_text(s_log_timer_label, "等待时间同步");
                return;
            }
            s_recording_callsign[0] = '\0';
            if (snapshot.flight_count > 0) {
                const size_t index = s_log_flight_index < snapshot.flight_count ? s_log_flight_index : 0;
                snprintf(s_recording_callsign, sizeof(s_recording_callsign), "%s",
                         snapshot.flights[index].callsign);
            }
            s_recording = true;
            s_recording_paused = false;
            s_recording_active_s = 0;
            s_recording_started_utc = fm_net_utc_now();
            s_recording_resume_utc = s_recording_started_utc;
            if (s_recording_callsign[0]) fm_net_request_route(s_recording_callsign);
        } else {
            const uint32_t ended = fm_net_utc_now();
            if (!s_recording_paused && ended > s_recording_resume_utc) {
                s_recording_active_s += ended - s_recording_resume_utc;
            }
            const fm_airport_t *departure = fm_airport_by_index(s_store->airport_index);
            const fm_airport_t *arrival = fm_airport_by_index(s_recording_arrival_index);
            const bool have_route = s_recording_callsign[0] &&
                strcmp(snapshot.route_callsign, s_recording_callsign) == 0 &&
                snapshot.route_origin_icao[0] && snapshot.route_destination_icao[0];
            if (departure && arrival && ended > s_recording_started_utc) {
                fm_log_record_t record = {
                    .started_utc = s_recording_started_utc,
                    .ended_utc = ended,
                };
                if (have_route) {
                    snprintf(record.departure_icao, sizeof(record.departure_icao), "%s",
                             snapshot.route_origin_icao);
                    snprintf(record.arrival_icao, sizeof(record.arrival_icao), "%s",
                             snapshot.route_destination_icao);
                } else {
                    snprintf(record.departure_icao, sizeof(record.departure_icao), "%s", departure->icao);
                    snprintf(record.arrival_icao, sizeof(record.arrival_icao), "%s", arrival->icao);
                }
                snprintf(record.callsign, sizeof(record.callsign), "%s",
                         s_recording_callsign[0] ? s_recording_callsign : "----");
                const uint32_t wall = ended - s_recording_started_utc;
                record.paused_seconds = wall > s_recording_active_s
                                      ? (uint16_t)(wall - s_recording_active_s < 65535
                                                   ? wall - s_recording_active_s : 65535)
                                      : 0;
                const float distance = fm_distance_km(departure->latitude, departure->longitude,
                                                      arrival->latitude, arrival->longitude);
                record.distance_km = distance > 65535.0f ? 65535 : (uint16_t)(distance + 0.5f);
                fm_store_add_log(s_store, &record);
            }
            s_recording = false;
            s_recording_paused = false;
            s_recording_active_s = 0;
        }
    } else {
        return;
    }
    if (!s_recording) rebuild_page();
    else refresh_logbook();
}

static void handle_settings_key(bsp_btn_t button, bsp_btn_ev_t event)
{
    if (fm_wifi_provisioning_active()) {
        if (event == BSP_BTN_CLICK && button == BSP_BTN_OK) {
            fm_wifi_reconnect();
        } else if (event == BSP_BTN_CLICK && button == BSP_BTN_DOWN) {
            fm_wifi_forget();
        }
        refresh_settings();
        return;
    }
    if (event != BSP_BTN_CLICK) return;
    if (button == BSP_BTN_UP) s_settings_index = (s_settings_index + 3) % 4;
    else if (button == BSP_BTN_DOWN) s_settings_index = (s_settings_index + 1) % 4;
    else if (button == BSP_BTN_OK) {
        if (s_settings_index == 0) {
            s_store->airport_index = (s_store->airport_index + 1) % FM_AIRPORT_COUNT;
        } else if (s_settings_index == 1) {
            static const uint8_t levels[] = { 25, 50, 75, 100 };
            size_t level = 0;
            for (size_t i = 0; i < sizeof(levels); ++i) {
                if (levels[i] == s_store->brightness_percent) {
                    level = (i + 1) % (sizeof(levels) / sizeof(levels[0]));
                    break;
                }
            }
            s_store->brightness_percent = levels[level];
            bsp_display_backlight(s_store->brightness_percent);
        } else if (s_settings_index == 2) {
            fm_wifi_provisioning_start();
        } else if (s_settings_index == 3) {
            fm_wifi_forget();
        }
        fm_store_save(s_store);
    } else {
        return;
    }
    refresh_settings();
}

static void handle_key(const fm_key_event_t *event)
{
    if (!event) return;
    if (event->event == BSP_BTN_LONG && event->button == BSP_BTN_OK) {
        if (s_page != FM_PAGE_HOME) enter_page(FM_PAGE_HOME);
        return;
    }
    switch (s_page) {
    case FM_PAGE_HOME: handle_home_key(event->button, event->event); break;
    case FM_PAGE_WEATHER: handle_weather_key(event->button, event->event); break;
    case FM_PAGE_RADAR: handle_radar_key(event->button, event->event); break;
    case FM_PAGE_RADIO: handle_radio_key(event->button, event->event); break;
    case FM_PAGE_LOG: handle_log_key(event->button, event->event); break;
    case FM_PAGE_SETTINGS: handle_settings_key(event->button, event->event); break;
    }
}

static void radar_sweep_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_page == FM_PAGE_RADAR) refresh_radar_sweep();
}

static void ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    fm_key_event_t event;
    while (s_key_queue && xQueueReceive(s_key_queue, &event, 0) == pdTRUE) {
        handle_key(&event);
    }
    refresh_chrome();
    switch (s_page) {
    case FM_PAGE_HOME: refresh_home(); break;
    case FM_PAGE_WEATHER: refresh_weather(); break;
    case FM_PAGE_RADAR: refresh_radar(); break;
    case FM_PAGE_RADIO: refresh_radio(); break;
    case FM_PAGE_LOG: refresh_logbook(); break;
    case FM_PAGE_SETTINGS: refresh_settings(); break;
    }
}

/* Boot splash: shown until the first frame of the app is ready. */
static void splash_sweep_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_splash_sweep) return;
    const float radians = (float)s_splash_angle_deg * (float)M_PI / 180.0f;
    s_splash_sweep_pts[0].x = 62;
    s_splash_sweep_pts[0].y = 62;
    s_splash_sweep_pts[1].x = 62 + (lv_coord_t)(cosf(radians) * 60.0f);
    s_splash_sweep_pts[1].y = 62 - (lv_coord_t)(sinf(radians) * 60.0f);
    lv_line_set_points(s_splash_sweep, s_splash_sweep_pts, 2);
    s_splash_angle_deg = (uint16_t)((s_splash_angle_deg + 6) % 360);
    if (s_splash_fill) {
        uint32_t elapsed = monotonic_ms() - s_splash_started_ms;
        if (elapsed > FM_SPLASH_MS) elapsed = FM_SPLASH_MS;
        lv_obj_set_width(s_splash_fill, (int)(28 + (124 - 28) * elapsed / FM_SPLASH_MS));
    }
}

static void splash_done_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_splash_sweep_timer) { lv_timer_del(s_splash_sweep_timer); s_splash_sweep_timer = NULL; }
    lv_timer_del(s_splash_timer);
    s_splash_timer = NULL;
    lv_screen_load(s_screen);
}

static void build_splash(void)
{
    s_splash_screen = lv_obj_create(NULL);
    if (!s_splash_screen) return;
    lv_obj_set_size(s_splash_screen, 240, 320);
    lv_obj_set_style_bg_color(s_splash_screen, lv_color_hex(FM_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_splash_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_splash_screen, 0, 0);
    lv_obj_set_style_pad_all(s_splash_screen, 0, 0);
    lv_obj_clear_flag(s_splash_screen, LV_OBJ_FLAG_SCROLLABLE);

    /* pre-rendered backdrop: dark sky, A320 from above, radar rings, scrim */
    memset(&s_splash_bg_dsc, 0, sizeof(s_splash_bg_dsc));
    s_splash_bg_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_splash_bg_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_splash_bg_dsc.header.w = 240;
    s_splash_bg_dsc.header.h = 320;
    s_splash_bg_dsc.header.stride = 240 * 2;
    s_splash_bg_dsc.data_size = sizeof(fm_splash_bg_rgb565);
    s_splash_bg_dsc.data = fm_splash_bg_rgb565;
    lv_obj_t *backdrop = lv_image_create(s_splash_screen);
    lv_image_set_src(backdrop, &s_splash_bg_dsc);
    lv_obj_set_pos(backdrop, 0, 0);
    lv_image_set_scale(backdrop, 256);

    s_splash_sweep = lv_line_create(s_splash_screen);
    lv_obj_set_pos(s_splash_sweep, 58, 62);
    lv_obj_set_size(s_splash_sweep, 124, 124);
    lv_obj_set_style_line_width(s_splash_sweep, 3, 0);
    lv_obj_set_style_line_color(s_splash_sweep, lv_color_hex(FM_COLOR_RADAR), 0);
    lv_obj_set_style_line_rounded(s_splash_sweep, true, 0);
    s_splash_sweep_pts[0].x = 62; s_splash_sweep_pts[0].y = 62;
    s_splash_sweep_pts[1].x = 122; s_splash_sweep_pts[1].y = 62;
    lv_line_set_points(s_splash_sweep, s_splash_sweep_pts, 2);

    lv_obj_t *tip = lv_obj_create(s_splash_screen);
    lv_obj_set_pos(tip, 117, 121);
    lv_obj_set_size(tip, 6, 6);
    lv_obj_set_style_radius(tip, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(tip, 0, 0);
    lv_obj_set_style_bg_color(tip, lv_color_hex(FM_COLOR_RADAR), 0);
    lv_obj_clear_flag(tip, LV_OBJ_FLAG_SCROLLABLE);

    /* the same flight markers the radar page uses */
    static const struct { int x, y; const char *callsign; bool highlight; } kTracks[] = {
        {  96, 108, "MU5101", false },
        { 146, 150, "CZ3567", false },
        {  86, 158, "CA1832", false },
        { 150,  96, "HO1256", false },
        { 120, 100, "CES123", true  },
    };
    for (size_t i = 0; i < sizeof(kTracks) / sizeof(kTracks[0]); ++i) {
        const uint32_t tint = kTracks[i].highlight ? FM_COLOR_AMBER : FM_COLOR_RADAR;
        lv_obj_t *dot = lv_obj_create(s_splash_screen);
        lv_obj_set_pos(dot, kTracks[i].x - 3, kTracks[i].y - 3);
        lv_obj_set_size(dot, 6, 6);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(tint), 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);

        const int label_x = kTracks[i].x < 100 ? kTracks[i].x - 46 : kTracks[i].x + 6;
        const int label_w = kTracks[i].x < 100 ? 44 : 52;
        lv_obj_t *label = make_label(s_splash_screen, kTracks[i].callsign, label_x,
                                     kTracks[i].y - 9, label_w, tint, LV_TEXT_ALIGN_LEFT);
        lv_obj_set_style_text_font(label, &flightmate_ui_12, 0);
        lv_obj_set_height(label, 13);
        lv_obj_set_style_bg_color(label, lv_color_hex(FM_COLOR_RADAR_BG), 0);
        lv_obj_set_style_bg_opa(label, LV_OPA_70, 0);
        lv_obj_set_style_pad_left(label, 2, 0);
        lv_obj_set_style_pad_right(label, 2, 0);
    }

    lv_obj_t *title = make_label(s_splash_screen, "Flight Mate", 20, 212, 200, FM_COLOR_TEXT, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_text_letter_space(title, 3, 0);
    lv_obj_set_height(title, 22);
    lv_obj_t *sub = make_label(s_splash_screen, "飞行伴侣", 20, 238, 200, FM_COLOR_CYAN, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_text_letter_space(sub, 6, 0);
    lv_obj_set_height(sub, 18);

    lv_obj_t *bar = lv_obj_create(s_splash_screen);
    lv_obj_set_pos(bar, 58, 271);
    lv_obj_set_size(bar, 124, 3);
    lv_obj_set_style_radius(bar, 2, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(FM_COLOR_HAIR), 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    s_splash_fill = lv_obj_create(bar);
    lv_obj_set_pos(s_splash_fill, 0, 0);
    lv_obj_set_size(s_splash_fill, 28, 3);
    lv_obj_set_style_radius(s_splash_fill, 2, 0);
    lv_obj_set_style_border_width(s_splash_fill, 0, 0);
    lv_obj_set_style_bg_color(s_splash_fill, lv_color_hex(FM_COLOR_CYAN), 0);
    lv_obj_clear_flag(s_splash_fill, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hint = make_label(s_splash_screen, "正在启动", 20, 284, 200, FM_COLOR_MUTED, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_text_font(hint, &flightmate_ui_12, 0);
    lv_obj_set_height(hint, 16);
}

esp_err_t fm_ui_init(fm_store_t *store)
{
    if (!store) return ESP_ERR_INVALID_ARG;
    s_store = store;
    s_key_queue = xQueueCreate(8, sizeof(fm_key_event_t));
    if (!s_key_queue) return ESP_ERR_NO_MEM;
    s_screen = lv_obj_create(NULL);
    if (!s_screen) return ESP_ERR_NO_MEM;
    lv_obj_set_size(s_screen, 240, 320);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(FM_COLOR_BG_BOTTOM), 0);
    lv_obj_set_style_bg_grad_color(s_screen, lv_color_hex(FM_COLOR_BG), 0);
    lv_obj_set_style_bg_grad_dir(s_screen, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    s_page = FM_PAGE_HOME;
    s_recording_arrival_index = (store->airport_index + 1) % FM_AIRPORT_COUNT;
    bsp_display_backlight(store->brightness_percent);
    rebuild_page();
    build_splash();
    if (s_splash_screen) {
        s_splash_started_ms = monotonic_ms();
        lv_screen_load(s_splash_screen);
        s_splash_sweep_timer = lv_timer_create(splash_sweep_cb, 40, NULL);
        s_splash_timer = lv_timer_create(splash_done_cb, FM_SPLASH_MS, NULL);
    } else {
        lv_screen_load(s_screen);
    }
    s_timer = lv_timer_create(ui_timer_cb, 250, NULL);
    if (!s_timer) return ESP_ERR_NO_MEM;
    lv_timer_t *sweep_timer = lv_timer_create(radar_sweep_timer_cb, 40, NULL);
    if (!sweep_timer) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

void fm_ui_key_event(bsp_btn_t button, bsp_btn_ev_t event, void *user)
{
    if (!s_key_queue) return;
    const fm_key_event_t item = { .button = button, .event = event };
    (void)xQueueSend(s_key_queue, &item, 0);
}