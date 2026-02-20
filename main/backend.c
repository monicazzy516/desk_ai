#include "backend.h"
#include "wifi.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "BACKEND";

/* URL 来自 menuconfig: Desk AI -> Backend URL */
#ifndef CONFIG_BACKEND_URL
#define CONFIG_BACKEND_URL "http://192.168.4.1:5000/chat"
#endif

#define FAKE_BODY "{\"fake\":\"hello from esp32\"}"
#define RESP_BUF_SIZE 256
#define UPLOAD_URL_MAX 128
#define UPLOAD_BODY_BUF_SIZE  (256 * 1024)   /* 256KB: 支持 ~5s TTS 音频 (24kHz * 2 bytes * 5s = 240KB) + JSON */
/** 上传后端最长等待时间（ms），超时则 backend_send_pcm 返回 false */
#define UPLOAD_TIMEOUT_MS  60000  /* 长录音 + Whisper+LLM 较慢，60s */

static esp_err_t on_client_data(esp_http_client_event_t *evt)
{
    static char buf[RESP_BUF_SIZE];
    static size_t len;

    switch (evt->event_id) {
    case HTTP_EVENT_ON_CONNECTED:
        len = 0;
        break;
    case HTTP_EVENT_ON_DATA:
        if (evt->data_len > 0 && len < sizeof(buf) - 1) {
            size_t copy = evt->data_len;
            if (copy > sizeof(buf) - 1 - len) copy = sizeof(buf) - 1 - len;
            memcpy(buf + len, evt->data, copy);
            len += copy;
        }
        break;
    case HTTP_EVENT_ON_FINISH:
        buf[len] = '\0';
        if (len > 0) {
            ESP_LOGI(TAG, "backend response: %s", buf);
        }
        len = 0;
        break;
    default:
        break;
    }
    return ESP_OK;
}

bool backend_send_fake_data(void)
{
    if (!wifi_is_connected()) {
        ESP_LOGW(TAG, "wifi not connected, skip send");
        return false;
    }

    esp_http_client_config_t cfg = {
        .url = CONFIG_BACKEND_URL,
        .method = HTTP_METHOD_POST,
        .event_handler = on_client_data,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "http client init failed");
        return false;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, FAKE_BODY, (int)strlen(FAKE_BODY));

    esp_err_t err = esp_http_client_perform(client);
    bool ok = (err == ESP_OK);
    if (!ok) {
        ESP_LOGE(TAG, "http perform failed: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    return ok;
}

/* 用于 /upload 响应：body = 纯 JSON（ok, user_text, reply_text），无音频 */
#define REPLY_TEXT_MAX 192
static bool s_upload_response_ok;
static uint8_t *s_upload_body_buf = NULL;  /* 动态分配（PSRAM），首次使用时分配 */
static size_t s_upload_body_len;
static int16_t *s_reply_pcm;           /* 无音频时为 NULL */
static uint32_t s_reply_pcm_samples;
static uint32_t s_reply_sample_rate_hz;
static char s_reply_text[REPLY_TEXT_MAX];       /* user_text（STT）*/
static char s_reply_reply_text[REPLY_TEXT_MAX]; /* reply_text（LLM），供 UI 显示 */

/** 解析已收到的 response body（纯 JSON 或 JSON+\\n+PCM），设置 s_upload_response_ok 等 */
static void parse_upload_response_body(void)
{
    if (s_upload_body_len == 0 || s_upload_body_buf == NULL) {
        return;
    }
    uint8_t *p = s_upload_body_buf;
    size_t i;
    size_t search_max = s_upload_body_len;
    if (search_max > 4096) {
        search_max = 4096;
    }
    for (i = 0; i < search_max && p[i] != '\n' && p[i] != '\r'; i++) { }
    size_t json_end = i;
    size_t pcm_start = s_upload_body_len; /* 无 PCM 时 */
    if (i < search_max && (p[i] == '\n' || p[i] == '\r')) {
        pcm_start = i + 1;
        if (p[i] == '\r' && i + 1 < s_upload_body_len && p[i + 1] == '\n') {
            pcm_start = i + 2;
        }
        p[json_end] = '\0';
    } else {
        /* 纯 JSON，无换行：整段为 JSON */
        if (s_upload_body_len < UPLOAD_BODY_BUF_SIZE) {
            p[s_upload_body_len] = '\0';
        } else {
            p[UPLOAD_BODY_BUF_SIZE - 1] = '\0';
        }
    }
    s_upload_response_ok = (strstr((const char *)p, "ok") != NULL && strstr((const char *)p, "true") != NULL)
        && strstr((const char *)p, "reply_text") != NULL;
    if (!s_upload_response_ok) {
        ESP_LOGI(TAG, "upload response: %s", (const char *)p);
        return;
    }
    s_reply_pcm = (pcm_start < s_upload_body_len) ? (int16_t *)(p + pcm_start) : NULL;
    s_reply_pcm_samples = (pcm_start < s_upload_body_len) ? (uint32_t)((s_upload_body_len - pcm_start) / 2) : 0;
    s_reply_sample_rate_hz = 16000;
    if (s_reply_pcm != NULL) {
        const char *sr = strstr((const char *)p, "\"sample_rate\":");
        if (sr != NULL) {
            unsigned int v = 0;
            if (sscanf(sr + 14, "%u", &v) == 1) {
                s_reply_sample_rate_hz = (uint32_t)v;
            }
        }
    }
    const char *tv = strstr((const char *)p, "\"user_text\":\"");
    if (tv != NULL) {
        const char *q = tv + 14;
        size_t n = 0;
        while (n < REPLY_TEXT_MAX - 1 && *q != '\0') {
            if (*q == '\\' && *(q + 1) == '"') {
                s_reply_text[n++] = '"';
                q += 2;
                continue;
            }
            if (*q == '"') {
                break;
            }
            s_reply_text[n++] = *q;
            q++;
        }
        s_reply_text[n] = '\0';
    } else {
        tv = strstr((const char *)p, "\"text\":\"");
        if (tv != NULL) {
            const char *q = tv + 9;
            size_t n = 0;
            while (n < REPLY_TEXT_MAX - 1 && *q != '\0') {
                if (*q == '\\' && *(q + 1) == '"') {
                    s_reply_text[n++] = '"';
                    q += 2;
                    continue;
                }
                if (*q == '"') {
                    break;
                }
                s_reply_text[n++] = *q;
                q++;
            }
            s_reply_text[n] = '\0';
        }
    }
    const char *rv = strstr((const char *)p, "\"reply_text\":\"");
    if (rv != NULL) {
        const char *q = rv + 15;
        size_t n = 0;
        while (n < REPLY_TEXT_MAX - 1 && *q != '\0') {
            if (*q == '\\' && *(q + 1) == '"') {
                s_reply_reply_text[n++] = '"';
                q += 2;
                continue;
            }
            if (*q == '"') {
                break;
            }
            s_reply_reply_text[n++] = *q;
            q++;
        }
        s_reply_reply_text[n] = '\0';
        if (n > 0) {
            ESP_LOGI(TAG, "reply_text: %s", s_reply_reply_text);
        }
    }
    ESP_LOGI(TAG, "upload response ok, pcm_samples=%lu rate=%lu",
             (unsigned long)s_reply_pcm_samples, (unsigned long)s_reply_sample_rate_hz);
}

void backend_get_reply_audio(const int16_t **out_pcm, uint32_t *out_samples, uint32_t *out_sample_rate_hz)
{
    if (out_pcm) {
        *out_pcm = s_reply_pcm;
    }
    if (out_samples) {
        *out_samples = s_reply_pcm_samples;
    }
    if (out_sample_rate_hz) {
        *out_sample_rate_hz = s_reply_sample_rate_hz;
    }
}

void backend_get_reply_text(char *buf, size_t buf_size)
{
    if (buf == NULL || buf_size == 0) {
        return;
    }
    size_t n = 0;
    while (s_reply_text[n] != '\0' && n < buf_size - 1) {
        buf[n] = s_reply_text[n];
        n++;
    }
    buf[n] = '\0';
}

void backend_get_reply_reply_text(char *buf, size_t buf_size)
{
    if (buf == NULL || buf_size == 0) {
        return;
    }
    size_t n = 0;
    while (s_reply_reply_text[n] != '\0' && n < buf_size - 1) {
        buf[n] = s_reply_reply_text[n];
        n++;
    }
    buf[n] = '\0';
}

/*
 * 音频上传协议（与后端约定）：
 * - Body: raw PCM，little-endian int16，单声道。
 * - Header 明确描述格式，便于后端解析或将来扩展：
 *   X-Sample-Rate: 采样率（如 48000、16000）
 *   X-Channels: 声道数（1）
 *   X-Format: 采样格式，如 pcm16
 * 可选扩展：日后可改为 JSON body { "sample_rate", "channels", "format", "data": base64 }。
 */
bool backend_send_pcm(const int16_t *pcm, uint32_t samples, uint32_t sample_rate_hz)
{
    if (!wifi_is_connected()) {
        ESP_LOGW(TAG, "wifi not connected, skip upload");
        return false;
    }
    if (pcm == NULL || samples == 0) {
        ESP_LOGW(TAG, "no pcm data, skip upload");
        return false;
    }

    /* 首次使用时分配缓冲区（优先 PSRAM） */
    if (s_upload_body_buf == NULL) {
        s_upload_body_buf = (uint8_t *)heap_caps_malloc(UPLOAD_BODY_BUF_SIZE, MALLOC_CAP_SPIRAM);
        if (s_upload_body_buf == NULL) {
            ESP_LOGE(TAG, "upload_body_buf alloc PSRAM failed (%u bytes), try internal", (unsigned)UPLOAD_BODY_BUF_SIZE);
            s_upload_body_buf = (uint8_t *)heap_caps_malloc(UPLOAD_BODY_BUF_SIZE, MALLOC_CAP_INTERNAL);
        }
        if (s_upload_body_buf == NULL) {
            ESP_LOGE(TAG, "upload_body_buf alloc failed");
            return false;
        }
        ESP_LOGI(TAG, "upload_body_buf %u bytes allocated in %s", (unsigned)UPLOAD_BODY_BUF_SIZE,
                 heap_caps_get_free_size(MALLOC_CAP_SPIRAM) ? "PSRAM" : "internal");
    }

    char upload_url[UPLOAD_URL_MAX];
    size_t base_len = strlen(CONFIG_BACKEND_URL);
    if (base_len >= 5 && memcmp(CONFIG_BACKEND_URL + base_len - 5, "/chat", 5) == 0) {
        snprintf(upload_url, sizeof(upload_url), "%.*s/upload", (int)(base_len - 5), CONFIG_BACKEND_URL);
    } else {
        snprintf(upload_url, sizeof(upload_url), "%s/upload", CONFIG_BACKEND_URL);
    }

    size_t body_bytes = (size_t)samples * sizeof(int16_t);
    char rate_buf[12];
    snprintf(rate_buf, sizeof(rate_buf), "%lu", (unsigned long)sample_rate_hz);

    s_reply_pcm = NULL;
    s_reply_pcm_samples = 0;
    s_reply_sample_rate_hz = 0;
    s_reply_text[0] = '\0';
    s_reply_reply_text[0] = '\0';
    s_upload_response_ok = false;
    s_upload_body_len = 0;

    esp_http_client_config_t cfg = {
        .url = upload_url,
        .method = HTTP_METHOD_POST,
        .event_handler = NULL,
        .timeout_ms = UPLOAD_TIMEOUT_MS,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "http client init failed (upload)");
        return false;
    }

    esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
    esp_http_client_set_header(client, "X-Sample-Rate", rate_buf);
    esp_http_client_set_header(client, "X-Channels", "1");
    esp_http_client_set_header(client, "X-Format", "pcm16");
    esp_http_client_set_post_field(client, (const char *)pcm, (int)body_bytes);

    esp_err_t err = esp_http_client_open(client, (int)body_bytes);
    bool ok = (err == ESP_OK);
    if (ok) {
        int w = esp_http_client_write(client, (const char *)pcm, (int)body_bytes);
        if (w != (int)body_bytes) {
            ok = false;
        }
    }
    if (ok) {
        err = esp_http_client_fetch_headers(client);
        ok = (err == ESP_OK);
        if (!ok) {
            int status = esp_http_client_get_status_code(client);
            if (status == 200) {
                /* 已收到 200，可能是读后续头/首包时超时；仍尝试读 body */
                ESP_LOGW(TAG, "upload fetch_headers err=%d (%s) but status=200, try read body",
                         (int)err, esp_err_to_name(err));
                ok = true;
            } else {
                ESP_LOGE(TAG, "upload fetch_headers err=%d (%s), status=%d",
                         (int)err, esp_err_to_name(err), status);
            }
        }
    }
    if (ok) {
        esp_http_client_set_timeout_ms(client, UPLOAD_TIMEOUT_MS);
        int r;
        int retries = 0;
        while (s_upload_body_len < UPLOAD_BODY_BUF_SIZE) {
            size_t space = UPLOAD_BODY_BUF_SIZE - s_upload_body_len;
            r = esp_http_client_read(client, (char *)(s_upload_body_buf + s_upload_body_len), (int)space);
            if (r > 0) {
                s_upload_body_len += (size_t)r;
                retries = 0;
                continue;
            }
            if (r == 0 && s_upload_body_len > 0 && s_upload_body_len < 512 && retries < 5) {
                retries++;
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            break;
        }
        ESP_LOGI(TAG, "upload ON_FINISH body_len=%u", (unsigned)s_upload_body_len);
        parse_upload_response_body();
    }
    int http_status = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    ok = ok && s_upload_response_ok;
    if (!ok) {
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "upload failed: err=%d (%s) http_status=%d",
                     (int)err, esp_err_to_name(err), http_status);
        } else {
            ESP_LOGW(TAG, "upload response not ok (http_status=%d)", http_status);
        }
    }
    return ok;
}
