#pragma once

#include <stdbool.h>
#include <stdint.h>

#define AUDIO_SAMPLE_RATE_HZ  16000  /* 16kHz：语音识别标准采样率 */

/**
 * 在 STATE_LISTENING 时调用：启动 I2S，连续录 PCM 到内部缓冲区。
 * 录到缓冲区满或收到 audio_stop_listening() 后置位 record_done。
 */
void audio_start_listening(void);

/** 请求停止录音（由 UI 再次点击触发）；record_task 会在当前块读完后退出。 */
void audio_stop_listening(void);

/** 等待本次录音结束，最多 timeout_ms 毫秒。返回是否在超时内收到 record_done。 */
bool audio_wait_record_done(uint32_t timeout_ms);

/**
 * 在 STATE_SPEAKING 时调用：等待 record_done 后，用 I2S 扬声器播放刚才录制的 PCM。
 * 在独立任务中执行，不阻塞状态机。
 */
void audio_play_recorded(void);

/**
 * 播放指定 PCM 缓冲区（int16 单声道），使用给定采样率。
 * 在独立任务中执行；pcm 指针在播放完成前必须有效。
 */
void audio_play_pcm(const int16_t *pcm, uint32_t samples, uint32_t sample_rate_hz);

/**
 * 获取最近一次录音的 PCM 缓冲区与采样数（只读，下次 LISTENING 前有效）。
 * 返回采样率固定为 AUDIO_SAMPLE_RATE_HZ，单声道 16bit。
 * 若尚未录过或长度为 0，*out_samples 为 0，*out_pcm 可为 NULL。
 */
void audio_get_recorded_pcm(const int16_t **out_pcm, uint32_t *out_samples);
