# MyPi-Hotspot

Firmware for a small Wi-Fi hotspot device built on the **Waveshare ESP32-P4-NANO** with a
**Waveshare 2" capacitive touch LCD**.

Version history: [CHANGELOG.md](CHANGELOG.md) · Project status, decisions and test checklist: [docs/PROJECT_STATUS.md](docs/PROJECT_STATUS.md) ·
[Next steps and UI conventions](docs/NEXT_STEPS.md) · [MyPi platform direction](docs/ARCHITECTURE.md)

Current firmware release: **0.8.1** — the approved MyPi μ/π logo in the LCD and web headers, a new **web theme**, an **Ethernet emergency mode** (see [Emergency mode](#emergency-mode-ethernet-rescue-v070)), an interactive **LCD menu** with a Wi-Fi character-wheel flow and OTA progress view, Wi-Fi uplink + own access point
(APSTA with NAT), Ethernet as WAN or LAN, STA/AP profiles, traffic stats with peak rates, a CSV traffic log, an iperf throughput test,
a responsive web dashboard and over-the-air (OTA) updates (see [Wi-Fi, AP & Ethernet](#wi-fi-ap--ethernet)). v0.8.1 (logo/header on top of the v0.8.0 UI) builds cleanly but is not yet hardware-tested. Diagnostics
are quiet by default and can be switched on at any time with **debug mode** (see [Debug mode](#debug-mode)).

| Action | Result |
|---|---|
| Boot | Boot banner on serial, then the LCD status home screen (debug mode: wiring check + color test pattern first) |
| Tap **OPEN MENU** | Open Wi-Fi, AP, clients, traffic, Ethernet, or system/display views |
| Wi-Fi uplink → scan → network | Saved profiles connect directly; a new network opens the local password character wheel, then saves and requests connection |
| AP → **STOP AP...** | Show an explicit disconnect confirmation; Back/navigation leaves the AP running |
| Swipe down during first 10 s | Enter Ethernet emergency mode (>= 60 px, rotation-corrected coordinates) |
| Any touch event | Shown in the web page touch history; in debug mode also logged on serial at 115200 baud (`DOWN`, `DRAG`, `MOVE`, `UP`, `TAP`) |
| Every 60 s (5 s in debug mode) | Heartbeat line: uptime, heap/PSRAM, Wi-Fi state/IP/RSSI, touch count, frames flushed, OTA state |
| Wi-Fi connected | Web UI at `http://mypi-hotspot.local/` (or the management address shown on the LCD): dashboard, settings, diagnostics, OTA |

The current firmware uses no LVGL — just `esp_lcd` (built into ESP-IDF), raw I2C,
and ESP-Hosted for Wi-Fi. [LVGL is the chosen future MyPi UI direction](docs/ARCHITECTURE.md),
not implemented in this firmware.
A failing display or touch controller is logged but does **not** stop Wi-Fi/web, so you can always
read the diagnostics remotely.

The display and web divider use generated 8N1 serial waveforms (`H` on the LCD and
`Hotspot` on the web). The shared web styles/scripts are local embedded assets; see
[local UI navigation, asset maintenance, validation, and future ideas](docs/NEXT_STEPS.md).
The exact merged μ/π logo asset is not present in this checkout; the private Companion
asset could not be accessed for this update, so no substitute logo has been used.

> v0.4.0 raises the LCD SPI clock to 40 MHz and the touch I2C clock to 400 kHz. If the screen shows
> noise or the touch error counter climbs, lower them in menuconfig (20 MHz / 100 kHz).

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

> ⚠️ **Power the module from the 3V3 pin only (confirmed).** Earlier both VCC (fed with 5 V) and
> 3V3 were connected: the screen stayed dark and the touch I2C bus timed out constantly. With VCC
> disconnected and only 3V3 wired, the display, touch dots and drag lines all work. The module has an on-board regulator
> and a level translator whose pull-ups follow the module supply, so feeding VCC *and* the 3V3 pin
> puts two regulators in parallel and can shift the logic levels away from the ESP32-P4's 3.3 V.
> Never let the module pull the P4 pins up to 5 V — they are not 5 V tolerant.

The display is SPI; only the touch controller (`TP_*`) is I2C. The SPI bus is shared between the
LCD and the module's microSD slot (`SD_CS`), which is why `MISO` exists.

| Module pin | Bus | Function | ESP32-P4 GPIO | Status |
|---|---|---|---|---|
| VCC | — | Supply input (3.3 V/5 V) to the module's on-board regulator | **leave unconnected** | confirmed |
| 3V3 | — | Module 3.3 V rail — the only supply wire | 3V3 | confirmed |
| GND | — | Ground | GND | — |
| SCLK | SPI | SPI clock | GPIO23 | confirmed |
| MOSI | SPI | Data to LCD / SD | GPIO5 | confirmed |
| MISO | SPI | Data from SD card (LCD is write-only) | GPIO4 | wired, SD not used yet |
| SD_CS | SPI | microSD chip select (held high) | GPIO32 | wired, SD not used yet |
| LCD_CS | SPI | LCD chip select | GPIO20 | confirmed |
| LCD_DC | — | LCD data / command | GPIO21 | confirmed |
| LCD_RESET | — | LCD hardware reset (active low) | GPIO26 | confirmed |
| LCD_BL | — | Backlight (high = on) | GPIO22 | confirmed |
| TP_SDA | I2C | Touch data | GPIO7 | confirmed |
| TP_SCL | I2C | Touch clock | GPIO8 | confirmed |
| TP_INT | — | Touch interrupt (active low, wakes the touch loop) | GPIO3 | confirmed |
| TP_RST | — | Touch reset (active low) | GPIO2 | confirmed |

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
├── main/
│   ├── CMakeLists.txt
│   ├── Kconfig.projbuild Project options (menuconfig → "MyPi-Hotspot")
│   ├── idf_component.yml Managed components: esp_wifi_remote, esp_hosted, mdns, iperf
│   ├── main.c            app_main: init, status screen start, touch loop, OTA rollback confirm
│   ├── pins.h            All GPIO assignments (module pin labels) — VERIFY BEFORE FLASHING
│   ├── display.h/.c      ST7789 via esp_lcd, 240×320 RGB565 framebuffer in PSRAM, ping-pong DMA flush, test pattern, ID reads
│   ├── display_font.h/.c Classic 5×7 ASCII font (Adafruit GFX glcdfont) + up/down arrows
│   ├── display_ui.h/.c   LCD status, navigable menus, UTF-8 password wheel, rescue and OTA views
│   ├── ui_layout.h       LCD semantic colors, menu geometry and touch hit helpers
│   ├── ui_serial.h       Shared 8N1 byte framing and waveform geometry
│   ├── ui_input.h        UTF-8 wheel encoding and Wi-Fi passphrase length helpers
│   ├── header_logo.h     Generated μ/π logo: 16 px LCD alpha map + 32 px web PNG (tools/render_header_logo.py)
│   ├── header_font.h     Generated antialiased LCD `Hotspot` title (tools/render_header_font.py)
│   ├── rescue.h/.c       Emergency mode (Ethernet rescue): triggers, option state, worker task
│   ├── touch.h/.c        CST816D polling over I2C (new i2c_master driver)
│   ├── diag.h/.c         Boot banner, shared status struct, heartbeat task, debug mode (NVS)
│   ├── logbuf.h/.c       RAM ring buffer mirroring all log output (served at /api/log)
│   ├── pintest.h/.c      Boot-time wiring check and manual pin control (served at /pins)
│   ├── wifi.h/.c         Wi-Fi APSTA over ESP-Hosted: STA auto-select/reconnect, AP + clients, scan, mDNS
│   ├── profiles.h/.c     STA (10) and AP (4) profiles in NVS
│   ├── net.h/.c          NAPT, DHCP server setup, traffic counters + peaks, traffic log (PSRAM), iperf server
│   ├── eth.h/.c          Ethernet (EMAC + IP101): LAN (L2 bridge to the AP) or WAN (uplink)
│   ├── web.h/.c          HTTP server: dashboard, JSON API, log, OTA upload, network API
│   └── web_ui.css/.js    Locally embedded shared web theme and generated serial waveform
└── tests/                Host-runnable framing, geometry, and credential primitive tests
```

The framebuffer (150 KB) lives in PSRAM. Only the changed ("dirty") rectangle is sent to the panel,
copied through a small internal DMA buffer in 40-row bands.

---

## Flash a prebuilt binary (no ESP-IDF needed)

Every push and pull request is built by GitHub Actions ([`.github/workflows/build.yml`](.github/workflows/build.yml)).
The build artifact contains the OTA application image `mypi_hotspot.bin` and the merged full-flash image.

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

For an existing installation, upload the artifact's **`mypi_hotspot.bin`** through the device's
**Update** page. Do not upload the merged full-flash image through OTA.

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
| `CONFIG_MYPI_WIFI_SSID` / `CONFIG_MYPI_WIFI_PASSWORD` | `TOFTA` / `bokbacken` | Station fallback when no STA profile is saved |
| `CONFIG_MYPI_AP_SSID` / `CONFIG_MYPI_AP_PASSWORD` | `MyPi-Hotspot` / `mypi1234` | Default access point (192.168.4.1) |
| `CONFIG_MYPI_AP_MAX_CONN` / `CONFIG_MYPI_AP_AUTOSTART` | 10 / y | AP client limit; start the AP at boot |
| `CONFIG_MYPI_ETH_ENABLE` / `_PHY_ADDR` / `_PHY_RST_GPIO` | y / -1 / 51 | Ethernet (menu **Ethernet**) |
| `CONFIG_MYPI_WIFI_BACKOFF_MAX_S` | 60 | Reconnect backoff 1 s → max |
| `CONFIG_MYPI_HOSTNAME` | `mypi-hotspot` | DHCP + mDNS name (`mypi-hotspot.local`) |
| `CONFIG_MYPI_LCD_SPI_CLOCK_MHZ` | 40 | ~30 ms per full frame; lower to 20 if you see noise (ST7789 spec max ~62) |
| `CONFIG_MYPI_LCD_INVERT_COLOR` | y | ST7789 panels usually need inversion |
| `CONFIG_MYPI_LCD_BGR_ORDER` | n | Swap if red and blue are exchanged |
| `CONFIG_MYPI_TOUCH_I2C_KHZ` | 400 | Fast-mode, max for CST816D and the ES8311 sharing the bus; lower to 100 if touch errors climb |
| `CONFIG_MYPI_DEBUG_MODE_DEFAULT` | n | Debug mode before anything is saved; normally toggled on the web page |
| `CONFIG_MYPI_HEARTBEAT_INTERVAL_S` | 60 | Seconds between heartbeat lines in normal mode (5 s in debug mode) |
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

Abbreviated, normal mode. Failures are always logged (`[FAIL] <step>: <error>`); the `[ OK ]` per step,
pin lists and touch events only appear in debug mode.

```
I (...) diag: ==================================================
I (...) diag:  MyPi-Hotspot firmware 0.5.1
I (...) diag:  Chip: esp32p4, 2 core(s), revision v3.1
I (...) diag:  Reset reason: power-on (1)
I (...) diag:  Running partition: ota_0 @ 0x020000 (6144 KB)
I (...) diag: ==================================================
I (...) diag: Debug mode off (Kconfig default) - toggle on the web status page
I (...) wifi: Initializing Wi-Fi via ESP-Hosted (ESP32-C5 over SDIO); this may take a few seconds
I (...) display: SPI clock 40 MHz, mode 0, invert=1, order=RGB, 240x320
I (...) display: ST7789 240x320 ready, SPI 40 MHz, framebuffer 153600 bytes in PSRAM
I (...) touch: CST816 ready: ID 0x.. FW 0x.., I2C 400 kHz, TP_INT interrupt
I (...) wifi: Got IP 192.168.1.42 (gw 192.168.1.1, mask 255.255.255.0)
I (...) wifi: Web UI: http://192.168.1.42/  or  http://mypi-hotspot.local/
I (...) diag: HB up=0h01m00s heap=...K(min ...K) psram=...K wifi=connected ip=192.168.1.42 rssi=-55 touch=0 frames=2 flush_err=0
```

### Debug mode

The diagnostics from the bring-up are kept, but are off by default. Turn them on with the
**Debug mode → Turn on** button on the web status page (or `curl -X POST "http://mypi-hotspot.local/api/debug?on=1"`).
The setting is stored in NVS and survives reboots and OTA updates.

| Takes effect | What debug mode adds |
|---|---|
| Immediately | DEBUG-level logs from the app: every init step `[ OK ]`, pin lists, I2C line levels, touch `DOWN`/`DRAG`/`MOVE`/`UP`/`TAP` |
| Immediately | Heartbeat every 5 s instead of 60 s |
| After a reboot | Boot-time wiring check (shown at `/pins`), panel ID read, I2C bus scan, color test pattern |

Always on, regardless of debug mode: the web status page, `/api/log`, OTA, error/warning logs, the
touch/flush error counters and manual pin control at `/pins`.

---

## Web UI

When Wi-Fi is connected, open `http://mypi-hotspot.local/` (mDNS) or the IP printed in the log.

| URL | Content |
|---|---|
| `/` | Live status page (refreshes every 2 s): uptime, memory, Wi-Fi, display, touch, last touch events, OTA state, log |
| `/api/status` | Same data as JSON |
| `/api/log` | The RAM log buffer as plain text (boot log included, oldest first) |
| `/update` | OTA upload form |
| Network API | STA/AP profiles, scan, AP clients, Ethernet mode, traffic stats, iperf — see [Network API](#network-api) |
| `/api/debug?on=1\|0` (POST) | Switch debug mode on/off (saved in NVS) |
| `/pins` | Wiring diagnostics: boot-time pin check (debug mode only) (pull test + short detection) and manual HIGH/LOW/input control per pin for multimeter checks (detaches the pin until reboot) |

## OTA updates

1. Build (or download from CI) the **app image** `mypi_hotspot.bin` — *not* the merged full-flash image.
2. Open `http://mypi-hotspot.local/update`, choose the file, click upload.
3. The image is streamed into the inactive OTA slot and validated (magic byte, chip ID, app
   descriptor, SHA-256). On success the boot partition is switched and the board reboots.
4. The new image marks itself valid once display/touch init has finished. If it crashes before
   that, the bootloader **rolls back** to the previous slot automatically.

Equivalent from a terminal: `curl --data-binary @build/mypi_hotspot.bin http://mypi-hotspot.local/update`.

**Progress on the LCD (v0.6.3).** While a P4 (`/update`) or C5 (`/c5update`) update runs, the LCD shows a
full-screen view refreshed at 5 Hz: *Firmware update (P4)* / *(C5)*, the phase (*Receiving + writing flash*,
*Verifying image*, *Upload complete - verified*, *Done - rebooting* or *FAILED: <reason>*), a progress bar with
percentage and KB done / total. Touch is ignored meanwhile. A failure stays on screen for about 10 s, then
the status screen returns; on success the board reboots 2 s after *Done - rebooting*. The same state is in
`/api/status` as `ota` (`active`, `target`, `phase`, `pct`, `bytes_done`, `bytes_total`, `err`), and the
upload pages poll it after 100 % to show verifying / done / failed.

## Wi-Fi co-processor (ESP32-C5)

Wi-Fi runs on the on-board ESP32-C5, which must run **ESP-Hosted slave firmware** compatible with
the host component (v3.x since v0.6.0, pinned to 3.0.9; v0.5.x used 2.13). 2.x and 3.x do not
talk to each other. The log prints both versions at startup and warns on mismatch. If Wi-Fi init
fails or the version cannot be read, update the C5 firmware (see `c5_slave/`). The display, touch,
Ethernet and P4 `/update` keep working without Wi-Fi; the board never reboots on a C5 link failure.

Safest upgrade order from v0.5.x (C5 on 2.x):
1. P4 on v0.5.5 (host 2.13) – flash via `/update` or USB.
2. C5 to 3.0.9 via `/c5update` (`c5-slave-esp-hosted-v3.0.9.bin`). Wi-Fi stops working afterwards.
3. P4 v0.6.0 via `/update` over **Ethernet** (or USB).

If P4 v0.6.0 is flashed first, flash the C5 over USB (`c5-slave-esp-hosted-v3.0.9-merged.bin`).

**Buffer tuning (v0.6.3).** The P4 host uses 4 SDIO RX staging slots (default 2); the SDIO clock stays at
40 MHz. The C5 slave defaults (`c5_slave/sdkconfig.defaults.esp32c5`) raise the Wi-Fi buffers to static RX 16,
dynamic RX 64, dynamic TX 64, A-MPDU TX/RX with BA window 32, and the C5 SDIO TX/RX queues to 40. This build is
published as `c5-slave-esp-hosted-v3.0.9-tuned.bin` (OTA via `/c5update`) and `...-tuned-merged.bin` (USB).
It still reports slave version 3.0.9, so it stays compatible with the 3.0.9 host.

## Wi-Fi, AP & Ethernet

Since v0.5.0 the device is a small router: the ESP32-C5 runs **APSTA** (station uplink + our own
access point on the same radio) and the P4 routes between the interfaces with lwIP **NAPT**.

```
            internet
               │
   ┌───────────┴───────────┐
   │ uplink: Wi-Fi STA      │  (or Ethernet in WAN mode — preferred while it has an IP)
   │                        │
   │  ESP32-P4 lwIP + NAPT  │
   │                        │
   │ AP 192.168.4.1/24      │  Wi-Fi clients (DHCP, NAT to the uplink)
   │   ║ L2 bridge          │
   │ ETH-LAN (no own IP)    │  wired clients, same 192.168.4.0/24 subnet
   └────────────────────────┘
```

In **LAN mode** Ethernet is bridged at layer 2 to the access point (same idea as Waveshare's
`08_eth2ap` example): Ethernet frames are forwarded raw to the Wi-Fi AP and back, so wired and
wireless clients are on **one subnet** (the AP profile's subnet, default `192.168.4.0/24`), get their
address from the AP's DHCP server, can reach each other directly, and share the uplink through the
AP's NAT. The Ethernet port has no IP of its own; the device is reached at `192.168.4.1` from both
sides. The bridge forwards only while the AP is running (it starts/stops with the AP). A small MAC
learning table keeps unicast traffic on the side where the destination lives.

### Defaults and subnets

| Interface | Default | Notes |
|---|---|---|
| STA (uplink) | Saved STA profiles (10 slots); fallback `CONFIG_MYPI_WIFI_SSID` / `_PASSWORD` (`TOFTA` / `bokbacken`) | Auto-connect picks the visible saved network with the best RSSI; DHCP or static IP per profile |
| AP | `MyPi-Hotspot` / `mypi1234`, `192.168.4.1/24`, NAT on | Started at boot (`CONFIG_MYPI_AP_AUTOSTART`) or with the AP profile flagged *start at boot* |
| AP profile subnets | `192.168.4.0/24`, `192.168.42.0/24`, `10.11.0.0/24`, `172.16.4.0/24` | AP is always `.1/24` with a DHCP server; DNS `8.8.8.8` is offered to clients |
| Ethernet LAN (default) | L2 bridge to the AP — wired clients share the AP subnet (`192.168.4.0/24` by default) | Connect a PC/switch to the RJ45 port; needs the AP running |
| Ethernet WAN | DHCP client | Becomes the default route (uplink) while it has an IP; AP clients are NATed out over Ethernet |

| Limit | Value |
|---|---|
| STA profiles / AP profiles | 10 / 4 (NVS namespaces `staprof` / `approf`) |
| AP clients | max 10 (`CONFIG_MYPI_AP_MAX_CONN`, ESP-Hosted limit) |
| DHCP leases per server | 24 (`CONFIG_LWIP_DHCPS_MAX_STATION_NUM`) |
| Password | empty (open) or 8–64 characters (WPA2-PSK) |
| Ethernet mode change | stored in NVS (`mypi`/`ethmode`), applied **after a reboot** |

Profiles are edited on the web page. Passwords are never sent back by the API (`has_pass` only); saving
with an empty password field keeps the stored password for the same SSID, tick *open* to remove it.

Kconfig (menuconfig → MyPi-Hotspot): `CONFIG_MYPI_AP_SSID`, `CONFIG_MYPI_AP_PASSWORD`,
`CONFIG_MYPI_AP_MAX_CONN`, `CONFIG_MYPI_AP_AUTOSTART`, and under **Ethernet** `CONFIG_MYPI_ETH_ENABLE`,
`CONFIG_MYPI_ETH_PHY_ADDR` (-1 = auto-detect), `CONFIG_MYPI_ETH_PHY_RST_GPIO` (51 on the NANO).

### Network API

All POST endpoints take a small JSON body and answer `{"ok":true,"msg":...}` or HTTP 400 `{"ok":false,"error":...}`.

| Endpoint | Body / response |
|---|---|
| `GET /api/profiles` | STA slots `{slot,valid,ssid,has_pass,dhcp,ip,mask,gw,dns}`, `active` slot |
| `POST /api/profile/save` | `{slot,ssid,pass,clear_pass,dhcp,ip,mask,gw,dns}` |
| `POST /api/profile/del` | `{slot}` |
| `POST /api/connect` | `{slot}` (`-1` = automatic, best RSSI) |
| `POST /api/disconnect` | stays disconnected until the next connect |
| `GET /api/scan` | `{networks:[{ssid,rssi,channel,auth,profile}]}` (blocks 2–4 s) |
| `GET /api/approfiles` | AP slots `{slot,valid,ssid,has_pass,subnet,nat,on_boot}` + `subnets` |
| `POST /api/approfile/save` | `{slot,ssid,pass,clear_pass,subnet,nat,on_boot}` |
| `POST /api/approfile/del` | `{slot}` |
| `POST /api/ap/start` / `POST /api/ap/stop` | `{slot}` (`-1` = Kconfig default AP) |
| `GET /api/apstatus` | AP state + `clients:[{mac,ip,rssi}]` |
| `GET /api/eth` / `POST /api/eth` | link, speed, IP (WAN), active/saved mode, `bridged`, `bridge_drops` (LAN) / `{mode:"lan"\|"wan"}` |
| `GET /api/radio` / `POST /api/radio` | active/saved width + protocols, what the C5 reports for STA/AP, upstream AP PHY / `{sta_bw:"auto\|20\|40\|80", ap_bw:"20\|40\|80", sta_proto, ap_proto:"default\|n\|ac\|ax"}` (reboot to apply) |
| `GET /api/netstats` | per interface (`sta`, `ap`, `eth`): bytes, packets, `rx_mbps`/`tx_mbps` (Mbit/s, 2 decimals, 1 s window), `rx_mbps_peak`/`tx_mbps_peak`; plus `log_running`, `log_ever`, `log_samples`, `log_max` |
| `POST /api/netstats/reset` | clear the peak rates → `{"ok":true}` |
| `POST /api/netstats/log/start` / `stop` | start (clears old samples) / stop the traffic log → `{"ok":true,"running":…,"samples":N}` |
| `GET /api/netstats/log` | traffic log as CSV download (`hotspot_log.csv`): `ts,sta_rx_mbps,sta_tx_mbps,ap_rx_mbps,ap_tx_mbps,eth_rx_mbps,eth_tx_mbps` (Mbit/s, 2 decimals) |
| `POST /api/iperf/start` / `stop`, `GET /api/iperf` | `{window_s}` / iperf server state and results |

`/api/status` now also contains `uplink` (`sta`, `eth` or `none`), `sta_profile`, an `ap` object and an `eth` object.

### Testing throughput

**1. Device ↔ client (iperf).** On the web page, under *Throughput test*, click **Arm** (default
window 300 s). The device runs an iperf2-compatible **TCP server on port 5001** and re-listens after each
test until the window expires. From a PC (iperf **2**, not iperf3):

```bash
iperf -c 192.168.4.1 -t 10 -i 1     # PC on the AP or on Ethernet LAN (bridged, same subnet)
iperf -c <STA IP> -t 10 -i 1        # PC on the upstream network (tests the STA link)
```

The page shows the live and average Mbit/s; the *Network traffic* table shows per-interface rates.
This measures the radio/cable link to the P4, not the router path.

**2. NAT path (the real hotspot use case).** Connect a phone or laptop to the AP (or a PC to Ethernet LAN)
and run an internet speed test (e.g. fast.com / speedtest.net). Watch *Network traffic*: the AP RX/TX
should mirror the STA TX/RX. The result is limited by the slowest of: uplink signal, shared radio airtime
(see below), SDIO to the C5, and NAPT forwarding on the P4.

**3. Peaks and the traffic log (v0.5.1).** The *Network traffic* table also shows the highest 1 s rate
per interface (**Reset peaks** clears them). **Start log** records one sample per second for every interface
into a 3600-sample ring buffer in PSRAM (1 hour; older samples are overwritten); **Download CSV** saves it.
Start the log, run a speed test or iperf, stop the log and compare the STA and AP columns to see where the
ceiling is.

### LCD status screen (v0.5.1)

After boot the panel shows a status screen, refreshed every 2 s: title (Wi-Fi icon when the AP runs),
uplink IP (green, or red *No uplink*), uplink SSID + channel, AP clients + channel and SSID, Ethernet state,
uplink throughput (live in cyan, peak in yellow, Mbit/s) and the traffic log state once it has been used.
While the status screen runs, touch drawing is disabled (touch events still appear in the web UI).

### Emergency mode (Ethernet rescue, v0.7.0)

Ethernet-only access when Wi-Fi or the ESP32-C5 is unusable. Runtime only - a
reboot always returns to normal operation (nothing is stored in NVS).

**Entering it**

* **Manually:** swipe down on the LCD (at least 60 px, mostly vertical) within
  10 s of power-on. The status screen shows a countdown
  `Swipe down: emergency mode (Ns)` while the window is open. The swipe is
  detected from the rotation-corrected touch coordinates, so it works with the
  180-degree mounting too.
* **Automatically** when the C5 fails completely:
  * `esp_wifi_init()` / Wi-Fi start fails, or `esp_wifi_init()` has not returned
    after 30 s;
  * ESP-Hosted posts `ESP_HOSTED_EVENT_TRANSPORT_FAILURE` (SDIO slave
    unresponsive);
  * 3 consecutive failed RPC health checks (`esp_hosted_get_coprocessor_fwversion`
    every 60 s).
  Checks are paused while the LCD shows a P4/C5 OTA update.

**What happens**

1. The Wi-Fi AP interface is taken down at once (no RPC, so it works with a dead
   C5) and the AP radio is stopped in the background. NAPT and the LAN bridge stop.
2. Ethernet gets its own IP stack: in LAN (bridge) mode a netif is created at
   runtime; in WAN mode the existing one is reused. The saved Ethernet mode is not
   changed.
3. The LCD shows 5 buttons: **DHCP** (preselected; the obtained IP is shown in
   large text) and a static `<AP subnet>.32/24` for each of the 4 AP subnets
   (`AP_SUBNET_BASE`, e.g. `192.168.4.32`, gateway `.1`). Tap another button to
   switch at any time.
4. The web UI is reachable at `http://<that IP>/`; the status page shows an
   `EMERGENCY MODE` row and `/api/status` has `eth.rescue`. STA profiles, C5
   update (`/c5update`), P4 update (`/update`) and reboot work as usual. Starting
   the AP is blocked until reboot (it would clash with the static subnet).

For a laptop connected directly with a cable: choose e.g. `192.168.4.32` on the
LCD and give the laptop a static `192.168.4.x/24` (not .1 or .32).

### Known limitations (read this)

- **APSTA fallback.** APSTA on the ESP32-C5 through ESP-Hosted (esp_wifi_remote) runs on the board
  (all builds up to v0.7.0 hardware-tested). If a C5 firmware ever rejects APSTA, the device falls back
  to STA only and the page shows *AP unavailable (APSTA rejected)*.
- **One radio, one channel.** STA and AP share the C5 radio, so the AP must use the STA's channel and
  airtime is shared: NAT throughput over Wi-Fi-in/Wi-Fi-out is roughly half of the single-link rate.
  When the STA moves to another network/channel, the AP follows, and AP clients may disconnect briefly.
  Channel width and protocols are runtime settings (web page section *Radio*, `GET/POST /api/radio`,
  stored in NVS, applied after a reboot). Defaults: STA width auto / driver-default protocols, AP 20 MHz /
  driver-default protocols. The page also shows what the C5 reports for STA and AP and the upstream AP's
  PHY capabilities, so combinations can be compared without reflashing.
- NAPT is enabled per interface with the lwIP call `ip_napt_enable_netif()` (instead of
  `esp_netif_napt_enable()`, which only allows one NAT interface). Wired LAN clients are NATed through the AP
  netif because Ethernet is bridged to it.
- **LAN bridge throughput** is bounded by the Wi-Fi side (shared airtime + SDIO to the C5). Ethernet → Wi-Fi
  frames are retried briefly when the ESP-Hosted queue is full and then dropped (`bridge_drops`); TCP backs off.
  If the AP is stopped (or the C5 fails), wired LAN clients get no DHCP and cannot reach the device —
  switch Ethernet to WAN mode or start the AP.
- Ethernet uses the internal EMAC + generic PHY driver (IP101 on the NANO); mode changes need a reboot.
- 802.11 throughput tuning (larger lwIP/Wi-Fi buffers, BA windows) is in `sdkconfig.defaults`;
  values on the C5 side are fixed by its ESP-Hosted slave firmware.

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
| Noise, shifted pixels or wrong colors after updating to v0.4.0 | SPI clock too high for the wiring — set `CONFIG_MYPI_LCD_SPI_CLOCK_MHZ=20` |
| Backlight on, screen dark **and** touch I2C timeouts | Module powered through both VCC (5 V) and 3V3 — disconnect VCC, use 3V3 only (see [Wiring](#wiring-display-module--esp32-p4-nano)) |
| Backlight off | `LCD_BL (GPIO22) -> 1` missing in log, or LCD_BL wire/VCC not connected |
| Backlight on, screen stays black/white, log shows `[ OK ]` | Wrong SCLK/MOSI/LCD_CS/LCD_DC, or LCD_RESET stuck low — check `main/pins.h`; try `CONFIG_MYPI_LCD_SPI_CLOCK_MHZ=1` |
| `SPI color transfer timeout` | SPI/DMA problem — check wiring and lower the SPI clock |
| `LCD ID read: skipped` on the web page | Normal — the panel ID is only read in debug mode |
| `Panel ID reads all 0x00/0xFF` warning | Expected on the Waveshare 2" module: MISO serves the TF card, the panel is write-only. Not an error by itself |
| `/pins`: every pin except LCD_BL "held HIGH" | Expected — the module's level translator pulls all signal lines up to the module supply. Feed the module from one supply pin only |
| `/pins`: a pin is "floating" | That wire probably does not reach the module |
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
