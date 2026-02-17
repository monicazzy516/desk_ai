#include "TCA9554PWR.h"

static const char *TAG = "TCA9554";
static i2c_master_dev_handle_t tca9554_dev = NULL;

uint8_t Read_REG(uint8_t REG)
{
    if (!tca9554_dev) return 0;
    uint8_t data = 0;
    esp_err_t ret = i2c_master_transmit_receive(tca9554_dev, &REG, 1, &data, 1, 1000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read reg 0x%02x: %s", REG, esp_err_to_name(ret));
    }
    return data;
}

void Write_REG(uint8_t REG, uint8_t Data)
{
    if (!tca9554_dev) return;
    uint8_t write_buf[2] = {REG, Data};
    esp_err_t ret = i2c_master_transmit(tca9554_dev, write_buf, 2, 1000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write reg 0x%02x: %s", REG, esp_err_to_name(ret));
    }
}

void Mode_EXIO(uint8_t Pin, uint8_t State)
{
    uint8_t bitsStatus = Read_REG(TCA9554_CONFIG_REG);
    uint8_t Data;
    if (State == 0) {
        Data = bitsStatus & ~(1 << (Pin - 1));
    } else {
        Data = bitsStatus | (1 << (Pin - 1));
    }
    Write_REG(TCA9554_CONFIG_REG, Data);
}

void Mode_EXIOS(uint8_t PinState)
{
    Write_REG(TCA9554_CONFIG_REG, PinState);
}

uint8_t Read_EXIO(uint8_t Pin)
{
    uint8_t inputBits = Read_REG(TCA9554_INPUT_REG);
    return (inputBits >> (Pin - 1)) & 0x01;
}

uint8_t Read_EXIOS(void)
{
    return Read_REG(TCA9554_INPUT_REG);
}

void Set_EXIO(uint8_t Pin, bool State)
{
    uint8_t bitsStatus = Read_REG(TCA9554_OUTPUT_REG);
    uint8_t Data;
    if (State) {
        Data = bitsStatus | (1 << (Pin - 1));
    } else {
        Data = bitsStatus & ~(1 << (Pin - 1));
    }
    Write_REG(TCA9554_OUTPUT_REG, Data);
}

void Set_EXIOS(uint8_t PinState)
{
    Write_REG(TCA9554_OUTPUT_REG, PinState);
}

void Set_Toggle(uint8_t Pin)
{
    uint8_t bitsStatus = Read_REG(TCA9554_OUTPUT_REG);
    uint8_t Data = bitsStatus ^ (1 << (Pin - 1));
    Write_REG(TCA9554_OUTPUT_REG, Data);
}

void TCA9554PWR_Init(uint8_t PinState)
{
    Write_REG(TCA9554_CONFIG_REG, PinState);
    Write_REG(TCA9554_OUTPUT_REG, 0x00);
}

esp_err_t EXIO_Init(i2c_master_bus_handle_t i2c_bus)
{
    if (!i2c_bus) {
        ESP_LOGE(TAG, "I2C bus is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TCA9554_ADDRESS,
        .scl_speed_hz = 400000,
    };

    esp_err_t ret = i2c_master_bus_add_device(i2c_bus, &dev_cfg, &tca9554_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add TCA9554 device: %s", esp_err_to_name(ret));
        return ret;
    }

    TCA9554PWR_Init(0x00);  // 所有引脚默认输出模式
    ESP_LOGI(TAG, "TCA9554 initialized");
    return ESP_OK;
}
