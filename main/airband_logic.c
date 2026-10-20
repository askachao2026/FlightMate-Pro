// main/airband_logic.c
#include "airband_logic.h"

#include <ctype.h>
#include <string.h>

// 本地安全拷贝：strlcpy 在 ESP-IDF 可用，但主机测试用的 glibc/MinGW 没有。
static void copy_cstr(char *dst, const char *src, size_t dst_len)
{
    if (!dst || dst_len == 0) {
        return;
    }
    size_t n = 0;
    if (src) {
        while (n + 1 < dst_len && src[n] != '\0') {
            dst[n] = src[n];
            n++;
        }
    }
    dst[n] = '\0';
}

static int pow10_int(int exp)
{
    int v = 1;
    while (exp-- > 0) {
        v *= 10;
    }
    return v;
}

int airband_wrap_index(int index, int count, int delta)
{
    if (count <= 0) {
        return 0;
    }
    // 先把越界输入夹回有效区间，避免负取模在 C 里实现相关。
    if (index < 0) {
        index = 0;
    }
    if (index >= count) {
        index = count - 1;
    }
    int next = (index + delta) % count;
    if (next < 0) {
        next += count;
    }
    return next;
}

int airband_clamp_volume(int percent)
{
    if (percent < 0) {
        return 0;
    }
    if (percent > 100) {
        return 100;
    }
    return percent;
}

int airband_volume_step(int percent, int dir)
{
    return airband_clamp_volume(percent + dir * 10);
}

bool airband_extract_frequency(const char *icy_name, char *out, size_t out_len)
{
    if (out && out_len > 0) {
        out[0] = '\0';
    }
    if (!icy_name || !out || out_len < 8) {
        return false;
    }

    const size_t len = strlen(icy_name);
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)icy_name[i])) {
            continue;
        }
        // 只接受“独立”的数字段，避免把 "RJTT" 之后的序号等当成频率。
        if (i > 0 && isdigit((unsigned char)icy_name[i - 1])) {
            continue;
        }

        size_t j = i;
        int whole = 0;
        int whole_digits = 0;
        while (j < len && isdigit((unsigned char)icy_name[j])) {
            if (whole_digits < 6) {
                whole = whole * 10 + (icy_name[j] - '0');
            }
            whole_digits++;
            j++;
        }
        if (whole_digits != 3 || j >= len || icy_name[j] != '.') {
            continue;
        }
        if (whole < 100 || whole > 140) {
            continue;
        }

        size_t k = j + 1;
        int frac = 0;
        int frac_digits = 0;
        while (k < len && isdigit((unsigned char)icy_name[k])) {
            if (frac_digits < 6) {
                frac = frac * 10 + (icy_name[k] - '0');
            }
            frac_digits++;
            k++;
        }
        if (frac_digits < 1) {
            continue;
        }

        // 统一成三位小数：1 -> 100，12 -> 120，123 -> 123，超过三位截断。
        int frac3 = frac;
        if (frac_digits == 1) {
            frac3 *= 100;
        } else if (frac_digits == 2) {
            frac3 *= 10;
        } else if (frac_digits > 3) {
            frac3 /= pow10_int(frac_digits - 3);
        }
        if (frac3 > 999) {
            frac3 = 999;
        }
        // 需要 10 字节："119.100" + '\0'。
        if (out_len < 8) {
            return false;
        }
        int written = 0;
        // 手工格式化，避免依赖 snprintf 的可配置实现。
        out[written++] = (char)('0' + (whole / 100) % 10);
        out[written++] = (char)('0' + (whole / 10) % 10);
        out[written++] = (char)('0' + whole % 10);
        out[written++] = '.';
        out[written++] = (char)('0' + (frac3 / 100) % 10);
        out[written++] = (char)('0' + (frac3 / 10) % 10);
        out[written++] = (char)('0' + frac3 % 10);
        out[written] = '\0';
        return true;
    }
    return false;
}

bool airband_url_is_valid(const char *url)
{
    if (!url) {
        return false;
    }
    const char *host = NULL;
    if (strncmp(url, "http://", 7) == 0) {
        host = url + 7;
    } else if (strncmp(url, "https://", 8) == 0) {
        host = url + 8;
    } else {
        return false;
    }
    if (!host || *host == '\0') {
        return false;
    }

    size_t host_len = 0;
    for (const char *p = host; *p != '\0' && *p != '/'; p++) {
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            return false;
        }
        host_len++;
    }
    return host_len >= 3 && host_len < 128;
}

bool airband_freq_is_unknown(const char *freq_text)
{
    return !freq_text || strcmp(freq_text, "--.---") == 0;
}

bool airband_resolve_redirect(const char *base_url, const char *location,
                              char *out, size_t out_len)
{
    if (!base_url || !location || !location[0] || !out || out_len == 0) {
        return false;
    }
    if (strncmp(location, "http://", 7) == 0 || strncmp(location, "https://", 8) == 0) {
        if (strlen(location) >= out_len) {
            return false;
        }
        copy_cstr(out, location, out_len);
        return true;
    }
    if (location[0] != '/') {
        return false;
    }
    const char *scheme_end = strstr(base_url, "://");
    if (!scheme_end) {
        return false;
    }
    const char *authority = scheme_end + 3;
    const char *slash = strchr(authority, '/');
    size_t prefix = slash ? (size_t)(slash - base_url) : strlen(base_url);
    if (prefix + strlen(location) + 1 > out_len) {
        return false;
    }
    memcpy(out, base_url, prefix);
    copy_cstr(out + prefix, location, out_len - prefix);
    return true;
}
