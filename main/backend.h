#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * 向后端 POST 一段假数据，接收响应并在 log 中打印一行。
 * 需先连上 Wi-Fi。阻塞执行。
 * 成功返回 true，失败返回 false。
 */
bool backend_send_fake_data(void);

/**
 * 向后端 POST /upload 发送 raw PCM（int16 单声道）。
 * 后端返回 body = 一行 JSON + "\\n" + raw PCM；成功则解析并保存 PCM 供播放。
 * 需先连上 Wi-Fi。阻塞执行。成功返回 true，失败返回 false。
 */
bool backend_send_pcm(const int16_t *pcm, uint32_t samples, uint32_t sample_rate_hz);

/**
 * 获取最近一次 /upload 成功返回的音频（指向 backend 内部缓冲，下次 upload 前有效）。
 * 若没有则 *out_samples 为 0，*out_sample_rate_hz 可为 0。
 */
void backend_get_reply_audio(const int16_t **out_pcm, uint32_t *out_samples, uint32_t *out_sample_rate_hz);

/**
 * 获取最近一次 /upload 成功返回的 STT 文本（user_text）。
 * 拷贝到 buf，最多 buf_size-1 字符并加 \\0 结尾；buf_size 可为 0。
 */
void backend_get_reply_text(char *buf, size_t buf_size);

/**
 * 获取最近一次 /upload 成功返回的 LLM 回复文本（reply_text），供 UI 显示。
 */
void backend_get_reply_reply_text(char *buf, size_t buf_size);
