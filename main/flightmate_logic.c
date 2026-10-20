#include "flightmate_logic.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define EARTH_RADIUS_KM 6371.0088f

const fm_airport_t FM_AIRPORTS[] = {
    { "ZSPD", "上海浦东", 31.1443f, 121.8083f },
    { "ZSSS", "上海虹桥", 31.1979f, 121.3363f },
    { "ZBAA", "北京首都", 40.0799f, 116.6031f },
    { "ZBAD", "北京大兴", 39.5098f, 116.4105f },
    { "ZGGG", "广州白云", 23.3924f, 113.2988f },
    { "ZGSZ", "深圳宝安", 22.6393f, 113.8107f },
    { "ZUUU", "成都双流", 30.5785f, 103.9471f },
    { "ZUTF", "成都天府", 30.3125f, 104.4413f },
    { "ZPPP", "昆明长水", 25.1019f, 102.9292f },
    { "ZSHC", "杭州萧山", 30.2295f, 120.4344f },
    { "ZSAM", "厦门高崎", 24.5440f, 118.1277f },
    { "ZSQD", "青岛胶东", 36.3614f, 120.0883f },
    { "ZGHA", "长沙黄花", 28.1892f, 113.2196f },
    { "KJFK", "纽约肯尼迪", 40.6413f, -73.7781f },
    { "KLAX", "洛杉矶", 33.9416f, -118.4085f },
    { "EGLL", "伦敦希思罗", 51.4700f, -0.4543f },
    { "LFPG", "巴黎戴高乐", 49.0097f, 2.5479f },
    { "EDDF", "法兰克福", 50.0379f, 8.5622f },
    { "OMDB", "迪拜", 25.2532f, 55.3657f },
    { "RJTT", "东京羽田", 35.5494f, 139.7798f },
    { "WSSS", "新加坡樟宜", 1.3644f, 103.9915f },
    { "KSFO", "旧金山", 37.6189f, -122.3750f },
    { "KATL", "亚特兰大", 33.6407f, -84.4277f },
    { "KEWR", "纽瓦克", 40.6895f, -74.1745f },
    { "KLGA", "拉瓜迪亚", 40.7769f, -73.8740f },
    { "KDTW", "底特律", 42.2162f, -83.3554f },
    { "KBNA", "纳什维尔", 36.1263f, -86.6774f },
    { "RJAA", "东京成田", 35.7720f, 140.3929f },
    { "VHHH", "香港赤鱲角", 22.3080f, 113.9185f },
    { "LSZH", "苏黎世", 47.4647f, 8.5492f },
    { "EPWR", "弗罗茨瓦夫", 51.1027f, 16.8858f },
    { "YMML", "墨尔本", -37.6733f, 144.8433f },
    { "KLAS", "拉斯维加斯", 36.0840f, -115.1537f },
    { "RCSS", "台北松山", 25.0694f, 121.5525f },
    { "PHNL", "檀香山", 21.3187f, -157.9224f },
    { "WIII", "雅加达", -6.1256f, 106.6559f },
    { "CYYZ", "多伦多皮尔逊", 43.6777f, -79.6248f },
};

const size_t FM_AIRPORT_COUNT = sizeof(FM_AIRPORTS) / sizeof(FM_AIRPORTS[0]);

void fm_copy_text(char *destination, size_t capacity, const char *source)
{
    if (!destination || capacity == 0) return;
    if (!source) source = "";
    snprintf(destination, capacity, "%s", source);
}

static float degrees_to_radians(float degrees)
{
    return degrees * (M_PI / 180.0f);
}

static float normalize_degrees(float degrees)
{
    while (degrees < 0.0f) degrees += 360.0f;
    while (degrees >= 360.0f) degrees -= 360.0f;
    return degrees;
}

float fm_distance_km(float lat1, float lon1, float lat2, float lon2)
{
    const float dlat = degrees_to_radians(lat2 - lat1);
    const float dlon = degrees_to_radians(lon2 - lon1);
    const float a = sinf(dlat / 2.0f) * sinf(dlat / 2.0f) +
                    cosf(degrees_to_radians(lat1)) * cosf(degrees_to_radians(lat2)) *
                    sinf(dlon / 2.0f) * sinf(dlon / 2.0f);
    return EARTH_RADIUS_KM * 2.0f * atan2f(sqrtf(a), sqrtf(1.0f - a));
}

float fm_bearing_deg(float lat1, float lon1, float lat2, float lon2)
{
    const float phi1 = degrees_to_radians(lat1);
    const float phi2 = degrees_to_radians(lat2);
    const float lambda = degrees_to_radians(lon2 - lon1);
    const float y = sinf(lambda) * cosf(phi2);
    const float x = cosf(phi1) * sinf(phi2) - sinf(phi1) * cosf(phi2) * cosf(lambda);
    return normalize_degrees(atan2f(y, x) * 180.0f / M_PI);
}

static int compare_flights(const void *left, const void *right)
{
    const fm_flight_t *a = left;
    const fm_flight_t *b = right;
    if (a->distance_km < b->distance_km) return -1;
    if (a->distance_km > b->distance_km) return 1;
    return strcmp(a->callsign, b->callsign);
}

void fm_sort_flights_by_distance(fm_flight_t *flights, size_t count)
{
    if (flights && count > 1) qsort(flights, count, sizeof(flights[0]), compare_flights);
}

fm_flight_status_t fm_classify_flight(const fm_flight_t *flight)
{
    if (!flight) return FM_FLIGHT_STATUS_ENROUTE;
    if (flight->on_ground) {
        return flight->speed_kmh > 50.0f ? FM_FLIGHT_STATUS_TAKEOFF : FM_FLIGHT_STATUS_ARRIVED;
    }
    if (flight->altitude_m >= 0.0f && flight->altitude_m < 2500.0f &&
        flight->vertical_rate_mps > 1.0f && flight->speed_kmh > 80.0f) {
        return FM_FLIGHT_STATUS_TAKEOFF;
    }
    return FM_FLIGHT_STATUS_ENROUTE;
}

const char *fm_flight_status_text(fm_flight_status_t status)
{
    switch (status) {
    case FM_FLIGHT_STATUS_TAKEOFF: return "起飞";
    case FM_FLIGHT_STATUS_ARRIVED: return "到达";
    case FM_FLIGHT_STATUS_ENROUTE:
    default: return "飞行中";
    }
}

void fm_airline_display_name(const char *callsign, const char *fallback,
                             char *out, size_t capacity)
{
    static const struct {
        const char *prefix;
        const char *name;
    } AIRLINES[] = {
        { "CCA", "中国国航" }, { "CES", "东方航空" }, { "CSN", "南方航空" },
        { "CHH", "海南航空" }, { "CSZ", "深圳航空" }, { "CQH", "春秋航空" },
        { "CUA", "中国联航" }, { "CXA", "厦门航空" }, { "DKH", "吉祥航空" },
        { "HXA", "华夏航空" }, { "JYH", "九元航空" }, { "LKE", "祥鹏航空" },
        { "OKA", "奥凯航空" }, { "RLH", "瑞丽航空" }, { "CPA", "国泰航空" },
        { "SIA", "新加坡航空" }, { "JAL", "日本航空" }, { "ANA", "全日空" },
        { "BAW", "英国航空" }, { "DLH", "汉莎航空" }, { "AFR", "法国航空" },
        { "KLM", "荷兰航空" }, { "UAE", "阿联酋航空" }, { "QTR", "卡塔尔航空" },
        { "ETD", "阿提哈德" }, { "THA", "泰国航空" }, { "UAL", "美国联合航空" },
        { "DAL", "达美航空" }, { "AAL", "美国航空" },
    };
    if (callsign) {
        for (size_t i = 0; i < sizeof(AIRLINES) / sizeof(AIRLINES[0]); ++i) {
            if (strncmp(callsign, AIRLINES[i].prefix, 3) == 0) {
                fm_copy_text(out, capacity, AIRLINES[i].name);
                return;
            }
        }
    }
    fm_copy_text(out, capacity, fallback && fallback[0] ? fallback : "未知");
}


static bool is_upper_icao(const char *token)
{
    if (!token || strlen(token) != 4) return false;
    for (size_t i = 0; i < 4; ++i) {
        if (!isupper((unsigned char)token[i])) return false;
    }
    return true;
}

static int parse_int_prefix(const char *token, int *consumed)
{
    char *end = NULL;
    long value = strtol(token, &end, 10);
    if (end == token) {
        if (consumed) *consumed = 0;
        return 0;
    }
    if (consumed) *consumed = (int)(end - token);
    return (int)value;
}

static int mps_to_knots(int mps)
{
    return (int)(mps * 1.94384f + 0.5f);
}

static void parse_wind(const char *token, fm_metar_t *out)
{
    if (!token) return;
    /* ICAO METAR reports the wind in KT or MPS depending on the region. */
    const char *unit = strstr(token, "KT");
    const bool is_mps = unit == NULL;
    if (!unit) unit = strstr(token, "MPS");
    if (!unit) return;
    const size_t digits = (size_t)(unit - token);
    if (digits < 5) return;

    int direction = -1;
    if (strncmp(token, "VRB", 3) != 0) {
        direction = (token[0] - '0') * 100 + (token[1] - '0') * 10 + (token[2] - '0');
    }
    const char *speed_start = token + 3;
    const char *gust = strchr(speed_start, 'G');
    char speed[4] = { 0 };
    size_t speed_len = gust && gust < unit ? (size_t)(gust - speed_start) : (size_t)(unit - speed_start);
    if (speed_len == 0 || speed_len >= sizeof(speed)) return;
    memcpy(speed, speed_start, speed_len);
    int value = atoi(speed);
    if (is_mps) value = mps_to_knots(value);
    out->wind_direction_deg = direction;
    out->wind_speed_kt = value;
    if (gust && gust < unit) {
        int gust_value = atoi(gust + 1);
        if (is_mps) gust_value = mps_to_knots(gust_value);
        out->wind_gust_kt = gust_value;
    }
}

static void parse_visibility(const char *token, fm_metar_t *out)
{
    if (!token) return;
    if (strcmp(token, "CAVOK") == 0) {
        fm_copy_text(out->visibility, sizeof(out->visibility), ">=10km");
        return;
    }
    const size_t length = strlen(token);
    if (length > 2 && strcmp(token + length - 2, "SM") == 0) {
        char value[12];
        size_t copy = length - 2;
        if (copy >= sizeof(value)) copy = sizeof(value) - 1;
        memcpy(value, token, copy);
        value[copy] = '\0';
        snprintf(out->visibility, sizeof(out->visibility), "%sSM", value);
        return;
    }
    bool digits = length >= 4;
    for (size_t i = 0; digits && i < length; ++i) digits = isdigit((unsigned char)token[i]);
    if (digits) {
        const int meters = atoi(token);
        if (meters >= 1000) snprintf(out->visibility, sizeof(out->visibility), "%dkm", meters / 1000);
        else snprintf(out->visibility, sizeof(out->visibility), "%dm", meters);
    }
}

static bool parse_temperature_pair(const char *token, int *temperature, int *dewpoint)
{
    if (!token) return false;
    const char *slash = strchr(token, '/');
    if (!slash || slash == token || slash[1] == '\0') return false;

    int value = 0;
    char *end = NULL;
    if (token[0] == 'M') {
        value = -(int)strtol(token + 1, &end, 10);
    } else {
        value = (int)strtol(token, &end, 10);
    }
    if (end != slash) return false;
    *temperature = value;

    if (slash[1] == 'M') {
        *dewpoint = -(int)strtol(slash + 2, NULL, 10);
    } else {
        *dewpoint = (int)strtol(slash + 1, NULL, 10);
    }
    return true;
}

bool fm_parse_metar(const char *raw, fm_metar_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->wind_direction_deg = -1;
    fm_copy_text(out->raw, sizeof(out->raw), raw);
    if (!raw || !raw[0]) return false;

    char work[FM_METAR_RAW_MAX];
    fm_copy_text(work, sizeof(work), raw);

    char *save = NULL;
    char *token = strtok_r(work, " \t\r\n", &save);
    /* AviationWeather "format=raw" lines are prefixed with the report type:
       METAR ZSPD 011830Z ...  Skip it so the station id is the first token. */
    while (token && (strcmp(token, "METAR") == 0 || strcmp(token, "SPECI") == 0 ||
                     strcmp(token, "COR") == 0)) {
        token = strtok_r(NULL, " \t\r\n", &save);
    }
    if (!token) return false;
    if (is_upper_icao(token)) {
        out->valid = true;
        fm_copy_text(out->station, sizeof(out->station), token);
        token = strtok_r(NULL, " \t\r\n", &save);
    }

    const char *wind_token = NULL;
    while (token) {
        if (strstr(token, "KT") || strstr(token, "MPS")) {
            parse_wind(token, out);
            wind_token = token;
            token = strtok_r(NULL, " \t\r\n", &save);
            break;
        }
        if (strlen(token) == 7 && token[6] == 'Z') {
            snprintf(out->observed, sizeof(out->observed), "%.4s", token + 2);
        }
        token = strtok_r(NULL, " \t\r\n", &save);
    }

    if (wind_token && token) {
        parse_visibility(token, out);
        token = strtok_r(NULL, " \t\r\n", &save);
    }

    while (token) {
        if (strchr(token, '/') && !out->has_temperature) {
            int temp = 0, dew = 0;
            if (parse_temperature_pair(token, &temp, &dew)) {
                out->temperature_c = temp;
                out->dewpoint_c = dew;
                out->has_temperature = true;
                out->has_dewpoint = true;
            }
        } else if (token[0] == 'Q' && isdigit((unsigned char)token[1])) {
            int consumed = 0;
            out->qnh_hpa = parse_int_prefix(token + 1, &consumed);
            out->has_qnh = consumed > 0;
        } else if (strlen(token) == 7 && token[6] == 'Z') {
            snprintf(out->observed, sizeof(out->observed), "%.4s", token + 2);
        }
        token = strtok_r(NULL, " \t\r\n", &save);
    }

    char wind_text[32] = "风 静";
    if (out->wind_direction_deg >= 0) {
        snprintf(wind_text, sizeof(wind_text), "风 %03d° %dkt", out->wind_direction_deg, out->wind_speed_kt);
    } else if (out->wind_speed_kt > 0) {
        snprintf(wind_text, sizeof(wind_text), "风 变化 %dkt", out->wind_speed_kt);
    }
    if (out->wind_gust_kt > 0) {
        size_t used = strlen(wind_text);
        snprintf(wind_text + used, sizeof(wind_text) - used, " 阵风%d", out->wind_gust_kt);
    }

    char temperature[40] = "温度 --";
    if (out->has_temperature) {
        snprintf(temperature, sizeof(temperature), "温度 %d°C", out->temperature_c);
    }
    char qnh[24] = "";
    if (out->has_qnh) snprintf(qnh, sizeof(qnh), " Q%d", out->qnh_hpa);
    snprintf(out->summary, sizeof(out->summary), "%s %s %s%s",
             wind_text,
             out->visibility[0] ? out->visibility : "能见度 --",
             temperature,
             qnh);

    if (!out->station[0]) fm_copy_text(out->station, sizeof(out->station), "----");
    return out->valid;
}

void fm_format_local_hhmm(uint32_t utc_seconds, char *out, size_t capacity)
{
    if (!out || capacity == 0) return;
    time_t local_seconds = (time_t)utc_seconds + 8 * 3600;
    struct tm value;
    const struct tm *converted = gmtime(&local_seconds);
    if (!converted) return;
    value = *converted;
    snprintf(out, capacity, "%02d:%02d", value.tm_hour, value.tm_min);
}

void fm_format_local_date(uint32_t utc_seconds, char *out, size_t capacity)
{
    if (!out || capacity == 0) return;
    time_t local_seconds = (time_t)utc_seconds + 8 * 3600;
    struct tm value;
    const struct tm *converted = gmtime(&local_seconds);
    if (!converted) return;
    value = *converted;
    snprintf(out, capacity, "%04d-%02d-%02d", value.tm_year + 1900, value.tm_mon + 1, value.tm_mday);
}

void fm_format_duration(uint32_t seconds, char *out, size_t capacity)
{
    if (!out || capacity == 0) return;
    const uint32_t hours = seconds / 3600;
    const uint32_t minutes = (seconds % 3600) / 60;
    if (hours > 0) snprintf(out, capacity, "%u小时%u分", (unsigned)hours, (unsigned)minutes);
    else snprintf(out, capacity, "%u分", (unsigned)minutes);
}

const fm_airport_t *fm_airport_by_index(size_t index)
{
    return index < FM_AIRPORT_COUNT ? &FM_AIRPORTS[index] : NULL;
}

int fm_airport_index_by_icao(const char *icao)
{
    if (!icao) return -1;
    for (size_t i = 0; i < FM_AIRPORT_COUNT; ++i) {
        if (strcmp(FM_AIRPORTS[i].icao, icao) == 0) return (int)i;
    }
    return -1;
}