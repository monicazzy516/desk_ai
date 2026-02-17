#include "audio.h"
#include <stdbool.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"

static const char *TAG = "AUDIO";

/* 板载 MIC：MIC_SCK=15, MIC_WS=2, MIC_SD=39 */
#define I2S_MIC_BCK_IO    GPIO_NUM_15
#define I2S_MIC_WS_IO     GPIO_NUM_2
#define I2S_MIC_DATA_IO   GPIO_NUM_39
/* PCM5101 扬声器：Speak_BCK=48, Speak_LRCK=38, Speak_DIN=47 */
#define I2S_SPK_BCK_IO    GPIO_NUM_48
#define I2S_SPK_WS_IO     GPIO_NUM_38
#define I2S_SPK_DATA_IO   GPIO_NUM_47

#define SAMPLE_RATE_HZ      48000
/* 8MB PSRAM：缓冲区放 PSRAM，最长约 10s；仅由用户点击停止或录满结束 */
#define MAX_RECORD_SAMPLES   (480000u)  /* 10s @ 48kHz = 960KB */
#define MIN_RECORD_SAMPLES   (9600u)    /* 至少 0.2s 再响应停止，避免误触 0 samples */
#define RECORD_BUF_BYTES     (MAX_RECORD_SAMPLES * sizeof(int16_t))
#define CHUNK_SAMPLES        1024
#define CHUNK_BYTES          (CHUNK_SAMPLES * sizeof(int16_t))

#define RECORD_DONE_BIT      (1u << 0)
#define RECORD_WAIT_MS       4000

static int16_t *s_record_buf;           /* PSRAM，audio_start_listening 时分配 */
static volatile uint32_t s_recorded_samples;
static volatile bool s_stop_requested;
static EventGroupHandle_t s_ev;

static void record_task(void *arg)
{
    (void)arg;
    if (s_record_buf == NULL) {
        ESP_LOGE(TAG, "record_buf NULL, skip");
        xEventGroupSetBits(s_ev, RECORD_DONE_BIT);
        vTaskDelete(NULL);
        return;
    }
    i2s_chan_handle_t rx_handle = NULL;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_frame_num = 240;
    chan_cfg.auto_clear_after_cb = true;

    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed %s", esp_err_to_name(ret));
        xEventGroupSetBits(s_ev, RECORD_DONE_BIT);
        vTaskDelete(NULL);
        return;
    }

    /* 单声道麦克风在 RIGHT 声道：明确指定 slot_mask 只读 RIGHT */
    i2s_std_slot_config_t slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
    slot_cfg.slot_mask = I2S_STD_SLOT_RIGHT;  /* 麦克风数据在 WS=HIGH 时输出 */
    
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE_HZ),
        .slot_cfg = slot_cfg,
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_MIC_BCK_IO,
            .ws = I2S_MIC_WS_IO,
            .dout = I2S_GPIO_UNUSED,
            .din = I2S_MIC_DATA_IO,
            .invert_flags = { 0 },
        },
    };

    ret = i2s_channel_init_std_mode(rx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed %s", esp_err_to_name(ret));
        i2s_del_channel(rx_handle);
        xEventGroupSetBits(s_ev, RECORD_DONE_BIT);
        vTaskDelete(NULL);
        return;
    }

    ret = i2s_channel_enable(rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed %s", esp_err_to_name(ret));
        i2s_del_channel(rx_handle);
        xEventGroupSetBits(s_ev, RECORD_DONE_BIT);
        vTaskDelete(NULL);
        return;
    }

    /* MONO 模式 + RIGHT slot：驱动只返回 RIGHT 声道数据（单声道流） */
    uint32_t total_samples = 0;
    
    while ((!s_stop_requested || total_samples < MIN_RECORD_SAMPLES) && total_samples < MAX_RECORD_SAMPLES) {
        size_t to_read = CHUNK_BYTES;
        if (total_samples + CHUNK_SAMPLES > MAX_RECORD_SAMPLES) {
            to_read = (MAX_RECORD_SAMPLES - total_samples) * sizeof(int16_t);
        }
        
        size_t bytes_read = 0;
        ret = i2s_channel_read(rx_handle, &s_record_buf[total_samples], to_read, &bytes_read, pdMS_TO_TICKS(100));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "i2s_channel_read failed: %s (total_samples=%lu). Check MIC pins: BCK=%d WS=%d DIN=%d",
                     esp_err_to_name(ret), (unsigned long)total_samples, I2S_MIC_BCK_IO, I2S_MIC_WS_IO, I2S_MIC_DATA_IO);
            break;
        }
        total_samples += (uint32_t)(bytes_read / sizeof(int16_t));
    }

    i2s_channel_disable(rx_handle);
    i2s_del_channel(rx_handle);

    s_recorded_samples = total_samples;
    
    /* 调试：统计录音数据质量 */
    if (total_samples > 0) {
        int16_t min_val = 32767, max_val = -32768;
        uint32_t zero_count = 0;
        uint64_t sum_abs = 0;
        for (uint32_t i = 0; i < total_samples; i++) {
            int16_t val = s_record_buf[i];
            if (val == 0) zero_count++;
            if (val < min_val) min_val = val;
            if (val > max_val) max_val = val;
            sum_abs += (val >= 0) ? val : -val;
        }
        uint32_t avg_abs = (uint32_t)(sum_abs / total_samples);
        ESP_LOGI(TAG, "recorded %lu samples | min=%d max=%d avg_abs=%lu zeros=%lu (%.1f%%)",
                 (unsigned long)total_samples, min_val, max_val, (unsigned long)avg_abs,
                 (unsigned long)zero_count, 100.0f * zero_count / total_samples);
        if (avg_abs < 50) {
            ESP_LOGW(TAG, "audio signal very weak (avg_abs=%lu), check MIC connection/gain or try PDM mode", (unsigned long)avg_abs);
        }
    } else {
        ESP_LOGI(TAG, "recorded 0 samples");
    }
    
    xEventGroupSetBits(s_ev, RECORD_DONE_BIT);
    vTaskDelete(NULL);
}

static void play_task(void *arg)
{
    (void)arg;
    EventBits_t u = xEventGroupWaitBits(s_ev, RECORD_DONE_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(RECORD_WAIT_MS));
    if (!(u & RECORD_DONE_BIT)) {
        ESP_LOGW(TAG, "play: no record done, skip");
        vTaskDelete(NULL);
        return;
    }
    uint32_t n = s_recorded_samples;
    if (n == 0) {
        ESP_LOGW(TAG, "play: 0 samples, skip");
        vTaskDelete(NULL);
        return;
    }

    i2s_chan_handle_t tx_handle = NULL;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_frame_num = 240;
    chan_cfg.auto_clear_after_cb = true;

    esp_err_t ret = i2s_new_channel(&chan_cfg, &tx_handle, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel tx failed %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_SPK_BCK_IO,
            .ws = I2S_SPK_WS_IO,
            .dout = I2S_SPK_DATA_IO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = { 0 },
        },
    };

    ret = i2s_channel_init_std_mode(tx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode tx failed %s", esp_err_to_name(ret));
        i2s_del_channel(tx_handle);
        vTaskDelete(NULL);
        return;
    }

    ret = i2s_channel_enable(tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable tx failed %s", esp_err_to_name(ret));
        i2s_del_channel(tx_handle);
        vTaskDelete(NULL);
        return;
    }

    size_t total_written = 0;
    const size_t to_write = (size_t)n * sizeof(int16_t);
    const size_t chunk = 1024 * sizeof(int16_t);
    while (total_written < to_write) {
        size_t w = to_write - total_written;
        if (w > chunk) {
            w = chunk;
        }
        size_t written = 0;
        ret = i2s_channel_write(tx_handle, (const char *)s_record_buf + total_written, w, &written, pdMS_TO_TICKS(1000));
        if (ret != ESP_OK) {
            break;
        }
        total_written += written;
    }

    i2s_channel_disable(tx_handle);
    i2s_del_channel(tx_handle);
    ESP_LOGI(TAG, "played %lu samples", (unsigned long)n);
    vTaskDelete(NULL);
}

/* 用于 audio_play_pcm：任务启动时从该结构读取参数 */
static struct {
    const int16_t *pcm;
    uint32_t samples;
    uint32_t sample_rate_hz;
} s_play_pcm_arg;

static void play_pcm_task(void *arg)
{
    (void)arg;
    const int16_t *pcm = s_play_pcm_arg.pcm;
    uint32_t n = s_play_pcm_arg.samples;
    uint32_t rate = s_play_pcm_arg.sample_rate_hz;
    if (pcm == NULL || n == 0 || rate == 0) {
        ESP_LOGW(TAG, "play_pcm: invalid arg");
        vTaskDelete(NULL);
        return;
    }

    i2s_chan_handle_t tx_handle = NULL;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_frame_num = 240;
    chan_cfg.auto_clear_after_cb = true;

    esp_err_t ret = i2s_new_channel(&chan_cfg, &tx_handle, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "play_pcm: i2s_new_channel failed %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }

    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    i2s_std_slot_config_t slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
    i2s_std_config_t std_cfg = {
        .clk_cfg = clk_cfg,
        .slot_cfg = slot_cfg,
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_SPK_BCK_IO,
            .ws = I2S_SPK_WS_IO,
            .dout = I2S_SPK_DATA_IO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = { 0 },
        },
    };

    ret = i2s_channel_init_std_mode(tx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "play_pcm: i2s_channel_init_std_mode failed %s", esp_err_to_name(ret));
        i2s_del_channel(tx_handle);
        vTaskDelete(NULL);
        return;
    }

    ret = i2s_channel_enable(tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "play_pcm: i2s_channel_enable failed %s", esp_err_to_name(ret));
        i2s_del_channel(tx_handle);
        vTaskDelete(NULL);
        return;
    }

    size_t total_written = 0;
    const size_t to_write = (size_t)n * sizeof(int16_t);
    const size_t chunk = 1024 * sizeof(int16_t);
    while (total_written < to_write) {
        size_t w = to_write - total_written;
        if (w > chunk) {
            w = chunk;
        }
        size_t written = 0;
        ret = i2s_channel_write(tx_handle, (const char *)pcm + total_written, w, &written, pdMS_TO_TICKS(1000));
        if (ret != ESP_OK) {
            break;
        }
        total_written += written;
    }

    i2s_channel_disable(tx_handle);
    i2s_del_channel(tx_handle);
    ESP_LOGI(TAG, "play_pcm: played %lu samples @ %lu Hz", (unsigned long)n, (unsigned long)rate);
    vTaskDelete(NULL);
}

void audio_play_pcm(const int16_t *pcm, uint32_t samples, uint32_t sample_rate_hz)
{
    if (pcm == NULL || samples == 0 || sample_rate_hz == 0) {
        ESP_LOGW(TAG, "play_pcm: skip invalid");
        return;
    }
    s_play_pcm_arg.pcm = pcm;
    s_play_pcm_arg.samples = samples;
    s_play_pcm_arg.sample_rate_hz = sample_rate_hz;
    BaseType_t ok = xTaskCreate(play_pcm_task, "play_pcm", 4096, NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate play_pcm failed");
    }
}

void audio_stop_listening(void)
{
    s_stop_requested = true;
}

bool audio_wait_record_done(uint32_t timeout_ms)
{
    if (s_ev == NULL) {
        return false;
    }
    EventBits_t u = xEventGroupWaitBits(s_ev, RECORD_DONE_BIT, pdTRUE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    return (u & RECORD_DONE_BIT) != 0;
}

void audio_start_listening(void)
{
    s_stop_requested = false;
    if (s_record_buf == NULL) {
        s_record_buf = (int16_t *)heap_caps_malloc(RECORD_BUF_BYTES, MALLOC_CAP_SPIRAM);
        if (s_record_buf == NULL) {
            ESP_LOGE(TAG, "record_buf alloc PSRAM failed (%u bytes), try internal", (unsigned)RECORD_BUF_BYTES);
            s_record_buf = (int16_t *)heap_caps_malloc(RECORD_BUF_BYTES, MALLOC_CAP_INTERNAL);
        }
        if (s_record_buf == NULL) {
            ESP_LOGE(TAG, "record_buf alloc failed");
            return;
        }
        ESP_LOGI(TAG, "record_buf %u bytes in %s", (unsigned)RECORD_BUF_BYTES,
                 heap_caps_get_free_size(MALLOC_CAP_SPIRAM) ? "PSRAM" : "internal");
    }
    if (s_ev == NULL) {
        s_ev = xEventGroupCreate();
        if (s_ev == NULL) {
            ESP_LOGE(TAG, "event group create failed");
            return;
        }
    }
    xEventGroupClearBits(s_ev, RECORD_DONE_BIT);
    s_recorded_samples = 0;

    BaseType_t ok = xTaskCreate(record_task, "record", 4096, NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate record failed");
    }
}

void audio_play_recorded(void)
{
    if (s_ev == NULL) {
        ESP_LOGW(TAG, "play: not listening yet, skip");
        return;
    }
    BaseType_t ok = xTaskCreate(play_task, "play", 4096, NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate play failed");
    }
}

void audio_get_recorded_pcm(const int16_t **out_pcm, uint32_t *out_samples)
{
    if (out_pcm) {
        *out_pcm = s_record_buf;
    }
    if (out_samples) {
        *out_samples = s_recorded_samples;
    }
}
