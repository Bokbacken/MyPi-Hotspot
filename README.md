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

> ⚠️ **VERIFY BEFORE FLASHING.** `SCLK` and `MOSI` are best-guess defaults. Check them against
> your actual wiring and edit the `VERIFY BEFORE FLASHING` block at the top of
> [`main/display.h`](main/display.h) and [`main/touch.h`](main/touch.h) if needed.

| LCD pin | Function | ESP32-P4 GPIO | Status |
|---|---|---|---|
| VCC | 3.3 V | 3V3 | — |
| GND | Ground | GND | — |
| SCL / CLK | SPI clock | **GPIO36** | ⚠️ VERIFY |
| SDA / DIN | SPI MOSI | **GPIO35** | ⚠️ VERIFY |
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
| 2–6 | Header | ✅ free |
| 7, 8 | I2C (touch, shared bus) | 🟡 used by this project |
| 9–13 | I2S audio codec | ❌ |
| 14–19 | SDIO to ESP32-C5 (Wi-Fi) | ❌ |
| 20, 21, 22 | LCD CS / DC / BL | 🟡 used by this project |
| 23–27 | Header | ✅ free |
| 28–34 | Header (check board schematic) | ✅ mostly free |
| 35, 36 | LCD MOSI / SCLK (proposed) | 🟡 used by this project — VERIFY |
| 37, 38 | UART0 TX/RX (console) | ❌ |
| 39–44 | microSD (TF) card | ❌ |
| 45–48 | Header / misc (check schematic) | ⚠️ check |
| 49–53 | Ethernet PHY / speaker amplifier (PA) | ❌ |
| 54 | Header (check schematic) | ⚠️ check |

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
I (...) display: Pins: SCLK=36 MOSI=35 CS=20 DC=21 BL=22
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
