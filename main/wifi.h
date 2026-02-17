#pragma once

#include <stdbool.h>
#include <stdint.h>

/** 初始化 Wi-Fi，开始连接；不阻塞 */
void wifi_init(void);

/** 是否已拿到 IP（已连上） */
bool wifi_is_connected(void);

/** 阻塞直到连上或超时，超时返回 false */
bool wifi_wait_connected(uint32_t timeout_ms);
