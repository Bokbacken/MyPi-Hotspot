# MyPi-Hotspot

Firmware for a small Wi-Fi hotspot device built on the **Waveshare ESP32-P4-NANO** with a
**Waveshare 2" capacitive touch LCD**.

This first milestone is a minimal **hello world** that proves the display and touch work:

| Action | Result |
|---|---|
| Boot | Screen fills with navy blue |
| Tap (finger down + up, < 10 px movement) | Filled circle (radius 8 px) in a **random color** at the touch point |
| Drag / swipe | **Black** line follows the finger |
| Any touch event | Logged to the serial console at 115200 baud (`DOWN`, `DRAG`, `MOVE`, `UP`, `TAP`) |

No LVGL, no Wi-Fi yet — just `esp_lcd` (built into ESP-IDF) and raw I2C.

---

## Hardware

| Part | Details |
|---|---|
| MCU board | Waveshare ESP32-P4-NANO-WIFI6-DB — ESP32-P4NRW32X, 32 MB PSRAM, 32 MB flash |
| Wi-Fi | ESP32-C5 co-processor over SDIO (not used yet) |
| Display | Waveshare 2" LCD, ST7789, 240×320, SPI |
| Touch | CST816D, I2C address `0x15`, polled (INT and RST not connected) |

### Wiring (display module → ESP32-P4-NANO)

> ⚠️ **VERIFY BEFORE FLASHING.** The board has no fixed SPI pins (the ESP32-P4 GPIO matrix can
> route SPI to any free GPIO). `SCLK = GPIO23` and `MOSI = GPIO5` were chosen as free header pins
> that avoid strapping pins (34–38), USB-JTAG (24/25) and the Wi-Fi co-processor. If you wire
> differently, edit the `VERIFY BEFORE FLASHING` block at the top of [`main/display.h`](main/display.h).
> **MISO is not needed** — the ST7789 module is write-only, so leave it unconnected.

| LCD pin | Function | ESP32-P4 GPIO | Status |
|---|---|---|---|
| VCC | 3.3 V | 3V3 | — |
| GND | Ground | GND | — |
| SCL / CLK | SPI clock | **GPIO23** | proposed |
| SDA / DIN | SPI MOSI | **GPIO5** | proposed |
| MISO | — | not connected (display is write-only) | — |
| CS | SPI chip select | GPIO20 | confirmed |
| DC | Data / command | GPIO21 | confirmed |
| BL | Backlight (high = on) | GPIO22 | confirmed |
| RST | LCD reset | not connected (software reset) | — |
| TP_SDA | Touch I2C data | GPIO7 | confirmed |
| TP_SCL | Touch I2C clock | GPIO8 | confirmed |
| TP_INT | Touch interrupt | not connected (polling) | — |
| TP_RST | Touch reset | not connected | — |

### ESP32-P4-NANO GPIO overview

| GPIO | Use | Free for user? |
|---|---|---|
| 0, 1 | 32 kHz crystal (XTAL32K) | ❌ |
| 2, 3, 4 | Header (pad-JTAG only if strapped, off by default) | ✅ free |
| 5 | LCD MOSI | 🟡 used by this project |
| 6 | Header, but tied to Wi-Fi co-processor IO via 0 Ω (C6 schematic) | ⚠️ avoid |
| 7, 8 | I2C (touch, shared bus) | 🟡 used by this project |
| 9–13 | I2S audio codec | ❌ |
| 14–19 | SDIO to ESP32-C5 (Wi-Fi) | ❌ |
| 20, 21, 22 | LCD CS / DC / BL | 🟡 used by this project |
| 23 | LCD SCLK | 🟡 used by this project |
| 24, 25 | USB Serial/JTAG | ⚠️ avoid |
| 26, 27 | Header (USB FS OTG alt. function) | ✅ free |
| 28–31 | Ethernet RMII | ❌ |
| 32, 33 | Header | ✅ free |
| 34–36 | Strapping (35 = boot mode; 34/35 also Ethernet) | ❌ |
| 37, 38 | UART0 TX/RX (console) + strapping | ❌ |
| 39–44 | microSD (TF) card | ❌ |
| 45–48 | Header / misc (check schematic) | ⚠️ check |
| 49–53 | Ethernet PHY / speaker amplifier (PA) | ❌ |
| 54 | ESP32-C5 reset (CHIP_PU) | ❌ |

Always confirm against the official Waveshare ESP32-P4-NANO schematic before connecting new hardware.

---

## Project layout

```
MyPi-Hotspot/
├── CMakeLists.txt        Top-level ESP-IDF project file
├── sdkconfig.defaults    Target, CPU 360 MHz, PSRAM, 16 MB flash, 115200 console
└── main/
    ├── CMakeLists.txt
    ├── main.c            app_main: init, touch loop, tap/drag logic
    ├── display.h/.c      ST7789 via esp_lcd, 240×320 RGB565 framebuffer in PSRAM
    └── touch.h/.c        CST816D polling over I2C (new i2c_master driver)
```

The framebuffer (150 KB) lives in PSRAM. Only the changed ("dirty") rectangle is sent to the panel,
copied through a small internal DMA buffer in 40-row bands.

---

## Build & flash quick-start

Requires **ESP-IDF v5.5** (tested to build with v5.5.1, target `esp32p4`).

```bash
# 1. Load the ESP-IDF environment (path depends on where you installed it)
. ~/esp/esp-idf/export.sh          # Windows: run "ESP-IDF 5.5 CMD/PowerShell" from the Start menu

# 2. Select the chip (only needed once)
idf.py set-target esp32p4

# 3. Build
idf.py build

# 4. Flash and open the serial monitor (replace the port with yours)
idf.py -p /dev/ttyACM0 flash monitor    # Linux
idf.py -p /dev/cu.usbmodem* flash monitor   # macOS
idf.py -p COM5 flash monitor            # Windows
```

Exit the monitor with `Ctrl+]`.

### First USB flash tips

- Use a USB-C **data** cable (some cables are charge-only).
- If the port does not appear or flashing fails to connect, put the board in download mode:
  hold **BOOT**, press and release **RESET**, then release **BOOT**, and run the flash command again.
  Press **RESET** afterwards to start the new firmware.
- Linux: if you get "permission denied" on the port, add yourself to the `dialout` group
  (`sudo usermod -aG dialout $USER`, then log out and in).

### Expected serial output

```
I (...) main: MyPi-Hotspot hello world starting
I (...) display: Pins: SCLK=23 MOSI=5 CS=20 DC=21 BL=22
I (...) display: ST7789 240x320 ready, framebuffer 153600 bytes in PSRAM
I (...) main: Screen filled navy blue
I (...) touch: Pins: SDA=7 SCL=8 addr=0x15
I (...) main: DOWN  x=120 y=160
I (...) main: TAP   x=120 y=160 color=0x5A3F
```

### Troubleshooting

| Symptom | Likely cause |
|---|---|
| Backlight on, screen stays black/white | Wrong SCLK/MOSI pins — check `main/display.h` |
| Colors look inverted | Remove/flip `esp_lcd_panel_invert_color(..., true)` in `display.c` |
| `CST816D did not ACK at 0x15` | SDA/SCL swapped or not connected — check `main/touch.h` |
| Touch works briefly then stops | Controller went to sleep; auto-sleep is disabled at init, power-cycle the board |
| Touch position mirrored | Panel orientation differs; mirror `x`/`y` in `touch_read()` |
