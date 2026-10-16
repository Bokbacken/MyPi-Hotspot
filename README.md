# MyPi-Hotspot

Firmware for a small Wi-Fi hotspot device built on the **Waveshare ESP32-P4-NANO** with a
**Waveshare 2" capacitive touch LCD**.

Current version: **0.2.0-debug** — the display/touch **hello world** plus diagnostics, Wi-Fi,
a web status page and over-the-air (OTA) updates, so the board can be debugged without a serial cable.

| Action | Result |
|---|---|
| Boot | Boot banner on serial, LCD test pattern (R/G/B/white/black + color bars), then navy blue |
| Tap (finger down + up, < 10 px movement) | Filled circle (radius 8 px) in a **random color** at the touch point |
| Drag / swipe | **Black** line follows the finger |
| Any touch event | Logged to the serial console at 115200 baud (`DOWN`, `DRAG`, `MOVE`, `UP`, `TAP`) and to the web page |
| Every 5 s | Heartbeat line: uptime, heap/PSRAM, Wi-Fi state/IP/RSSI, touch count, frames flushed, OTA state |
| Wi-Fi connected | Web UI at `http://mypi-hotspot.local/` (or the IP from the log): status, log, OTA upload |

No LVGL — just `esp_lcd` (built into ESP-IDF), raw I2C, and ESP-Hosted for Wi-Fi.
A failing display or touch controller is logged but does **not** stop Wi-Fi/web, so you can always
read the diagnostics remotely.

> ⚠️ Not yet tested on real hardware. Treat the first flash as a diagnostic run and check the log.

---

## Hardware

| Part | Details |
|---|---|
| MCU board | Waveshare ESP32-P4-NANO-WIFI6-DB — ESP32-P4NRW32X, 32 MB PSRAM, 32 MB flash |
| Wi-Fi | ESP32-C5 co-processor over SDIO 4-bit (CLK=18, CMD=19, D0–D3=14–17, reset=54) via ESP-Hosted |
| Display | Waveshare 2" LCD, ST7789, 240×320, SPI |
| Touch | CST816D, I2C address `0x15`, polled (TP_RST pulsed at init, TP_INT read-ready) |

### Wiring (display module → ESP32-P4-NANO)

> ⚠️ **VERIFY BEFORE FLASHING.** All GPIO assignments live in one file, [`main/pins.h`](main/pins.h),
> with macro names matching the module labels (`PIN_SCLK`, `PIN_LCD_CS`, `PIN_TP_INT`, …).
> The board has no fixed SPI pins (the ESP32-P4 GPIO matrix can route SPI to any free GPIO); the
> proposed pins avoid strapping pins (34–38), USB-JTAG (24/25) and the Wi-Fi co-processor.
> Set a pin to `-1` in `pins.h` if you leave that wire unconnected.

The display is SPI; only the touch controller (`TP_*`) is I2C. The SPI bus is shared between the
LCD and the module's microSD slot (`SD_CS`), which is why `MISO` exists.

| Module pin | Bus | Function | ESP32-P4 GPIO | Status |
|---|---|---|---|---|
| VCC | — | 3.3 V supply | 3V3 | — |
| GND | — | Ground | GND | — |
| SCLK | SPI | SPI clock | **GPIO23** | proposed |
| MOSI | SPI | Data to LCD / SD | **GPIO5** | proposed |
| MISO | SPI | Data from SD card (LCD is write-only) | **GPIO4** | proposed |
| SD_CS | SPI | microSD chip select (held high) | **GPIO32** | proposed |
| LCD_CS | SPI | LCD chip select | GPIO20 | confirmed |
| LCD_DC | — | LCD data / command | GPIO21 | confirmed |
| LCD_RESET | — | LCD hardware reset (active low) | **GPIO26** | proposed |
| LCD_BL | — | Backlight (high = on) | GPIO22 | confirmed |
| TP_SDA | I2C | Touch data | GPIO7 | confirmed |
| TP_SCL | I2C | Touch clock | GPIO8 | confirmed |
| TP_INT | — | Touch interrupt (active low, polled for now) | **GPIO3** | proposed |
| TP_RST | — | Touch reset (active low) | **GPIO2** | proposed |

### ESP32-P4-NANO GPIO overview

| GPIO | Use | Free for user? |
|---|---|---|
| 0, 1 | 32 kHz crystal (XTAL32K) | ❌ |
| 2 | TP_RST | 🟡 used by this project |
| 3 | TP_INT | 🟡 used by this project |
| 4 | MISO | 🟡 used by this project |
| 5 | MOSI | 🟡 used by this project |
| 6 | Header, but tied to Wi-Fi co-processor IO via 0 Ω (C6 schematic) | ⚠️ avoid |
| 7, 8 | TP_SDA / TP_SCL (I2C, shared with on-board ES8311) | 🟡 used by this project |
| 9–13 | I2S audio codec | ❌ |
| 14–19 | SDIO to ESP32-C5 (Wi-Fi) | ❌ |
| 20, 21, 22 | LCD_CS / LCD_DC / LCD_BL | 🟡 used by this project |
| 23 | SCLK | 🟡 used by this project |
| 24, 25 | USB Serial/JTAG | ⚠️ avoid |
| 26 | LCD_RESET | 🟡 used by this project |
| 27 | Header | ✅ free |
| 28–31 | Ethernet RMII | ❌ |
| 32 | SD_CS | 🟡 used by this project |
| 33 | Header | ✅ free |
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
├── CMakeLists.txt        Top-level ESP-IDF project file (PROJECT_VER)
├── sdkconfig.defaults    Chip rev v3.x, CPU 400 MHz, PSRAM, 16 MB flash, custom partitions, rollback, ESP-Hosted
├── partitions.csv        nvs, otadata, phy_init, ota_0 (6 MB), ota_1 (6 MB), storage (FAT)
└── main/
    ├── CMakeLists.txt
    ├── Kconfig.projbuild Project options (menuconfig → "MyPi-Hotspot")
    ├── idf_component.yml Managed components: esp_wifi_remote, esp_hosted, mdns
    ├── main.c            app_main: init, touch loop, tap/drag logic, OTA rollback confirm
    ├── pins.h            All GPIO assignments (module pin labels) — VERIFY BEFORE FLASHING
    ├── display.h/.c      ST7789 via esp_lcd, 240×320 RGB565 framebuffer in PSRAM, test pattern, ID reads
    ├── touch.h/.c        CST816D polling over I2C (new i2c_master driver)
    ├── diag.h/.c         Boot banner, shared status struct, heartbeat task
    ├── logbuf.h/.c       RAM ring buffer mirroring all log output (served at /api/log)
    ├── wifi.h/.c         Wi-Fi STA over ESP-Hosted, reconnect backoff, RSSI, mDNS
    └── web.h/.c          HTTP server: status page, JSON API, log, OTA upload
```

The framebuffer (150 KB) lives in PSRAM. Only the changed ("dirty") rectangle is sent to the panel,
copied through a small internal DMA buffer in 40-row bands.

---

## Flash a prebuilt binary (no ESP-IDF needed)

Every push is built by GitHub Actions ([`.github/workflows/build.yml`](.github/workflows/build.yml)),
once that workflow file is present in the repo.

> **Upgrading from the old hello world?** v0.2 moved the partition table from `0x8000` to `0xA000`
> and added two OTA slots, so the **first** flash must be the full `mypi-hotspot-merged.bin` at
> `0x0` over USB. After that, updates can go over Wi-Fi (see [OTA updates](#ota-updates)).

1. Open the repo's **Actions** tab → latest **Build firmware** run → download the
   `mypi-hotspot-firmware-<sha>` artifact (zip) and unzip it.
   Tagged versions (`v*`) also attach the files to a GitHub Release.
2. Connect the board with a USB-C data cable.
3. In **Chrome or Edge** open <https://espressif.github.io/esptool-js/>, click **Connect**, pick the
   serial port, add `mypi-hotspot-merged.bin` at address **`0x0`**, and click **Program**.
4. Press **RESET** on the board. Use the page's **Console** tab (115200 baud) to see the log.

Alternative with Python only: `pip install esptool` then
`esptool.py --chip esp32p4 -p <PORT> write_flash 0x0 mypi-hotspot-merged.bin`.

If the board will not connect: hold **BOOT**, tap **RESET**, release **BOOT**, then retry.

If esptool-js fails with *"Serial data stream stopped: possible serial noise or corruption"*
(seen with the on-board CH343 USB-UART, VID `0x1a86` / PID `0x55d3`): set **Baudrate** to
**115200** *before* clicking Connect, connect directly (no USB hub), and try another data cable.

---

## Build & flash quick-start

Requires **ESP-IDF v6.1 or newer** (tested with v6.1, target `esp32p4`). The board uses
ESP32-P4 **rev v3.1** silicon; older IDF releases only support rev <3.0 and the bootloader will
refuse to start. `sdkconfig.defaults` sets `CONFIG_ESP32P4_REV_MIN_300=y` (rev v3.x) and
`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_400=y`. If you change IDF version or `sdkconfig.defaults`, run
`idf.py fullclean` (or delete `sdkconfig`) first. The first configure downloads the managed
components (`esp_wifi_remote`, `esp_hosted`, `mdns`) and needs internet access.

Wi-Fi credentials, hostname, LCD SPI clock / color options and diagnostics are set in
`idf.py menuconfig` → **MyPi-Hotspot**:

| Option | Default | Notes |
|---|---|---|
| `CONFIG_MYPI_WIFI_SSID` / `CONFIG_MYPI_WIFI_PASSWORD` | `TOFTA` / `bokbacken` | Station mode |
| `CONFIG_MYPI_WIFI_BACKOFF_MAX_S` | 60 | Reconnect backoff 1 s → max |
| `CONFIG_MYPI_HOSTNAME` | `mypi-hotspot` | DHCP + mDNS name (`mypi-hotspot.local`) |
| `CONFIG_MYPI_LCD_SPI_CLOCK_MHZ` | 10 | Deliberately slow for jumper wires; try 40 once the image is stable |
| `CONFIG_MYPI_LCD_INVERT_COLOR` | y | ST7789 panels usually need inversion |
| `CONFIG_MYPI_LCD_BGR_ORDER` | n | Swap if red and blue are exchanged |
| `CONFIG_MYPI_LCD_TEST_PATTERN` | y | Full-screen colors + color bars at boot |
| `CONFIG_MYPI_HEARTBEAT_INTERVAL_S` | 5 | Seconds between heartbeat lines (1–3600) |
| `CONFIG_MYPI_LOG_BUFFER_SIZE` | 16384 | RAM log ring buffer (PSRAM) |

> Credentials are compiled into the firmware. Do not publish binaries built with private credentials.

```bash
# 1. Load the ESP-IDF environment (path depends on where you installed it)
. ~/esp/esp-idf/export.sh          # Windows: use the ESP-IDF 6.1 shell installed by the ESP-IDF Installation Manager (EIM)

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

Abbreviated; every init step is logged as `[ OK ]` or `[FAIL] <step>: <error>`.

```
I (...) diag: ==================================================
I (...) diag:  MyPi-Hotspot firmware 0.2.0-debug
I (...) diag:  Chip: esp32p4, 2 core(s), revision v3.1
I (...) diag:  Reset reason: power-on (1)
I (...) diag:  Running partition: ota_0 @ 0x020000 (6144 KB)
I (...) diag: ==================================================
I (...) wifi: Initializing Wi-Fi via ESP-Hosted (ESP32-C5 over SDIO); this may take a few seconds
I (...) display: Pins: SCLK=23 MOSI=5 MISO=4 SD_CS=32 LCD_CS=20 LCD_DC=21 LCD_RESET=26 LCD_BL=22
I (...) display: SPI clock 10 MHz, mode 0, invert=1, order=RGB, 240x320
I (...) display: ST7789 240x320 ready, framebuffer 153600 bytes in PSRAM
I (...) display: Test pattern start
I (...) main: Screen filled navy blue: ESP_OK
I (...) touch: Pins: TP_SDA=7 TP_SCL=8 TP_INT=3 TP_RST=2 addr=0x15
I (...) wifi: Got IP 192.168.1.42 (gw 192.168.1.1, mask 255.255.255.0)
I (...) wifi: Web UI: http://192.168.1.42/  or  http://mypi-hotspot.local/
I (...) diag: HB up=0h00m10s heap=...K(min ...K) psram=...K wifi=connected ip=192.168.1.42 rssi=-55 touch=0 frames=12 flush_err=0
I (...) main: TAP   x=120 y=160 color=0x5A3F
```

---

## Web UI

When Wi-Fi is connected, open `http://mypi-hotspot.local/` (mDNS) or the IP printed in the log.

| URL | Content |
|---|---|
| `/` | Live status page (refreshes every 2 s): uptime, memory, Wi-Fi, display, touch, last touch events, OTA state, log |
| `/api/status` | Same data as JSON |
| `/api/log` | The RAM log buffer as plain text (boot log included, oldest first) |
| `/update` | OTA upload form |

## OTA updates

1. Build (or download from CI) the **app image** `mypi_hotspot.bin` — *not* the merged full-flash image.
2. Open `http://mypi-hotspot.local/update`, choose the file, click upload.
3. The image is streamed into the inactive OTA slot and validated (magic byte, chip ID, app
   descriptor, SHA-256). On success the boot partition is switched and the board reboots.
4. The new image marks itself valid once display/touch init has finished. If it crashes before
   that, the bootloader **rolls back** to the previous slot automatically.

Equivalent from a terminal: `curl --data-binary @build/mypi_hotspot.bin http://mypi-hotspot.local/update`.

## Wi-Fi co-processor (ESP32-C5)

Wi-Fi runs on the on-board ESP32-C5, which must run **ESP-Hosted slave firmware** compatible with
the host component (v2.x, built with v2.13 here). The log prints both versions at startup and warns
on mismatch. If Wi-Fi init fails or the version cannot be read, update the C5 firmware following
the [ESP-Hosted-MCU](https://github.com/espressif/esp-hosted-mcu) instructions. The display and touch
keep working without Wi-Fi.

---

## Troubleshooting

#### Flashing / serial

| Symptom | Likely cause |
|---|---|
| esptool-js: *"Serial data stream stopped: possible serial noise or corruption"* | Baudrate too high for the CH343 bridge — select **115200** before **Connect**, connect without a USB hub, try another data cable |
| Board does not connect | Enter download mode: hold **BOOT**, tap **RESET**, release **BOOT** |
| Boot loop, `chip revision` error from bootloader | Built for rev < 3.0 or with an old IDF — rebuild with IDF ≥ 6.1 after `idf.py fullclean` |
| `partition table` / `invalid header` errors after upgrading from v0.1 | Old layout still in flash — flash the full merged image at `0x0` |

#### Dark or wrong screen

Check the serial log first — each display step is logged.

| Symptom | Likely cause |
|---|---|
| Backlight off | `LCD_BL (GPIO22) -> 1` missing in log, or LCD_BL wire/VCC not connected |
| Backlight on, screen stays black/white, log shows `[ OK ]` | Wrong SCLK/MOSI/LCD_CS/LCD_DC, or LCD_RESET stuck low — check `main/pins.h`; try `CONFIG_MYPI_LCD_SPI_CLOCK_MHZ=1` |
| `SPI color transfer timeout` | SPI/DMA problem — check wiring and lower the SPI clock |
| `Panel ID reads all 0x00/0xFF` warning | Normal if the LCD SDO is not wired to MISO; not an error by itself |
| Test pattern shows inverted colors | Toggle `CONFIG_MYPI_LCD_INVERT_COLOR` |
| Red and blue swapped | Toggle `CONFIG_MYPI_LCD_BGR_ORDER` |

#### Touch / Wi-Fi

| Symptom | Likely cause |
|---|---|
| `CST816D did not ACK at 0x15` | TP_SDA/TP_SCL swapped or not connected, or TP_RST held low — check `main/pins.h` |
| Touch works briefly then stops | Controller went to sleep; auto-sleep is disabled at init, power-cycle the board |
| Touch position mirrored | Panel orientation differs; mirror `x`/`y` in `touch_read()` |
| `AP not found` | Wrong SSID, AP out of range, or AP is 5 GHz-only/hidden — check `CONFIG_MYPI_WIFI_SSID` |
| Repeated `Disconnected: reason ...` with auth errors | Wrong password — check `CONFIG_MYPI_WIFI_PASSWORD` |
| `[FAIL] esp_wifi_init` / version cannot be read | C5 not running compatible ESP-Hosted slave firmware, or SDIO problem |
| `mypi-hotspot.local` does not resolve | mDNS not supported on the client (some Android/Windows setups) — use the IP from the log |
