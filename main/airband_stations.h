// main/airband_stations.h
// 网络航空波段收音机：内置频道（预设）数据模型。
//
// 说明：本机没有 118-137 MHz 射频接收电路，应用播放的是公开网络语音流
// （LiveATC 等 Icecast/MP3 流）。预设只登记已实测可用的频道；没有实测到
// 频率的频道把 freq_text 记为 "--.---"，运行时用流的 icy-name 元数据补充，
// 绝不在代码里凭空填写频率。
#pragma once

#include <stddef.h>

// 单个预设频道。
typedef struct {
    const char *name_zh;   // 中文台站名，例如 "纽约肯尼迪"`
    const char *icao;      // ICAO 四字代码，例如 "KJFK"
    const char *kind_zh;   // 席位类型，例如 "塔台" / "进近" / "地面" / "通播"
    const char *freq_text; // 频率显示文本；未知时固定为 "--.---"
    const char *url;       // HTTP(S) MP3 流地址
} airband_station_t;

// 返回内置预设数组（只读，静态存储，无需释放）。数组长度为 airband_station_count()。
const airband_station_t *airband_stations(void);

// 预设数量。
size_t airband_station_count(void);

// 自定义频道的下标（始终是最后一个预设）。调用方用它区分“内置”与“用户自定义”。
size_t airband_custom_station_index(void);

// 频率未知时使用的占位文本。
#define AIRBAND_FREQ_UNKNOWN "--.---"
