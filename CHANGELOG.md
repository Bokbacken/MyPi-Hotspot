# Changelog

All notable changes to the MyPi-Hotspot firmware (ESP32-P4 host). Versions match
`PROJECT_VER` in `CMakeLists.txt`. "HW-tested" means the change has run on the real
board; build and hardware validation status are stated for unreleased changes.
All versions up to and including 0.7.0 have been run on the hardware.

## 0.8.2 — LVGL foundation build (LVGL variant HW-tested)
- **HW-tested:** the `sdkconfig.lvgl` variant was flashed via `/update`; the LVGL
  status viewport renders correctly and normal operation works. This also covers the
  0.8.1 header/logo changes. The default variant was not separately flashed.
- First firmware build of PR #9 (LVGL 9.4.0, `mypi_ui/` status model, `ui_lvgl_port`).
  PR #9 could not be built in the Copilot environment (component registry DNS failure).
- Default build keeps `CONFIG_MYPI_UI_LVGL=n`: same UI as 0.8.1 (LVGL is fetched but
  not linked into the image).
- New `sdkconfig.lvgl` overlay enables the experimental LVGL home status viewport:
  `idf.py -B build_lvgl -D SDKCONFIG=build_lvgl/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.lvgl" build`.
- ESP-IDF v6.1: both variants build with 0 warnings. Host tests pass
  (`test_ui_primitives`, `test_web_ui.js`, `test_header_logo.py`, `test_mypi_ui` with real LVGL 9.4.0).
- App image: default 1 054 032 B, LVGL variant 1 324 176 B (+264 KiB, Montserrat font + LVGL core).

## 0.8.1 — Integrate PR #5/#6 into the 0.8.0 UI (builds, not hardware-tested)
- Resolves the overlapping UI branches (PR #5 LCD wheel/routed web views, PR #6
  header/logo) against 0.8.0. LVGL will replace the local rendering
  (`docs/ARCHITECTURE.md`), so only one transitional implementation is kept.
- Kept from 0.8.0: LCD home/menu/scan/password/AP/clients/traffic/Ethernet/system
  views, tap navigation, AP-stop confirmation, `ui_input.h` character wheel,
  single-page web dashboard with `/ui.css` + `/ui.js`, and the host tests.
- Added from PR #6: the approved μ/π logo (`MyPi-transparent.png`) as a white mark in
  the LCD home header (16 px) and in all web headers (32 px), the antialiased DejaVu
  Sans Bold `Hotspot` LCD title, the LCD Wi-Fi icon colored by STA RSSI (dark red
  < -80 dBm, red < -70, orange < -60, yellow < -40, green), the generators
  `tools/render_header_logo.py` / `tools/render_header_font.py`, the font license,
  and `tests/test_header_logo.py`.
- Dropped as duplicates: `char_wheel.c/.h` and its drag-capture touch API (PR #5),
  the routed `/wifi`, `/access-point`, ... web views (PR #5), and PR #6's inline
  BRAND_STYLE/BRAND_HEADER/BRAND_SCRIPT web header. The branches stay in git history.
- Backend, APIs, OTA/rescue touch priority, rotation and wiring diagnostics unchanged.

## 0.8.0 — MyPi interface update (builds, not hardware-tested)
- Branded LCD home and menu views, asynchronous local Wi-Fi scan/connect, UTF-8
  character wheel, client/traffic/Ethernet/system views, and explicit AP-stop
  confirmation.
- Shared offline web theme and generated 8N1 serial waveform (`Hotspot`) for the
  dashboard, both OTA pages, and pins diagnostics.
- Host tests cover serial framing/waveform geometry, touch geometry, wheel and
  credential primitives. ESP-IDF v6.1 build: OK, 0 warnings. Device validation outstanding.
- Build fix: AP profile button label truncated to 24 chars (`-Werror=format-truncation`).
- The private Companion logo asset was inaccessible; the exact merged μ/π mark has
  not been added and must not be considered implemented.

## 0.7.0 — Ethernet emergency mode (HW-tested)
- New `rescue.c/.h`: emergency mode that gives web access over Ethernet even when the
  ESP32-C5 / ESP-Hosted link is dead.
- Manual trigger: swipe down (>= 60 px, coordinate based so it respects 180° rotation)
  within 10 s after boot. The status screen shows a countdown.
- Automatic triggers: `esp_wifi_init()`/Wi-Fi start failure, Wi-Fi init > 30 s,
  `ESP_HOSTED_EVENT_TRANSPORT_FAILURE`, or 3 failed C5 health checks in a row
  (`esp_hosted_get_coprocessor_fwversion()` every 60 s). Paused during OTA.
- LCD view with 5 buttons: DHCP (preselected) or static `<AP subnet>.32/24`
  (gateway `.1`) for each of the 4 AP subnets.
- AP netif is taken down immediately (no RPC); the AP radio is stopped in a
  background task. In LAN mode the L2 bridge is stopped and Ethernet gets its own netif.
- `/api/status` exposes `eth.rescue`; the status page shows an "EMERGENCY MODE" row.
- Runtime only: nothing is written to NVS, a reboot returns to normal operation.

## 0.6.3 — OTA progress view and buffer tuning (HW-tested)
- Full-screen OTA progress on the LCD (phase, bar, %, KB) for P4 and C5 updates;
  touch is ignored while it is shown.
- `/update` and `/c5update` poll `/api/status` after upload and show the phase/errors.
- P4: `ESP_HOSTED_HOST_SDIO_RX_STAGING_SLOTS=4`.
- C5 slave: SDIO TX/RX queues 40, A-MPDU TX/RX, BA window 32.

## 0.6.2 — Display rotation (HW-tested)
- Runtime 180° screen and touch rotation (NVS `mypi/lcd_rot`, `POST /api/display`,
  checkbox in the web UI). Confirmed on hardware.
- `/api/reboot` returns JSON. AP channel is only re-read when the STA channel changes.

## 0.6.1 — OTA robustness (HW-tested)
- TCP recvmbox sized for the 128K window, 45 s stall timeout, % progress on upload pages.

## 0.6.0 — ESP-Hosted 3.0.9 (HW-tested)
- Host on `espressif/esp_hosted` 3.0.9 (v3 Kconfig names). Requires the 3.0.9 C5 slave
  (`c5_slave/`). v2 and v3 RPC are incompatible.
- SDIO 4-bit 40 MHz. The P4 no longer reboots on C5 link failure.
- Web server, Ethernet and mDNS start before Wi-Fi.

## 0.5.5 — C5 OTA and radio limits (HW-tested)
- `/c5update` + `POST /api/c5update`: update the C5 slave over ESP-Hosted.
- `coproc` object in `/api/status` (chip, slave FW, host version, mismatch, MACs, SDIO pins).
- Bandwidth clamped to 40 MHz and 11ax disabled (ESP-IDF #19238), with NVS migration.
- `c5_slave/`: ESP-Hosted 3.0.9 slave project with a 2.x-compatible partition layout.

## 0.5.4 — Radio settings (HW-tested)
- Protocol bitmask with checkboxes, band lock, TX power, beacon interval / DTIM.
- Deeper Wi-Fi/SDIO/TCP buffers on the P4.

## 0.5.3 — Runtime radio configuration (HW-tested)
- Bandwidth/protocol at runtime, negotiated PHY info, STA TX retry.
- Baseline throughput (ch36 STA, ch1 AP, 20 MHz/ac): STA RX 16.32 / TX 20.76 Mbit/s,
  AP RX 20.61 / TX 16.26 Mbit/s (peak).

## 0.5.2 — Ethernet performance (HW-tested)
- Backpressure ETH→host, deeper EMAC/hosted queues, TX drop counters.

## 0.5.1 — Statistics and LCD status (HW-tested)
- Peak throughput (Mbit/s, 2 decimals), PSRAM traffic log (CSV), LCD status screen.
- Dropped the dual-band assumption: C5 AP and STA always share one channel.

## 0.5.0 — Hotspot (HW-tested)
- Wi-Fi APSTA via ESP-Hosted with NAPT; 10 STA and 4 AP profiles in NVS.
- Ethernet (EMAC + IP101): LAN = L2 bridge to the AP subnet, or WAN = DHCP uplink.
- Per-interface traffic counters, iperf2 TCP server on port 5001, web UI + JSON API.

## 0.4.0 — Display performance (HW-tested)
- LCD SPI 40 MHz, touch I2C 400 kHz, ping-pong DMA flush.
- Quiet logs by default; runtime debug mode (NVS) re-enables wiring diagnostics.

## 0.3.x — ESP-IDF v6.1 and wiring diagnostics (HW-tested)
- Moved to ESP-IDF v6.1. Boot pin check and `/pins` page, touch INT wait, I2C bus recovery.
- Documented: power the LCD module from the 3V3 pin only.

## 0.2.0-debug — Foundations (HW-tested)
- Diagnostics/heartbeat, RAM log buffer (`/api/log`), Wi-Fi STA over the C5,
  web status page, OTA upload with rollback, custom partition table.

## Hello world
- ST7789 display + CST816D touch on the ESP32-P4-NANO; GPIOs centralized in `pins.h`.
