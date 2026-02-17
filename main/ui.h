#pragma once
#include "state.h"
#include "driver/i2c_master.h"

/** 初始化显示硬件与 LVGL，必须在 ui_init 之前调用 */
void display_init(void);
void ui_init(void);
void ui_update(device_state_t state);
i2c_master_bus_handle_t ui_get_i2c_bus(void);