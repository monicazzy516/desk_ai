#include "ui.h"
#include "state.h"
#include "audio.h"
#include "esp_log.h"
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
static lv_obj_t *state_label = NULL;   /* 当前 state 名称，便于 debug */
static lv_obj_t *reply_label = NULL;   /* SPEAKING 时显示后端 reply_text */

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
            .swap_bytes = 0,
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

static void screen_clicked_cb(lv_event_t *e)
{
    (void)e;
    device_state_t cur = get_state();
    switch (cur) {
    case STATE_IDLE:
        set_state(STATE_LISTENING);
        break;
    case STATE_LISTENING: {
        audio_stop_listening();
        (void)audio_wait_record_done(2000);
        set_state(STATE_RECORDED);
        break;
    }
    case STATE_RECORDED:
        set_state(STATE_THINKING);
        break;
    case STATE_THINKING:
        set_state(STATE_SPEAKING);
        break;
    case STATE_SPEAKING:
        set_state(STATE_IDLE);
        break;
    default:
        break;
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
    lv_obj_add_event_cb(screen, screen_clicked_cb, LV_EVENT_CLICKED, NULL);
    state_label = lv_label_create(screen);
    lv_obj_set_style_text_color(state_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(state_label, &lv_font_montserrat_14, 0);
    lv_label_set_text(state_label, "IDLE");
    lv_obj_align(state_label, LV_ALIGN_TOP_MID, 0, 12);
    reply_label = lv_label_create(screen);
    lv_obj_set_style_text_color(reply_label, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(reply_label, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(reply_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(reply_label, LCD_H_RES - 24);
    lv_obj_align(reply_label, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(reply_label, "");
    lv_obj_add_flag(reply_label, LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();
    ESP_LOGI(TAG, "UI init (click: IDLE->LISTENING->RECORDED->THINKING->SPEAKING->IDLE)");
}

void ui_update(device_state_t state)
{
    if (screen == NULL) {
        return;
    }
    lv_color_t color;
    const char *state_name;
    switch (state) {
        case STATE_IDLE:
            color = lv_color_hex(0x000000);
            state_name = "IDLE";
            break;
        case STATE_LISTENING:
            color = lv_color_hex(0x0066CC);  /* 蓝 */
            state_name = "LISTENING";
            break;
        case STATE_RECORDED:
            color = lv_color_hex(0x00AA88);  /* 青绿 */
            state_name = "RECORDED";
            break;
        case STATE_THINKING:
            color = lv_color_hex(0xCCAA00);  /* 黄 */
            state_name = "THINKING";
            break;
        case STATE_SPEAKING:
            color = lv_color_hex(0xFFFFFF);  /* 白 */
            state_name = "SPEAKING";
            break;
        default:
            color = lv_color_hex(0x000000);
            state_name = "?";
            break;
    }
    lvgl_port_lock(0);
    lv_obj_set_style_bg_color(screen, color, 0);
    if (state_label != NULL) {
        lv_label_set_text(state_label, state_name);
        lv_color_t text_color = (state == STATE_IDLE || state == STATE_LISTENING || state == STATE_RECORDED)
            ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x000000);
        lv_obj_set_style_text_color(state_label, text_color, 0);
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
