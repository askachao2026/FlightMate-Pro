// main/airband_logic.h
// 网络航空波段收音机的纯逻辑：不依赖 ESP-IDF / LVGL，可脱离硬件做主机测试。
#pragma once

#include <stdbool.h>
#include <stddef.h>

// 频道选择环绕。delta 通常为 +1/-1；index 越界时先夹到有效范围再环绕。
// count<=0 时返回 0。无副作用。
int airband_wrap_index(int index, int count, int delta);

// 音量夹取到 0..100。非数字输入由调用方保证为 int。
int airband_clamp_volume(int percent);

// 音量步进：percent + dir*10，再夹取。dir 为正/负/零。
int airband_volume_step(int percent, int dir);

// 从 Icecast 的 icy-name（如 "KJFK Tower 119.1"）提取 118-137 MHz 频率，
// 规范化为 "119.100" 写入 out。成功返回 true；未找到返回 false 且 out 保持空串。
// 该函数只做字符串解析，不做任何 I/O。
bool airband_extract_frequency(const char *icy_name, char *out, size_t out_len);

// 校验用户填写的流地址：必须是 http:// 或 https://，且主机段非空、无空白字符。
bool airband_url_is_valid(const char *url);

// 规范化后的频率文本是否仍是占位符。
bool airband_freq_is_unknown(const char *freq_text);

// 解析 HTTP 重定向目标：支持绝对地址（http/https）与以 '/' 开头的绝对路径。
// 成功写入 out 并返回 true；相对路径、空值或超长返回 false。
bool airband_resolve_redirect(const char *base_url, const char *location,
                              char *out, size_t out_len);
