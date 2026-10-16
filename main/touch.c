#include "touch.h"

#include "diag.h"
#include "display.h"  // LCD_H_RES / LCD_V_RES for clamping
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "touch";

// CST816D registers
#define REG_GESTURE_ID      0x01    // Followed by: finger count, XH, XL, YH, YL
#define REG_CHIP_ID         0xA7
#define REG_FW_VERSION      0xA9
#define REG_DIS_AUTO_SLEEP  0xFE    // Write non-zero to keep the chip awake

#define I2C_TIMEOUT_MS          30
#define BUS_RESET_AFTER_ERRORS  5   // Consecutive failures before an I2C bus reset
#define REINIT_AFTER_ERRORS     20  // Consecutive failures before a full bus re-init + TP_RST
#define ERROR_LOG_INTERVAL_US   (5 * 1000 * 1000)

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static SemaphoreHandle_t s_int_sem;
static bool s_int_enabled;

static uint32_t s_err_total;
static uint32_t s_err_consecutive;
static uint32_t s_err_since_log;
static uint32_t s_bus_resets;
static uint32_t s_reinits;
static int64_t s_last_err_log_us;
static esp_err_t s_last_err = ESP_OK;
static uint8_t s_chip_id, s_fw;

static esp_err_t read_regs(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, I2C_TIMEOUT_MS);
}

static esp_err_t write_reg(uint8_t reg, uint8_t val)
{
    uint8_t data[2] = { reg, val };
    return i2c_master_transmit(s_dev, data, sizeof(data), I2C_TIMEOUT_MS);
}

static void IRAM_ATTR tp_int_isr(void *arg)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_int_sem, &woken);
    if (woken) {
        portYIELD_FROM_ISR();
    }
}

static void tp_reset_pulse(void)
{
    if (PIN_TP_RST < 0) {
        return;
    }
    gpio_set_level(PIN_TP_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_TP_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(60));   // CST816 needs ~50 ms to boot after reset
}

// Check idle levels of TP_SDA/TP_SCL before the I2C driver takes them over.
// Both must read high (pulled up). A line stuck low points at wiring, a missing
// pull-up or a slave holding the bus. If SDA is stuck low, clock SCL 9 times
// to free a slave that is stuck mid-byte.
static void check_bus_lines(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PIN_TP_SDA) | (1ULL << PIN_TP_SCL),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&cfg);
    esp_rom_delay_us(50);
    int sda = gpio_get_level(PIN_TP_SDA);
    int scl = gpio_get_level(PIN_TP_SCL);
    ESP_LOGD(TAG, "Idle line levels: TP_SDA=%d TP_SCL=%d (both should be 1)", sda, scl);
    if (scl == 0) {
        ESP_LOGE(TAG, "TP_SCL is stuck LOW - check wiring/short on GPIO%d", PIN_TP_SCL);
    }
    if (sda == 0 && scl == 1) {
        ESP_LOGW(TAG, "TP_SDA is stuck LOW - sending 9 SCL pulses to free the bus");
        gpio_set_direction(PIN_TP_SCL, GPIO_MODE_INPUT_OUTPUT_OD);
        for (int i = 0; i < 9 && gpio_get_level(PIN_TP_SDA) == 0; i++) {
            gpio_set_level(PIN_TP_SCL, 0);
            esp_rom_delay_us(10);
            gpio_set_level(PIN_TP_SCL, 1);
            esp_rom_delay_us(10);
        }
        ESP_LOGI(TAG, "After recovery: TP_SDA=%d", gpio_get_level(PIN_TP_SDA));
    }
}

static esp_err_t bus_create(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = TOUCH_I2C_PORT,
        .sda_io_num = PIN_TP_SDA,
        .scl_io_num = PIN_TP_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,  // The board/LCD module also has pull-ups
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "i2c_new_master_bus");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TOUCH_I2C_ADDR,
        .scl_speed_hz = TOUCH_I2C_HZ,
        .scl_wait_us = 20000,   // Tolerate clock stretching by the CST816
    };
    return i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
}

static void bus_destroy(void)
{
    if (s_dev) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }
    if (s_bus) {
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
    }
}

static void bus_scan(void)
{
    char found[128] = "";
    size_t n = 0;
    int count = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(s_bus, addr, 20) == ESP_OK) {
            n += snprintf(found + n, sizeof(found) - n, " 0x%02X", addr);
            count++;
        }
        if (n >= sizeof(found) - 6) {
            break;
        }
    }
    // Known devices on the board bus (GPIO7/8): ES8311 codec 0x18, CST816D 0x15.
    ESP_LOGI(TAG, "I2C scan (TP_SDA/TP_SCL): %d device(s):%s", count, count ? found : " none");
}

static void update_status(void)
{
    if (s_err_consecutive == 0) {
        diag_set_touch_status("ok (ID 0x%02X FW 0x%02X%s) err=%lu resets=%lu",
                              s_chip_id, s_fw, s_int_enabled ? ", INT" : ", poll",
                              (unsigned long)s_err_total, (unsigned long)(s_bus_resets + s_reinits));
    } else {
        diag_set_touch_status("failing: %s x%lu (total err=%lu, bus resets=%lu, re-inits=%lu)",
                              esp_err_to_name(s_last_err), (unsigned long)s_err_consecutive,
                              (unsigned long)s_err_total, (unsigned long)s_bus_resets,
                              (unsigned long)s_reinits);
    }
}

esp_err_t touch_init(void)
{
    ESP_LOGD(TAG, "Pins: TP_SDA=%d TP_SCL=%d TP_INT=%d TP_RST=%d addr=0x%02X, %d Hz",
             PIN_TP_SDA, PIN_TP_SCL, PIN_TP_INT, PIN_TP_RST, TOUCH_I2C_ADDR, TOUCH_I2C_HZ);
    diag_set_touch_status("initializing");

#ifndef CONFIG_MYPI_TOUCH_VERBOSE_I2C_LOGS
    // The I2C driver logs an error line per failed transaction, which floods the
    // console at the poll rate. We log our own rate-limited summary instead.
    esp_log_level_set("i2c.master", ESP_LOG_NONE);
#endif

    if (PIN_TP_RST >= 0) {
        gpio_config_t rst_cfg = {
            .pin_bit_mask = 1ULL << PIN_TP_RST,
            .mode = GPIO_MODE_OUTPUT,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&rst_cfg), TAG, "tp_rst gpio");
        tp_reset_pulse();
    }

    check_bus_lines();

    esp_err_t err = bus_create();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[FAIL] I2C bus init: %s", esp_err_to_name(err));
        diag_set_touch_status("I2C bus init failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGD(TAG, "[ OK ] I2C bus (port %d)", TOUCH_I2C_PORT);

    if (diag_debug_enabled()) {
        bus_scan();
    }

    err = i2c_master_probe(s_bus, TOUCH_I2C_ADDR, 50);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[FAIL] CST816D did not ACK at 0x%02X (%s) - check TP_SDA/TP_SCL wiring",
                 TOUCH_I2C_ADDR, esp_err_to_name(err));
    } else {
        ESP_LOGD(TAG, "[ OK ] CST816D ACK at 0x%02X", TOUCH_I2C_ADDR);
    }

    if (read_regs(REG_CHIP_ID, &s_chip_id, 1) == ESP_OK && read_regs(REG_FW_VERSION, &s_fw, 1) == ESP_OK) {
        ESP_LOGD(TAG, "[ OK ] CST816 chip ID 0x%02X, FW 0x%02X", s_chip_id, s_fw);
    } else {
        ESP_LOGW(TAG, "[FAIL] Could not read CST816 chip ID / FW version");
    }

#if CONFIG_MYPI_TOUCH_USE_INT
    if (PIN_TP_INT >= 0) {
        s_int_sem = xSemaphoreCreateBinary();
        gpio_config_t int_cfg = {
            .pin_bit_mask = 1ULL << PIN_TP_INT,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .intr_type = GPIO_INTR_NEGEDGE,   // CST816 pulls INT low when it has data
        };
        esp_err_t ierr = gpio_config(&int_cfg);
        esp_err_t serr = gpio_install_isr_service(0);
        if (serr == ESP_ERR_INVALID_STATE) {
            serr = ESP_OK;   // Already installed by another component
        }
        if (ierr == ESP_OK && serr == ESP_OK && gpio_isr_handler_add(PIN_TP_INT, tp_int_isr, NULL) == ESP_OK) {
            s_int_enabled = true;
            ESP_LOGD(TAG, "[ OK ] TP_INT interrupt on GPIO%d (idle level %d)", PIN_TP_INT,
                     gpio_get_level(PIN_TP_INT));
        } else {
            ESP_LOGW(TAG, "[FAIL] TP_INT interrupt setup (%s/%s) - falling back to polling",
                     esp_err_to_name(ierr), esp_err_to_name(serr));
        }
    }
#endif

    if (!s_int_enabled) {
        // Polling only works if the chip never sleeps.
        if (write_reg(REG_DIS_AUTO_SLEEP, 0x01) != ESP_OK) {
            ESP_LOGW(TAG, "Could not disable auto-sleep; touch may stop responding when idle");
        }
    }
    update_status();
    ESP_LOGI(TAG, "CST816 ready: ID 0x%02X FW 0x%02X, I2C %d kHz, %s", s_chip_id, s_fw,
             TOUCH_I2C_HZ / 1000, s_int_enabled ? "TP_INT interrupt" : "polling");
    return ESP_OK;
}

bool touch_wait(bool finger_down, int idle_timeout_ms)
{
    if (!s_int_enabled || finger_down) {
        // Polling mode, or tracking a finger (the CST816 pulses INT while touched,
        // but polling during contact gives smoother drag lines).
        vTaskDelay(pdMS_TO_TICKS(TOUCH_ACTIVE_POLL_MS));
        return true;
    }
    if (xSemaphoreTake(s_int_sem, pdMS_TO_TICKS(idle_timeout_ms)) == pdTRUE) {
        return true;
    }
    return false;   // No touch; caller may skip reading (chip is likely asleep)
}

static void handle_error(esp_err_t err)
{
    s_last_err = err;
    s_err_total++;
    s_err_consecutive++;
    s_err_since_log++;

    int64_t now = esp_timer_get_time();
    if (s_err_since_log == 1 || now - s_last_err_log_us >= ERROR_LOG_INTERVAL_US) {
        ESP_LOGW(TAG, "read failed: %s (%lu in a row, %lu since last report, %lu total)",
                 esp_err_to_name(err), (unsigned long)s_err_consecutive,
                 (unsigned long)s_err_since_log, (unsigned long)s_err_total);
        s_err_since_log = 0;
        s_last_err_log_us = now;
    }

    if (s_err_consecutive % REINIT_AFTER_ERRORS == 0) {
        s_reinits++;
        ESP_LOGW(TAG, "Re-initializing I2C bus + TP_RST pulse (re-init #%lu)", (unsigned long)s_reinits);
        bus_destroy();
        tp_reset_pulse();
        check_bus_lines();
        esp_err_t berr = bus_create();
        if (berr != ESP_OK) {
            ESP_LOGE(TAG, "Bus re-init failed: %s", esp_err_to_name(berr));
        }
    } else if (s_err_consecutive % BUS_RESET_AFTER_ERRORS == 0 && s_bus) {
        s_bus_resets++;
        i2c_master_bus_reset(s_bus);
    }
    update_status();
}

esp_err_t touch_read(touch_point_t *pt)
{
    if (!s_dev) {
        handle_error(ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t d[6];
    esp_err_t err = read_regs(REG_GESTURE_ID, d, sizeof(d));
    if (err != ESP_OK) {
        handle_error(err);
        return err;
    }
    if (s_err_consecutive) {
        ESP_LOGI(TAG, "read recovered after %lu failure(s)", (unsigned long)s_err_consecutive);
        s_err_consecutive = 0;
        s_err_since_log = 0;
        update_status();
    }

    pt->gesture = d[0];
    pt->pressed = (d[1] & 0x0F) > 0;
    uint16_t x = ((d[2] & 0x0F) << 8) | d[3];
    uint16_t y = ((d[4] & 0x0F) << 8) | d[5];
    pt->x = x < LCD_H_RES ? x : LCD_H_RES - 1;
    pt->y = y < LCD_V_RES ? y : LCD_V_RES - 1;
    return ESP_OK;
}

uint32_t touch_consecutive_errors(void)
{
    return s_err_consecutive;
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
