#include "ui.h"
#include "state.h"
#include "audio.h"
#include "background_img.h"
#include "idle_img.h"
#include "smile_img.h"
#include "hand_img.h"
#include "heart_img.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st77916.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_lvgl_port.h"

static const char *TAG = "UI";

/* Waveshare ESP32-S3-LCD-1.85 引脚 (Wiki: Internal Hardware Connection) */
#define LCD_PCLK   40
#define LCD_DATA0  46
#define LCD_DATA1  45
#define LCD_DATA2  42
#define LCD_DATA3  41
#define LCD_CS     21
#define LCD_RST    -1   /* 板子为 EXIO2，暂不接复位 */
#define LCD_BL     5
#define LCD_H_RES  360
#define LCD_V_RES  360
#define LCD_HOST   SPI2_HOST

/* 触屏版 ESP32-S3-Touch-LCD-1.85：CST816 I2C */
#define TP_I2C_SDA  1
#define TP_I2C_SCL  3
#define TP_INT      4
#define TP_RST      -1  /* EXIO1，暂不接 */

static lv_display_t *disp_handle = NULL;
static lv_obj_t *screen = NULL;
static lv_obj_t *bg_img = NULL;        /* 背景图片对象 */
static lv_obj_t *idle_obj = NULL;      /* 柴犬图片对象（IDLE/THINKING 状态显示）*/
static lv_obj_t *smile_obj = NULL;     /* 笑脸柴犬图片对象（SPEAKING/LISTENING 状态显示）*/
static lv_obj_t *hand_obj = NULL;      /* 手指图标对象（IDLE 状态抚摸交互显示）*/
static lv_obj_t *heart_obj = NULL;     /* 爱心图标对象（IDLE 状态抚摸交互显示）*/
static lv_obj_t *state_label = NULL;   /* 当前 state 名称，便于 debug */
static lv_obj_t *reply_label = NULL;   /* SPEAKING 时显示后端 reply_text */
static lv_timer_t *petting_smile_timer = NULL;  /* 抚摸后笑脸持续定时器 */

static bool panel_io_cb(esp_lcd_panel_io_handle_t panel_io,
                        esp_lcd_panel_io_event_data_t *edata,
                        void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    (void)user_ctx;
    return false;
}

void display_init(void)
{
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_handle_t panel_handle = NULL;

    ESP_LOGI(TAG, "Init QSPI bus");
    const spi_bus_config_t bus_cfg = ST77916_PANEL_BUS_QSPI_CONFIG(
        LCD_PCLK,
        LCD_DATA0,
        LCD_DATA1,
        LCD_DATA2,
        LCD_DATA3,
        LCD_H_RES * 80 * sizeof(uint16_t));
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Install panel IO");
    const esp_lcd_panel_io_spi_config_t io_cfg =
        ST77916_PANEL_IO_QSPI_CONFIG(LCD_CS, panel_io_cb, NULL);
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,
                                              &io_cfg, &io_handle));

    ESP_LOGI(TAG, "Install ST77916 panel");
    st77916_vendor_config_t vendor_cfg = {
        .flags = { .use_qspi_interface = 1 },
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st77916(io_handle, &panel_cfg, &panel_handle));
    esp_lcd_panel_reset(panel_handle);
    esp_lcd_panel_init(panel_handle);
    esp_lcd_panel_disp_on_off(panel_handle, true);

    /* 背光 */
    gpio_config_t bl = {
        .pin_bit_mask = (1ULL << LCD_BL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&bl);
    gpio_set_level(LCD_BL, 1);

    ESP_LOGI(TAG, "Init LVGL port");
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    /* 缩小显存以适配内部 RAM；启用 PSRAM 后仍可改大或恢复双缓冲 */
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = panel_handle,
        .control_handle = NULL,
        .buffer_size = LCD_H_RES * 40,
        .double_buffer = false,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = 1,
            .swap_bytes = 1,  /* 交换字节序以修复颜色显示 */
        },
    };
    disp_handle = lvgl_port_add_disp(&disp_cfg);

    /* 触屏：CST816 I2C → LVGL input */
    i2c_master_bus_handle_t tp_i2c = NULL;
    i2c_master_bus_config_t i2c_bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = TP_I2C_SDA,
        .scl_io_num = TP_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
    };
    if (i2c_new_master_bus(&i2c_bus_cfg, &tp_i2c) == ESP_OK) {
        esp_lcd_panel_io_handle_t tp_io = NULL;
        esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
        tp_io_cfg.scl_speed_hz = 400000;
        if (esp_lcd_new_panel_io_i2c(tp_i2c, &tp_io_cfg, &tp_io) == ESP_OK) {
            esp_lcd_touch_handle_t tp_handle = NULL;
            esp_lcd_touch_config_t tp_cfg = {
                .x_max = LCD_H_RES,
                .y_max = LCD_V_RES,
                .rst_gpio_num = TP_RST,
                .int_gpio_num = TP_INT,
                .levels = { .reset = 0, .interrupt = 0 },
                .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
            };
            if (esp_lcd_touch_new_i2c_cst816s(tp_io, &tp_cfg, &tp_handle) == ESP_OK) {
                const lvgl_port_touch_cfg_t touch_cfg = {
                    .disp = disp_handle,
                    .handle = tp_handle,
                };
                lvgl_port_add_touch(&touch_cfg);
                ESP_LOGI(TAG, "Touch CST816 added");
            }
        }
    } else {
        ESP_LOGW(TAG, "Touch I2C init skip (no touch?)");
    }

    ESP_LOGI(TAG, "Display init done");
}

/** 双击检测：记录上次点击时间（微秒） */
static int64_t last_click_time_us = 0;
#define DOUBLE_CLICK_THRESHOLD_MS  500  /* 500ms 内连续点击视为双击 */

/** 抚摸后笑脸持续定时器回调：1秒后切换回 idle */
static void petting_smile_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    device_state_t cur = get_state();
    
    /* 只在 IDLE 状态下切换回 idle 表情 */
    if (cur == STATE_IDLE) {
        if (idle_obj != NULL) {
            lv_obj_clear_flag(idle_obj, LV_OBJ_FLAG_HIDDEN);
        }
        if (smile_obj != NULL) {
            lv_obj_add_flag(smile_obj, LV_OBJ_FLAG_HIDDEN);
        }
        ESP_LOGI(TAG, "Petting smile timeout, switch back to idle");
    }
    
    /* 删除定时器 */
    if (petting_smile_timer != NULL) {
        lv_timer_del(petting_smile_timer);
        petting_smile_timer = NULL;
    }
}

static void screen_clicked_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    device_state_t cur = get_state();
    
    /* IDLE 状态下的抚摸交互：滑动时显示手指图标、爱心并立即切换到笑脸 */
    if (cur == STATE_IDLE && code == LV_EVENT_PRESSING) {
        /* 获取触摸点坐标 */
        lv_indev_t *indev = lv_indev_get_act();
        if (indev && hand_obj != NULL) {
            lv_point_t point;
            lv_indev_get_point(indev, &point);
            
            /* 显示手指图标并移动到触摸点 */
            lv_obj_clear_flag(hand_obj, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(hand_obj, point.x - 60, point.y - 60);  /* 居中显示（120x120图标）*/
            
            /* 显示爱心图标（居中靠上，固定位置）*/
            if (heart_obj != NULL) {
                lv_obj_clear_flag(heart_obj, LV_OBJ_FLAG_HIDDEN);
            }
            
            /* 立即切换到 smile 表情 */
            if (idle_obj != NULL) {
                lv_obj_add_flag(idle_obj, LV_OBJ_FLAG_HIDDEN);
            }
            if (smile_obj != NULL) {
                lv_obj_clear_flag(smile_obj, LV_OBJ_FLAG_HIDDEN);
            }
        }
        return;
    }
    
    /* IDLE 状态下的抚摸交互：停止滑动后隐藏爱心，保持笑脸1秒 */
    if (cur == STATE_IDLE && code == LV_EVENT_RELEASED) {
        /* 隐藏手指图标和爱心图标 */
        if (hand_obj != NULL) {
            lv_obj_add_flag(hand_obj, LV_OBJ_FLAG_HIDDEN);
        }
        if (heart_obj != NULL) {
            lv_obj_add_flag(heart_obj, LV_OBJ_FLAG_HIDDEN);
        }
        
        /* smile 表情保持显示，1秒后切换回 idle */
        /* 删除之前的定时器（如果存在）*/
        if (petting_smile_timer != NULL) {
            lv_timer_del(petting_smile_timer);
        }
        
        /* 创建新定时器：1秒后切换回 idle */
        petting_smile_timer = lv_timer_create(petting_smile_timer_cb, 1000, NULL);
        lv_timer_set_repeat_count(petting_smile_timer, 1);
        
        ESP_LOGI(TAG, "Petting stopped, keep smile for 1s");
        return;
    }
    
    /* IDLE 状态下的点击事件：需要双击才能唤醒 LISTENING */
    if (cur == STATE_IDLE && code == LV_EVENT_CLICKED) {
        int64_t now_us = esp_timer_get_time();
        int64_t delta_ms = (now_us - last_click_time_us) / 1000;
        last_click_time_us = now_us;
        
        if (delta_ms > 0 && delta_ms < DOUBLE_CLICK_THRESHOLD_MS) {
            /* 双击检测成功，进入 LISTENING */
            ESP_LOGI(TAG, "Double click detected (%.0f ms), enter LISTENING", (float)delta_ms);
            set_state(STATE_LISTENING);
        } else {
            /* 首次点击或间隔过长，等待第二次点击 */
            ESP_LOGI(TAG, "First click, waiting for double click...");
        }
        return;
    }
    
    /* 其他状态：单击处理 */
    if (code == LV_EVENT_CLICKED) {
        switch (cur) {
        case STATE_LISTENING: {
            audio_stop_listening();
            (void)audio_wait_record_done(2000);
            set_state(STATE_THINKING);  /* 直接进入 THINKING，跳过 RECORDED */
            break;
        }
        case STATE_RECORDED:
            /* RECORDED 状态已废弃，直接跳转到 THINKING */
            set_state(STATE_THINKING);
            break;
        case STATE_THINKING:
            /* THINKING 会自动切换到 SPEAKING，用户点击无效 */
            break;
        case STATE_SPEAKING:
            /* SPEAKING 播放音频完成后会自动返回 IDLE，但允许用户点击提前中断 */
            set_state(STATE_IDLE);
            break;
        default:
            break;
        }
    }
}

void ui_init(void)
{
    if (disp_handle == NULL) {
        ESP_LOGW(TAG, "Display not inited, skip UI init");
        return;
    }
    lvgl_port_lock(0);
    screen = lv_display_get_screen_active(disp_handle);
    
    /* 设置黑色背景（作为图片后面的底色） */
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
    
    /* 创建背景图片 */
    bg_img = lv_img_create(screen);
    lv_img_set_src(bg_img, &background_img);
    lv_obj_align(bg_img, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(bg_img, LV_OBJ_FLAG_CLICKABLE);  /* 图片不响应点击 */
    
    /* 创建柴犬图片（叠加在背景上，只在 IDLE/THINKING 显示）*/
    idle_obj = lv_img_create(screen);
    lv_img_set_src(idle_obj, &idle_img);
    lv_obj_align(idle_obj, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(idle_obj, LV_OBJ_FLAG_CLICKABLE);  /* 图片不响应点击 */
    lv_obj_add_flag(idle_obj, LV_OBJ_FLAG_HIDDEN);  /* 初始隐藏 */
    
    /* 创建笑脸柴犬图片（叠加在背景上，只在 SPEAKING/LISTENING 显示）*/
    smile_obj = lv_img_create(screen);
    lv_img_set_src(smile_obj, &smile_img);
    lv_obj_align(smile_obj, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(smile_obj, LV_OBJ_FLAG_CLICKABLE);  /* 图片不响应点击 */
    lv_obj_add_flag(smile_obj, LV_OBJ_FLAG_HIDDEN);  /* 初始隐藏 */
    
    /* 创建手指图标（IDLE 状态抚摸交互时显示）*/
    hand_obj = lv_img_create(screen);
    lv_img_set_src(hand_obj, &hand_img);
    lv_obj_clear_flag(hand_obj, LV_OBJ_FLAG_CLICKABLE);  /* 图片不响应点击 */
    lv_obj_add_flag(hand_obj, LV_OBJ_FLAG_HIDDEN);  /* 初始隐藏 */
    
    /* 创建爱心图标（IDLE 状态抚摸交互时显示，居中靠上）*/
    heart_obj = lv_img_create(screen);
    lv_img_set_src(heart_obj, &heart_img);
    lv_obj_align(heart_obj, LV_ALIGN_TOP_MID, 0, 50);  /* 居中靠上，距离顶部60px */
    lv_obj_clear_flag(heart_obj, LV_OBJ_FLAG_CLICKABLE);  /* 图片不响应点击 */
    lv_obj_add_flag(heart_obj, LV_OBJ_FLAG_HIDDEN);  /* 初始隐藏 */
    
    /* 屏幕交互事件（在图片之上） */
    lv_obj_add_event_cb(screen, screen_clicked_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(screen, screen_clicked_cb, LV_EVENT_PRESSING, NULL);  /* 滑动抚摸 */
    lv_obj_add_event_cb(screen, screen_clicked_cb, LV_EVENT_RELEASED, NULL);  /* 停止抚摸 */
    
    /* 状态标签 */
    state_label = lv_label_create(screen);
    lv_obj_set_style_text_color(state_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(state_label, &lv_font_montserrat_14, 0);
    lv_label_set_text(state_label, "IDLE");
    lv_obj_align(state_label, LV_ALIGN_TOP_MID, 0, 12);
    
    /* 回复文本标签 */
    reply_label = lv_label_create(screen);
    lv_obj_set_style_text_color(reply_label, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(reply_label, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(reply_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(reply_label, LCD_H_RES - 24);
    lv_obj_align(reply_label, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(reply_label, "");
    lv_obj_add_flag(reply_label, LV_OBJ_FLAG_HIDDEN);
    
    lvgl_port_unlock();
    ESP_LOGI(TAG, "UI init with background image (IDLE -[double click]-> LISTENING -> THINKING -> auto SPEAKING -> auto IDLE)");
}

void ui_update(device_state_t state)
{
    if (screen == NULL) {
        return;
    }
    lv_color_t text_color;
    const char *state_name;
    switch (state) {
        case STATE_IDLE:
            text_color = lv_color_hex(0xFFFFFF);  /* 白色 */
            state_name = "IDLE";
            break;
        case STATE_LISTENING:
            text_color = lv_color_hex(0x00CCFF);  /* 亮蓝 */
            state_name = "LISTENING";
            break;
        case STATE_RECORDED:
            text_color = lv_color_hex(0x00FF88);  /* 亮青绿 */
            state_name = "RECORDED";
            break;
        case STATE_THINKING:
            text_color = lv_color_hex(0xFFDD00);  /* 亮黄 */
            state_name = "THINKING";
            break;
        case STATE_SPEAKING:
            text_color = lv_color_hex(0xFF88FF);  /* 亮粉红 */
            state_name = "SPEAKING";
            break;
        default:
            text_color = lv_color_hex(0xFFFFFF);
            state_name = "?";
            break;
    }
    lvgl_port_lock(0);
    /* 不再修改背景颜色，背景始终是图片 */
    if (state_label != NULL) {
        lv_label_set_text(state_label, state_name);
        lv_obj_set_style_text_color(state_label, text_color, 0);
    }
    
    /* 柴犬图片：只在 IDLE 和 THINKING 状态显示 */
    if (idle_obj != NULL) {
        if (state == STATE_IDLE || state == STATE_THINKING) {
            lv_obj_clear_flag(idle_obj, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(idle_obj, LV_OBJ_FLAG_HIDDEN);
        }
    }
    
    /* 笑脸柴犬图片：只在 SPEAKING 和 LISTENING 状态显示 */
    if (smile_obj != NULL) {
        if (state == STATE_SPEAKING || state == STATE_LISTENING) {
            lv_obj_clear_flag(smile_obj, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(smile_obj, LV_OBJ_FLAG_HIDDEN);
        }
    }
    
    /* 手指图标和爱心图标：状态切换时隐藏（只在 IDLE 抚摸时显示）*/
    if (state != STATE_IDLE) {
        if (hand_obj != NULL) {
            lv_obj_add_flag(hand_obj, LV_OBJ_FLAG_HIDDEN);
        }
        if (heart_obj != NULL) {
            lv_obj_add_flag(heart_obj, LV_OBJ_FLAG_HIDDEN);
        }
    }
    
    /* 取消抚摸笑脸定时器（如果离开 IDLE 状态）*/
    if (state != STATE_IDLE && petting_smile_timer != NULL) {
        lv_timer_del(petting_smile_timer);
        petting_smile_timer = NULL;
    }
    
    if (reply_label != NULL) {
        if (state == STATE_SPEAKING) {
            const char *txt = state_get_last_reply_text();
            lv_label_set_text(reply_label, (txt != NULL && txt[0] != '\0') ? txt : "(no reply)");
            lv_obj_clear_flag(reply_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_label_set_text(reply_label, "");
            lv_obj_add_flag(reply_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
    lvgl_port_unlock();
}
