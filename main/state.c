#include "state.h"
#include "ui.h"
#include "audio.h"
#include "backend.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "STATE";
static device_state_t current_state = STATE_IDLE;

#define LAST_USER_TEXT_MAX 192
#define LAST_REPLY_TEXT_MAX 192
static char last_user_text[LAST_USER_TEXT_MAX];
static char last_reply_text[LAST_REPLY_TEXT_MAX];

/** THINKING 界面至少显示多久（≥300ms，避免一闪而过） */
#define THINKING_MIN_DISPLAY_MS  300
/** 上传后端最长等待：10s 超时，由 backend.c UPLOAD_TIMEOUT_MS 保证 */

static void thinking_task(void *arg)
{
    (void)arg;
    const int16_t *pcm = NULL;
    uint32_t samples = 0;
    audio_get_recorded_pcm(&pcm, &samples);
    if (pcm == NULL || samples == 0) {
        ESP_LOGW(TAG, "THINKING: no pcm, skip upload");
        set_state(STATE_IDLE);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "THINKING: upload %lu samples (min_display=%dms, timeout=10s)", (unsigned long)samples, THINKING_MIN_DISPLAY_MS);

    int64_t start_us = esp_timer_get_time();
    bool ok = backend_send_pcm(pcm, samples, AUDIO_SAMPLE_RATE_HZ);
    int64_t elapsed_us = esp_timer_get_time() - start_us;
    int64_t min_display_us = (int64_t)THINKING_MIN_DISPLAY_MS * 1000;
    if (elapsed_us < min_display_us) {
        uint32_t remain_ms = (uint32_t)((min_display_us - elapsed_us) / 1000);
        vTaskDelay(pdMS_TO_TICKS(remain_ms));
    }

    if (ok) {
        backend_get_reply_text(last_user_text, sizeof(last_user_text));
        backend_get_reply_reply_text(last_reply_text, sizeof(last_reply_text));
        if (last_user_text[0] != '\0') {
            ESP_LOGI(TAG, "user said: %s", last_user_text);
        }
        ESP_LOGI(TAG, "THINKING: backend ok (click to SPEAKING)");
        /* 不自动切 SPEAKING，等用户点击 */
    } else {
        last_user_text[0] = '\0';
        last_reply_text[0] = '\0';
        ESP_LOGW(TAG, "THINKING: backend failed/timeout -> IDLE");
        set_state(STATE_IDLE);
    }
    vTaskDelete(NULL);
}

void state_init(void)
{
    current_state = STATE_IDLE;
    ESP_LOGI(TAG, "initial state = IDLE");
}

void set_state(device_state_t new_state)
{
    if (current_state == new_state) return;

    current_state = new_state;
    ESP_LOGI(TAG, "state changed to %d", current_state);
    ui_update(new_state);

    if (new_state == STATE_LISTENING) {
        audio_start_listening();
    }
    if (new_state == STATE_THINKING) {
        (void)xTaskCreate(thinking_task, "thinking", 4096, NULL, 4, NULL);
    }
    if (new_state == STATE_SPEAKING) {
        /* 后端已不返回音频，仅在 UI 显示 reply_text；若有音频则播放 */
        const int16_t *reply_pcm = NULL;
        uint32_t reply_samples = 0;
        uint32_t reply_rate = 0;
        backend_get_reply_audio(&reply_pcm, &reply_samples, &reply_rate);
        if (reply_pcm != NULL && reply_samples > 0 && reply_rate > 0) {
            audio_play_pcm(reply_pcm, reply_samples, reply_rate);
        }
        /* 无后端音频时不播放，UI 显示 last_reply_text */
    }
}

device_state_t get_state(void)
{
    return current_state;
}

const char *state_get_last_user_text(void)
{
    return last_user_text;
}

const char *state_get_last_reply_text(void)
{
    return last_reply_text;
}
