// main/airband_audio.h
// 网络音频流播放：HTTP(S) 读取 Icecast/MP3 流，用 libhelix-mp3 解码，
// 再经 BSP 的 ES8311 播放。所有耗时操作都在独立任务中，不阻塞按键与 UI。
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    AIRBAND_AUDIO_IDLE = 0,   // 未播放
    AIRBAND_AUDIO_CONNECTING, // 正在建立 HTTP 连接
    AIRBAND_AUDIO_PLAYING,    // 正在解码播放
    AIRBAND_AUDIO_RETRYING,   // 流中断，等待重连
    AIRBAND_AUDIO_ERROR,      // 连续失败或参数错误
} airband_audio_state_t;

typedef struct {
    airband_audio_state_t state; // 当前状态
    char stream_name[56];        // 流上报的 icy-name（可能为空）
    char detail[56];             // 简短错误/提示文本（可能为空）
    uint32_t frames;             // 本次会话已解码帧数，用于诊断
} airband_audio_status_t;

// 初始化音频 BSP 与播放任务（幂等）。不联网、不播放。
esp_err_t airband_audio_init(void);

// 输出音量 0..100，立即生效并记住，供后续会话使用。
void airband_audio_set_volume(uint8_t percent);

// 当前音量百分比（0..100）。
uint8_t airband_audio_volume(void);

// 开始播放 URL。会先停止上一个会话；非阻塞。
esp_err_t airband_audio_play(const char *url);

// 立刻归还 Helix 解码器（未播放时生效），供联网前腾内存。
void airband_audio_release_decoder(void);

// 请求停止并等待任务回到空闲（有界等待，不强制删除任务）。
void airband_audio_stop(void);

// 是否有播放会话处于活动状态（含连接/重连）。
bool airband_audio_is_active(void);

// 读取状态快照（线程安全）。
void airband_audio_get_status(airband_audio_status_t *out);

// 进入配网等内存敏感流程前释放 codec（保留任务与 I2S channel）。
void airband_audio_release(void);

// 配网结束后重新初始化 codec 并恢复音量。
esp_err_t airband_audio_reacquire(void);
