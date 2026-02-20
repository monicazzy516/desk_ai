#pragma once

typedef enum {
    STATE_IDLE = 0,
    STATE_LISTENING,
    STATE_RECORDED,   /* [已废弃] 保留以兼容旧代码，现在 LISTENING 直接进入 THINKING */
    STATE_THINKING,
    STATE_SPEAKING
} device_state_t;

void state_init(void);
void set_state(device_state_t new_state);
device_state_t get_state(void);

/** 最近一次 STT 识别结果（user_text），无则为空串。 */
const char *state_get_last_user_text(void);

/** 最近一次 LLM 回复（reply_text），供 UI 显示；无则为空串。 */
const char *state_get_last_reply_text(void);
