#pragma once

// ============================================================================
// === VERIFY BEFORE FLASHING — adjust to your actual wiring ===
// Waveshare 2" Capacitive Touch LCD module -> Waveshare ESP32-P4-NANO.
// Macro names match the labels printed on the LCD module (PIN_<label>).
//
// The ESP32-P4 GPIO matrix can route SPI/I2C to any free GPIO.
// Never use: 34-38 (strapping; 35 low at reset = download mode, 37/38 = console),
//            24/25 (USB Serial/JTAG), 6 (tied to Wi-Fi co-processor IO),
//            14-19 + 54 (ESP32-C5 SDIO/reset), 28-31 (Ethernet RMII).
// Set a pin to (-1) if that wire is not connected.
// ============================================================================

// --- SPI bus (shared by the LCD and the module's microSD slot) ---
#define PIN_SCLK        23      // SCLK      SPI clock
#define PIN_MOSI        5       // MOSI      SPI data to LCD / SD
#define PIN_MISO        4       // MISO      SPI data from SD card (LCD is write-only)
#define PIN_SD_CS       32      // SD_CS     microSD chip select (held high = deselected)

// --- LCD (ST7789) ---
#define PIN_LCD_CS      20      // LCD_CS    LCD chip select
#define PIN_LCD_DC      21      // LCD_DC    data/command select
#define PIN_LCD_RESET   26      // LCD_RESET hardware reset (active low)
#define PIN_LCD_BL      22      // LCD_BL    backlight (high = on)

// --- Touch (CST816D, I2C; shared bus with on-board ES8311 0x18) ---
#define PIN_TP_SDA      7       // TP_SDA    I2C data
#define PIN_TP_SCL      8       // TP_SCL    I2C clock
#define PIN_TP_INT      3       // TP_INT    touch interrupt (active low; firmware still polls)
#define PIN_TP_RST      2       // TP_RST    touch reset (active low)
// ============================================================================
