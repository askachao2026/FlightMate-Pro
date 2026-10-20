// main/airband_stations.c
// 内置频道表：只登记实测返回 audio/mpeg 且能正常播放的公开流
// （http://d.liveatc.net/<mount>，会自动 302 到 HTTPS 媒体服务器）。
// 第一批（美国/日本）2026-10-10 实测；第二批（香港/瑞士/波兰/澳洲/拉斯维加斯/
// 肯尼迪合并席）2026-10-12 实测同样返回 audio/mpeg 200。
//
// 频率列：只在流的 icy-name 明确给出频率时才填写（如 KJFK 119.1、RJAA 124.400），
// 其余显示为占位符，运行时由 icy-name 自动补全，绝不凭空填写频率。
//
// 中国大陆没有合法的公开航空语音流，因此本表只含国际频道；本地/自建频道请用
// 列表末尾的"自定义频道"，在配网页面填入自己的 rtl_airband/Icecast 地址。
//
// 注意：LiveATC 条款限定个人收听，禁止再分发/转售。
#include "airband_stations.h"

static const airband_station_t s_stations[] = {
    // ---- 美国 ----
    { "纽约肯尼迪", "KJFK", "塔台", "119.100", "http://d.liveatc.net/kjfk_twr" },
    { "纽约肯尼迪", "KJFK", "地面", AIRBAND_FREQ_UNKNOWN, "http://d.liveatc.net/kjfk_gnd" },
    { "洛杉矶",     "KLAX", "塔台", AIRBAND_FREQ_UNKNOWN, "http://d.liveatc.net/klax_twr" },
    { "洛杉矶",     "KLAX", "地面", AIRBAND_FREQ_UNKNOWN, "http://d.liveatc.net/klax_gnd" },
    { "旧金山",     "KSFO", "塔台", AIRBAND_FREQ_UNKNOWN, "http://d.liveatc.net/ksfo_twr" },
    { "旧金山",     "KSFO", "地面", AIRBAND_FREQ_UNKNOWN, "http://d.liveatc.net/ksfo_gnd" },
    { "亚特兰大",   "KATL", "塔台", AIRBAND_FREQ_UNKNOWN, "http://d.liveatc.net/katl_twr" },
    { "纽瓦克",     "KEWR", "塔台", AIRBAND_FREQ_UNKNOWN, "http://d.liveatc.net/kewr_twr" },
    { "拉瓜迪亚",   "KLGA", "塔台", AIRBAND_FREQ_UNKNOWN, "http://d.liveatc.net/klga_twr" },
    { "底特律",     "KDTW", "塔台", AIRBAND_FREQ_UNKNOWN, "http://d.liveatc.net/kdtw_twr" },
    { "纳什维尔",   "KBNA", "塔台", AIRBAND_FREQ_UNKNOWN, "http://d.liveatc.net/kbna_twr" },
    // ---- 日本 ----
    { "东京羽田",   "RJTT", "塔台", AIRBAND_FREQ_UNKNOWN, "http://d.liveatc.net/rjtt_twr" },
    { "东京羽田",   "RJTT", "进近", AIRBAND_FREQ_UNKNOWN, "http://d.liveatc.net/rjtt_app" },
    { "东京羽田",   "RJTT", "地面", AIRBAND_FREQ_UNKNOWN, "http://d.liveatc.net/rjtt_gnd" },
    { "东京羽田",   "RJTT", "通播", AIRBAND_FREQ_UNKNOWN, "http://d.liveatc.net/rjtt_atis" },
    { "东京成田",   "RJAA", "塔台", AIRBAND_FREQ_UNKNOWN, "http://d.liveatc.net/rjaa_twr" },
    { "东京成田",   "RJAA", "进近", "124.400",            "http://d.liveatc.net/rjaa_app" },
    // ---- 香港（第二批实测）----
    { "香港赤鱲角", "VHHH", "进近", AIRBAND_FREQ_UNKNOWN, "http://d.liveatc.net/vhhh5" },
    { "香港赤鱲角", "VHHH", "塔台", AIRBAND_FREQ_UNKNOWN, "http://d.liveatc.net/vhhh9" },
    // ---- 欧洲（第二批实测）----
    { "苏黎世",     "LSZH", "塔台", AIRBAND_FREQ_UNKNOWN, "http://d.liveatc.net/lszh1_twr" },
    { "弗罗茨瓦夫", "EPWR", "进近", AIRBAND_FREQ_UNKNOWN, "http://d.liveatc.net/epwr4_app2" },
    // ---- 大洋洲（第二批实测）----
    { "墨尔本",     "YMML", "综合", AIRBAND_FREQ_UNKNOWN, "http://d.liveatc.net/ymml3" },
    // ---- 美国（第二批实测补充）----
    { "拉斯维加斯", "KLAS", "塔台",     AIRBAND_FREQ_UNKNOWN, "http://d.liveatc.net/klas4_twr" },
    { "纽约肯尼迪", "KJFK", "地面塔台", AIRBAND_FREQ_UNKNOWN, "http://d.liveatc.net/kjfk9_s" },
    // ---- 第三批（作者提供，2026-10-20）----
    { "台北松山",   "RCSS", "综合", AIRBAND_FREQ_UNKNOWN, "http://d.liveatc.net/rcss2" },
    { "檀香山",     "PHNL", "塔台", AIRBAND_FREQ_UNKNOWN, "http://d.liveatc.net/phnl1_twr" },
    { "新加坡樟宜", "WSSS", "综合", AIRBAND_FREQ_UNKNOWN, "http://d.liveatc.net/wsss3" },
    { "雅加达",     "WIII", "离场", "119.750",            "http://d.liveatc.net/wiii_dep_119750" },
    { "多伦多皮尔逊","CYYZ", "进近", "132.800",            "http://d.liveatc.net/cyyz1_app_132800" },
    // ---- 自定义频道：地址由配网页面写入 NVS ----
};

const airband_station_t *airband_stations(void) { return s_stations; }

size_t airband_station_count(void) { return sizeof(s_stations) / sizeof(s_stations[0]); }

size_t airband_custom_station_index(void) { return airband_station_count() - 1; }
