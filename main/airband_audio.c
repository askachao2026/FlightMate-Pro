// main/airband_audio.c
#include "airband_audio.h"
/* FlightMate 版：不引入 airband_storage，直接给出流地址长度上限。 */
#define AIRBAND_URL_MAX 193
#include "airband_logic.h"
#include "bsp_audio.h"

#include "airband_extra_ca.h"
#include "minimp3.h"
#include "mp3dec.h"   // Helix：22 kHz 流的实时解码路径
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "airband_audio";

#define AUDIO_TASK_STACK      24576  // TLS 握手 + minimp3 解码都需要较深栈
#define AUDIO_TASK_PRIORITY   5
#define IN_BUF_SIZE           4096
// Helix 单个 MP3 帧最多 2 通道 x 2 粒度 x 576 采样。
#define OUT_SAMPLES_MAX       (2 * 2 * 576 + 64)
#define HTTP_TIMEOUT_MS       6000
#define RETRY_DELAY_MS        2500
#define MAX_OPEN_FAILURES     4
#define STOP_WAIT_STEPS       80   // 80 x 100ms = 8s 上限
// Helix 常驻要 25KB；空闲堆低于这个值时本次会话退回 minimp3，
// 保证 http 客户端还有内存把流拉起来（下次空闲堆变大还会再试）。
#define HELIX_MIN_FREE_HEAP   45000

static TaskHandle_t s_task;
static SemaphoreHandle_t s_lock;
static volatile bool s_active;    // 有播放会话
static volatile bool s_stop;      // 请求取消
static char s_url[AIRBAND_URL_MAX];
static airband_audio_status_t s_status;
static bool s_audio_ready;
static volatile bool s_task_ready;
static volatile bool s_helix_release;   // 收到后由解码任务释放 Helix   // 一次性分配完成的标志
static uint8_t s_volume = 70;

static void status_set(airband_audio_state_t state, const char *detail)
{
    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        s_status.state = state;
        if (detail) {
            strlcpy(s_status.detail, detail, sizeof(s_status.detail));
        } else {
            s_status.detail[0] = '\0';
        }
        xSemaphoreGive(s_lock);
    }
}

static void status_set_name(const char *name)
{
    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        if (name) {
            strlcpy(s_status.stream_name, name, sizeof(s_status.stream_name));
        } else {
            s_status.stream_name[0] = '\0';
        }
        xSemaphoreGive(s_lock);
    }
}

void airband_audio_get_status(airband_audio_status_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!s_lock) {
        return;
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        *out = s_status;
        xSemaphoreGive(s_lock);
    }
}

void airband_audio_set_volume(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    s_volume = percent;
    if (s_audio_ready) {
        bsp_audio_set_volume(percent);
    }
}

uint8_t airband_audio_volume(void)
{
    return s_volume;
}

#define MAX_REDIRECT_HOPS 5
#define REDIRECT_URL_MAX  256

// LiveATC 会返回 302 并要求跟随到 HTTPS 的 s1-bos.liveatc.net。ESP-IDF 的手动
// open/fetch/read 流程不会自动跟随重定向（只有 esp_http_client_perform() 会），
// 且 esp_http_client_get_header() 只能读“请求头”，读不到响应里的 Location。
// 因此这里关闭自动重定向，用 HTTP_EVENT_ON_HEADER 自己抓 Location，再重新请求。
static char s_redirect_location[REDIRECT_URL_MAX];
static bool s_redirect_location_valid;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key && evt->header_value) {
        if (strcasecmp(evt->header_key, "Location") == 0) {
            strlcpy(s_redirect_location, evt->header_value, sizeof(s_redirect_location));
            s_redirect_location_valid = true;
        }
        else if (strcasecmp(evt->header_key, "icy-name") == 0) {
            status_set_name(evt->header_value);
        }
    }
    return ESP_OK;
}

// LiveATC 的媒体服务器同时支持明文 HTTP(80)。改用 HTTP 可完全避开 TLS：
// 既不受其"只下发叶证书"导致的 X.509 校验失败影响，又省下约 37KB TLS 内存
// 与加解密开销（让 Helix 解码器能常驻、22kHz 频道跑满实时）。
// 只对 liveatc.net 生效，自定义流地址保持原样（仍用 TLS + 系统 CA 包校验）。
static void prefer_plain_http_for_liveatc(char *url)
{
    if (url && strncmp(url, "https://", 8) == 0 && strstr(url, "liveatc.net") != NULL) {
        memmove(url + 4, url + 5, strlen(url + 5) + 1);   // https:// -> http://
    }
}

// 打开 HTTP(S) 流并手动跟随 301/302/303/307/308，最多 MAX_REDIRECT_HOPS 跳。
// 成功返回已建立连接的客户端句柄，调用方负责 cleanup；失败返回 NULL。
//
// 证书策略：先做正常校验（liveatc.net 用内置补充 CA：中间证书+根证书；其它主机用
// ESP-IDF 的完整 CA 包）。若 liveatc.net 的 TLS 校验失败，再退化为不校验证书重试一次
// ——LiveATC 的媒体服务器不下发中间证书，且会按客户端能力下发不同叶证书，链路组合
// 不稳定；这是公开音频流，可用性优先（退化时会记录日志）。
static esp_http_client_handle_t http_open(const char *url)
{
    char current[REDIRECT_URL_MAX];
    if (!url || strlen(url) >= sizeof(current)) {
        return NULL;
    }
    strlcpy(current, url, sizeof(current));
    prefer_plain_http_for_liveatc(current);

    for (int hop = 0; hop < MAX_REDIRECT_HOPS; hop++) {
        bool is_liveatc = (strstr(current, "liveatc.net") != NULL);
        esp_http_client_handle_t client = NULL;

        for (int attempt = 0; attempt < 2; attempt++) {
            bool insecure = (attempt == 1);
            if (insecure && !is_liveatc) {
                break;   // 只有 LiveATC 允许退化为不校验
            }
            s_redirect_location_valid = false;
            s_redirect_location[0] = '\0';
            esp_http_client_config_t cfg = {
                .url = current,
                .timeout_ms = HTTP_TIMEOUT_MS,
                .buffer_size = 2048,
                .keep_alive_enable = false,
                .crt_bundle_attach = (!is_liveatc) ? esp_crt_bundle_attach : NULL,
                .cert_pem = (is_liveatc && !insecure) ? airband_extra_ca_pem : NULL,
                .skip_cert_common_name_check = insecure,
                .user_agent = "FlightMate-Pro/1.0",
                .event_handler = http_event_handler,
                .disable_auto_redirect = true,
            };
            client = esp_http_client_init(&cfg);
            if (!client) {
                ESP_LOGE(TAG, "HTTP 客户端创建失败");
                return NULL;
            }
            esp_err_t err = esp_http_client_open(client, 0);
            if (err == ESP_OK) {
                if (insecure) {
                    ESP_LOGW(TAG, "TLS 校验失败后已退化为不校验证书: %s", current);
                }
                break;
            }
            ESP_LOGW(TAG, "连接失败(%s)%s: %s", esp_err_to_name(err),
                     insecure ? "[已关闭证书校验]" : "", current);
            esp_http_client_cleanup(client);
            client = NULL;
            if (!is_liveatc) {
                return NULL;
            }
        }
        if (!client) {
            return NULL;
        }

        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status == 200) {
            if (hop > 0) {
                ESP_LOGI(TAG, "重定向后播放成功: %s", current);
            }
            return client;
        }
        if (status >= 300 && status < 400) {
            char next[REDIRECT_URL_MAX];
            if (s_redirect_location_valid &&
                airband_resolve_redirect(current, s_redirect_location, next, sizeof(next))) {
                prefer_plain_http_for_liveatc(next);
                ESP_LOGI(TAG, "HTTP %d 重定向 -> %s", status, next);
                strlcpy(current, next, sizeof(current));
                esp_http_client_cleanup(client);
                continue;
            }
            ESP_LOGW(TAG, "HTTP %d 重定向目标不可用: '%s'", status, s_redirect_location);
        } else {
            ESP_LOGW(TAG, "HTTP 状态 %d (%s)", status, current);
        }
        esp_http_client_cleanup(client);
        return NULL;
    }
    ESP_LOGW(TAG, "重定向次数超过 %d 次", MAX_REDIRECT_HOPS);
    return NULL;
}

// 探测缓冲区里第一帧 MP3 的采样率；返回 0 表示数据还不够。
// 用途：低采样率(<=11 kHz，多为 VBR)用 minimp3（兼容性好），高采样率(>=12 kHz)
// 用 Helix（实测解码速度足够实时，minimp3 在 C3 上只有 62% 实时）。
static int peek_mp3_rate(const unsigned char *buf, int len)
{
    static const int r_mpeg1[3] = {44100, 48000, 32000};
    static const int r_mpeg2[3] = {22050, 24000, 16000};
    static const int r_mpeg25[3] = {11025, 12000, 8000};
    for (int i = 0; i + 4 <= len; i++) {
        if (buf[i] != 0xFF || (buf[i + 1] & 0xE0) != 0xE0) {
            continue;
        }
        int ver = (buf[i + 1] >> 3) & 0x03;    // 3=MPEG1, 2=MPEG2, 0=MPEG2.5
        int layer = (buf[i + 1] >> 1) & 0x03;  // 1 = Layer III
        int sr = (buf[i + 2] >> 2) & 0x03;
        int br = (buf[i + 2] >> 4) & 0x0F;
        if (ver == 1 || layer != 1 || sr == 3 || br == 0 || br == 15) {
            continue;
        }
        if (ver == 3) return r_mpeg1[sr];
        if (ver == 2) return r_mpeg2[sr];
        return r_mpeg25[sr];
    }
    return 0;
}

// 统一输出：按流原生采样率打开 codec（只在采样率变化时），双声道下混为单声道后写出。
static void output_pcm(int16_t *buf, int samples, int channels, int rate,
                       int *last_rate, unsigned *frames_ok, const char *decoder_name)
{
    if (samples <= 0) {
        return;
    }
    if (rate > 0 && rate != *last_rate) {
        if (bsp_audio_set_format((uint32_t)rate, 16, 1) == ESP_OK) {
            bsp_audio_set_volume(s_volume);
            *last_rate = rate;
        } else {
            ESP_LOGE(TAG, "设置播放格式 %dHz/1ch 失败", rate);
        }
    }
    if (channels == 2) {
        for (int i = 0; i < samples; i++) {
            int32_t mix = (int32_t)buf[2 * i] + (int32_t)buf[2 * i + 1];
            buf[i] = (int16_t)(mix / 2);
        }
    }
    esp_err_t werr = bsp_audio_write(buf, (size_t)samples * sizeof(int16_t));
    if (*frames_ok == 0) {
        int peak = 0;
        for (int i = 0; i < samples; i++) {
            int v = buf[i];
            if (v < 0) v = -v;
            if (v > peak) peak = v;
        }
        ESP_LOGI(TAG, "首帧诊断[%s]: 峰值=%d 样本=%d 声道=%d 写入=%s 音量=%u",
                 decoder_name, peak, samples, channels, esp_err_to_name(werr), (unsigned)s_volume);
    } else if (werr != ESP_OK) {
        ESP_LOGW(TAG, "音频写入失败: %s", esp_err_to_name(werr));
    }
    (*frames_ok)++;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        s_status.frames++;
        xSemaphoreGive(s_lock);
    }
}

// 一次播放会话：连接、拉流、按采样率选解码器、播放。返回时把状态置为 IDLE。
static void play_session(mp3dec_t *mm_dec, HMP3Decoder helix_dec, unsigned char *in_buf, int16_t *out_buf)
{
    int open_failures = 0;

    while (s_active && !s_stop) {
        status_set(AIRBAND_AUDIO_CONNECTING, "正在连接语音流");
        esp_http_client_handle_t client = http_open(s_url);
        if (!client) {
            ESP_LOGW(TAG, "打开语音流失败(%d/%d), 空闲堆=%u", open_failures + 1,
                     MAX_OPEN_FAILURES, (unsigned)esp_get_free_heap_size());
            open_failures++;
            if (open_failures >= MAX_OPEN_FAILURES) {
                status_set(AIRBAND_AUDIO_ERROR, "无法连接语音流");
                ESP_LOGW(TAG, "连续 %d 次连接失败，停止重试", open_failures);
                break;
            }
            status_set(AIRBAND_AUDIO_RETRYING, "连接失败，稍后重试");
            for (int i = 0; i < RETRY_DELAY_MS / 100 && !s_stop; i++) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            continue;
        }
        open_failures = 0;
        status_set(AIRBAND_AUDIO_PLAYING, "正在收听");
        ESP_LOGI(TAG, "开始播放: %s", s_url);

        int dec_kind = 0;              // 0=未定, 1=minimp3, 2=Helix
        int last_rate = 0;
        unsigned frames_ok = 0;
        unsigned decode_errors = 0;
        int len = 0;
        bool switched = false;   // 每个会话最多自动切换一次解码器

        while (s_active && !s_stop) {
            if (len >= IN_BUF_SIZE) {
                len = 0;   // 塞满仍无法解码：丢弃，避免死循环
            }
            int got = esp_http_client_read(client, (char *)in_buf + len, IN_BUF_SIZE - len);
            if (got <= 0) {
                ESP_LOGW(TAG, "流读取结束/出错: %d", got);
                break;
            }
            len += got;

            if (dec_kind == 0) {
                int rate = peek_mp3_rate(in_buf, len);
                if (rate == 0) {
                    if (len >= 8192) {
                        ESP_LOGW(TAG, "读到 %d 字节仍无 MP3 帧，判断该频道当前无音频", len);
                        status_set(AIRBAND_AUDIO_ERROR, "该频道暂无信号，换一个试试");
                        s_stop = true;
                        break;
                    }
                    continue;   // 还看不出采样率，继续读
                }
                // <=11.025kHz（多为 MPEG-2.5）用 minimp3，12kHz 及以上用 Helix。
                dec_kind = (rate >= 12000) ? 2 : 1;
                if (dec_kind == 2 && !helix_dec) {
                    dec_kind = 1;   // Helix 没拿到就退回 minimp3
                }
                mp3dec_init(mm_dec);
                ESP_LOGI(TAG, "解码器选择: %s (流采样率 %d Hz)",
                         dec_kind == 2 ? "Helix" : "minimp3", rate);
            }

            int off = 0;
            if (dec_kind == 2) {
                // ---- Helix 路径（高采样率，实测可实时）----
                while (off < len && s_active && !s_stop) {
                    int sync = MP3FindSyncWord(in_buf + off, len - off);
                    if (sync < 0) {
                        off = len;
                        break;
                    }
                    off += sync;
                    unsigned char *rp = in_buf + off;
                    int left = len - off;
                    int err = MP3Decode(helix_dec, &rp, &left, out_buf, 0);
                    if (err == 0) {
                        MP3FrameInfo info;
                        MP3GetLastFrameInfo(helix_dec, &info);
                        output_pcm(out_buf, info.outputSamps / (info.nChans ? info.nChans : 1),
                                   info.nChans, info.samprate, &last_rate, &frames_ok, "Helix");
                        off = (int)(rp - in_buf);
                    } else if (err == ERR_MP3_INDATA_UNDERFLOW) {
                        break;
                    } else if (err == ERR_MP3_MAINDATA_UNDERFLOW) {
                        int next = (int)(rp - in_buf);
                        off = (next > off) ? next : off + 1;
                        decode_errors++;
                    } else {
                        decode_errors++;
                        if (decode_errors <= 5) {
                            ESP_LOGW(TAG, "Helix 解码错误 %u (err=%d)", decode_errors, err);
                        }
                        off++;
                    }
                }
            } else {
                // ---- minimp3 路径（低采样率 VBR，Helix 解不出的那些流）----
                while (off < len && s_active && !s_stop) {
                    mp3dec_frame_info_t info;
                    int samples = mp3dec_decode_frame(mm_dec, in_buf + off, len - off, out_buf, &info);
                    if (info.frame_bytes > 0) {
                        off += info.frame_bytes;
                        if (samples > 0) {
                            output_pcm(out_buf, samples, info.channels, info.hz,
                                       &last_rate, &frames_ok, "minimp3");
                        } else {
                            decode_errors++;
                        }
                    } else if ((len - off) < (int)(sizeof(int16_t) * MINIMP3_MAX_SAMPLES_PER_FRAME)) {
                        break;
                    } else {
                        decode_errors++;
                        off++;
                    }
                }
            }

            // 兜底：所选解码器在首段数据里完全解不出帧时，自动换另一个解码器。
            // 这样新频道/自定义流不需要人工指定解码器。
            if (!switched && frames_ok == 0 && decode_errors >= 200) {
                switched = true;
                dec_kind = (dec_kind == 2) ? 1 : 2;
                if (dec_kind == 2 && !helix_dec) {
                    dec_kind = 1;
                }
                if (dec_kind == 1) {
                    mp3dec_init(mm_dec);
                }
                decode_errors = 0;
                off = len;   // 丢弃已缓冲数据，让新解码器重新同步
                ESP_LOGW(TAG, "所选解码器无有效帧，自动切换为 %s",
                         dec_kind == 2 ? "Helix" : "minimp3");
            }

            if (off > 0) {
                memmove(in_buf, in_buf + off, (size_t)(len - off));
                len -= off;
            }
        }

        ESP_LOGI(TAG, "本次会话: 解码成功 %u 帧, 跳过/错误 %u 次, 采样率 %dHz",
                 frames_ok, decode_errors, last_rate);
        esp_http_client_cleanup(client);
        if (s_active && !s_stop) {
            status_set(AIRBAND_AUDIO_RETRYING, "语音流中断，正在重连");
            for (int i = 0; i < RETRY_DELAY_MS / 100 && s_active && !s_stop; i++) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
    }
    status_set(AIRBAND_AUDIO_IDLE, "");
}

static void audio_task(void *arg)
{
    (void)arg;
    /* 解码缓冲与 minimp3 解码器全部静态分配：运行期堆被 LVGL/网络/照片切得很碎，
       动态申请这些大块必然失败（旧版就是在这里挂掉并连带把设备搞重启）。 */
    static unsigned char s_in_buf[IN_BUF_SIZE];
    static int16_t s_out_buf[OUT_SAMPLES_MAX];
    /* 低采样率（MPEG-2.5 8–12kHz）Helix 实测解不了，必须用 minimp3；22kHz 用 Helix 才够实时。 */
    static mp3dec_t s_decoder;
    mp3dec_t *decoder = &s_decoder;
    mp3dec_init(decoder);
    unsigned char *in_buf = s_in_buf;
    int16_t *out_buf = s_out_buf;
    /* Helix 不在这里常驻：它要 19KB 连续内存，常驻会把气象/雷达的 HTTPS 挤掉。
       放到首次播放时按需分配，分配不到就退回 minimp3（低采样率流本来就够用）。 */
    HMP3Decoder helix = NULL;
    s_task_ready = true;
    ESP_LOGI(TAG, "音频任务就绪（空闲堆=%u）", (unsigned)esp_get_free_heap_size());

    for (;;) {
        /* 空闲 20 秒就结束任务、把 24KB 栈还给堆：气象/雷达的 HTTPS 需要几十 KB
           连续内存，音频这块不还回去它们就永远连不上。下次点播时再重建。 */
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000)) == 0) {
            if (s_active) {
                continue;
            }
            if (helix) {
                MP3FreeDecoder(helix);
                helix = NULL;
            }
            ESP_LOGI(TAG, "音频空闲，任务退出并归还 24KB 栈（空闲堆=%u）",
                     (unsigned)esp_get_free_heap_size());
            if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
            s_task = NULL;
            s_task_ready = false;
            if (s_lock) xSemaphoreGive(s_lock);
            vTaskDelete(NULL);
            return;
        }
        if (!s_active) {
            if (s_helix_release && helix) {
                MP3FreeDecoder(helix);
                helix = NULL;
                ESP_LOGI(TAG, "已按请求归还 Helix（空闲堆=%u）",
                         (unsigned)esp_get_free_heap_size());
            }
            s_helix_release = false;
            continue;
        }
        const uint32_t free_heap = (uint32_t)esp_get_free_heap_size();
        if (!helix && free_heap >= HELIX_MIN_FREE_HEAP) {
            helix = MP3InitDecoder();
            if (helix) {
                ESP_LOGI(TAG, "Helix 解码器就绪（空闲堆=%u）", (unsigned)esp_get_free_heap_size());
            } else {
                ESP_LOGW(TAG, "Helix 分配失败(空闲堆=%u)，本次退回 minimp3", (unsigned)free_heap);
            }
        } else if (!helix) {
            ESP_LOGI(TAG, "空闲堆 %u 不足 %d，本次用 minimp3（低采样率流足够实时）",
                     (unsigned)free_heap, HELIX_MIN_FREE_HEAP);
        }
        status_set(AIRBAND_AUDIO_CONNECTING, "准备连接");
        play_session(decoder, helix, in_buf, out_buf);
        s_active = false;
        ESP_LOGI(TAG, "音频任务栈余量: %u 字节",
                 (unsigned)uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t));
    }
}

esp_err_t airband_audio_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_audio_ready) {
        esp_err_t err = bsp_audio_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "音频初始化失败: %s", esp_err_to_name(err));
            return err;
        }
        s_audio_ready = true;
    }
    bsp_audio_set_volume(s_volume);
    if (!s_task) {
        s_task_ready = false;
        /* 24KB 连续栈只在堆很干净时拿得到：main 在开机早期先建一次，
           空闲超时后任务会自毁把栈还给堆，下次点播再重建。 */
        if (xTaskCreate(audio_task, "airband_audio", AUDIO_TASK_STACK, NULL,
                        AUDIO_TASK_PRIORITY, &s_task) != pdPASS) {
            s_task = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    /* 一次性分配在任务里完成，这里等它就绪，避免调用方拿到“半成品”句柄。 */
    for (int i = 0; i < 60 && s_task && !s_task_ready; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return (s_task && s_task_ready) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t airband_audio_play(const char *url)
{
    if (!url || !airband_url_is_valid(url)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_task || !s_task_ready) {
        esp_err_t err = airband_audio_init();
        if (err != ESP_OK) {
            return err;
        }
    }
    /* 任务若在一次性分配阶段失败会自毁并置 s_task=NULL，此时绝不能去通知它。 */
    if (!s_task || !s_task_ready) {
        return ESP_ERR_NO_MEM;
    }
    airband_audio_stop();

    strlcpy(s_url, url, sizeof(s_url));
    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        s_status.frames = 0;
        s_status.stream_name[0] = '\0';
        s_status.detail[0] = '\0';
        xSemaphoreGive(s_lock);
    }
    s_stop = false;
    s_helix_release = false;
    s_active = true;
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
    return ESP_OK;
}

/* 立刻归还 Helix 解码器（约 19KB）：离开电台去联网前调用，
   否则航迹雷达/机场气象的 TLS 会因连续内存不足而连不上。
   正在收听时不打断播放。 */
void airband_audio_release_decoder(void)
{
    if (s_active) {
        return;
    }
    s_helix_release = true;
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

void airband_audio_stop(void)
{
    if (!s_task || !s_active) {
        return;
    }
    s_stop = true;
    // 有界等待任务回到空闲；http read 超时会保证最多几秒。
    for (int i = 0; i < STOP_WAIT_STEPS && s_active; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    s_stop = false;
    if (s_active) {
        ESP_LOGW(TAG, "播放任务未在超时内停止");
    }
}

bool airband_audio_is_active(void)
{
    return s_active;
}

void airband_audio_release(void)
{
    airband_audio_stop();
    if (s_audio_ready) {
        // 释放 codec 对象，保留 I2S channel 与共享 I2C 总线。
        bsp_audio_sleep();
        s_audio_ready = false;
    }
}

esp_err_t airband_audio_reacquire(void)
{
    if (s_audio_ready) {
        return ESP_OK;
    }
    esp_err_t err = bsp_audio_init();
    if (err == ESP_OK) {
        s_audio_ready = true;
        bsp_audio_set_volume(s_volume);
    }
    return err;
}
