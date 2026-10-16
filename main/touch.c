#include "touch.h"

#include "display.h"  // LCD_H_RES / LCD_V_RES for clamping
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "touch";

// CST816D registers
#define REG_GESTURE_ID      0x01    // Followed by: finger count, XH, XL, YH, YL
#define REG_CHIP_ID         0xA7
#define REG_FW_VERSION      0xA9
#define REG_DIS_AUTO_SLEEP  0xFE    // Write non-zero to keep the chip awake (needed for polling)

#define I2C_TIMEOUT_MS      50

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;

static esp_err_t read_regs(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, I2C_TIMEOUT_MS);
}

static esp_err_t write_reg(uint8_t reg, uint8_t val)
{
    uint8_t data[2] = { reg, val };
    return i2c_master_transmit(s_dev, data, sizeof(data), I2C_TIMEOUT_MS);
}

esp_err_t touch_init(void)
{
    ESP_LOGI(TAG, "Pins: SDA=%d SCL=%d addr=0x%02X", TOUCH_SDA, TOUCH_SCL, TOUCH_I2C_ADDR);

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = TOUCH_I2C_PORT,
        .sda_io_num = TOUCH_SDA,
        .scl_io_num = TOUCH_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,  // The LCD module also has its own pull-ups
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "i2c bus");

    // Give the controller time to boot after power-up.
    vTaskDelay(pdMS_TO_TICKS(50));

    esp_err_t err = i2c_master_probe(s_bus, TOUCH_I2C_ADDR, I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CST816D did not ACK at 0x%02X (%s) - check SDA/SCL wiring",
                 TOUCH_I2C_ADDR, esp_err_to_name(err));
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TOUCH_I2C_ADDR,
        .scl_speed_hz = TOUCH_I2C_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev), TAG, "i2c device");

    uint8_t chip_id = 0, fw = 0;
    if (read_regs(REG_CHIP_ID, &chip_id, 1) == ESP_OK && read_regs(REG_FW_VERSION, &fw, 1) == ESP_OK) {
        ESP_LOGI(TAG, "CST816 chip ID 0x%02X, FW 0x%02X", chip_id, fw);
    }

    // Without an INT line we must poll, so stop the chip from auto-sleeping.
    if (write_reg(REG_DIS_AUTO_SLEEP, 0x01) != ESP_OK) {
        ESP_LOGW(TAG, "Could not disable auto-sleep; touch may stop responding when idle");
    }
    return ESP_OK;
}

esp_err_t touch_read(touch_point_t *pt)
{
    uint8_t d[6];
    ESP_RETURN_ON_ERROR(read_regs(REG_GESTURE_ID, d, sizeof(d)), TAG, "read");

    pt->gesture = d[0];
    pt->pressed = (d[1] & 0x0F) > 0;
    uint16_t x = ((d[2] & 0x0F) << 8) | d[3];
    uint16_t y = ((d[4] & 0x0F) << 8) | d[5];
    pt->x = x < LCD_H_RES ? x : LCD_H_RES - 1;
    pt->y = y < LCD_V_RES ? y : LCD_V_RES - 1;
    return ESP_OK;
}

const char *touch_gesture_name(uint8_t gesture)
{
    switch (gesture) {
    case TOUCH_GESTURE_NONE:        return "none";
    case TOUCH_GESTURE_SWIPE_UP:    return "swipe-up";
    case TOUCH_GESTURE_SWIPE_DOWN:  return "swipe-down";
    case TOUCH_GESTURE_SWIPE_LEFT:  return "swipe-left";
    case TOUCH_GESTURE_SWIPE_RIGHT: return "swipe-right";
    case TOUCH_GESTURE_SINGLE_TAP:  return "single-tap";
    case TOUCH_GESTURE_DOUBLE_TAP:  return "double-tap";
    case TOUCH_GESTURE_LONG_PRESS:  return "long-press";
    default:                        return "unknown";
    }
}
