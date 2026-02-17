#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "state.h"
#include "ui.h"
#include "wifi.h"
#include "backend.h"

static void startup_task(void *arg)
{
    (void)arg;
    if (wifi_wait_connected(15000)) {
        if (backend_send_fake_data()) {
            ESP_LOGI("DESK_AI", "backend send ok");
        } else {
            ESP_LOGW("DESK_AI", "backend send failed");
        }
    } else {
        ESP_LOGW("DESK_AI", "wifi connect timeout");
    }
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI("DESK_AI", "Device booted");

    display_init();
    state_init();
    ui_init();

    wifi_init();
    xTaskCreate(startup_task, "startup", 4096, NULL, 3, NULL);

    /* 状态仅由触屏点击切换；THINKING 内由 backend task 负责 POST → 等 response → set_state(SPEAKING) */
}
