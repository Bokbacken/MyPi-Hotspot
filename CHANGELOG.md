# Changelog

All notable changes to the MyPi-Hotspot firmware (ESP32-P4 host). Versions match
`PROJECT_VER` in `CMakeLists.txt`. "HW-tested" means the change has run on the real
board; "untested" means it builds cleanly but has not been verified on hardware.
All versions up to and including 0.7.0 have been run on the hardware.

## Unreleased — LCD and web UI refinement (not hardware-tested)
- Add an LCD Wi-Fi character wheel with range tabs, tap/drag navigation, UTF-8-safe
  input, masked-by-default password display, and saving through the STA profile store.
- Split the embedded dashboard into routed Overview, Wi-Fi, Access point, Ethernet,
  Traffic, System / display, and Diagnostics views; preserve the existing API and OTA routes.
- Add a responsive shared navigation/branding header and a GitHub Actions ESP-IDF
  build artifact containing the OTA application image.
- Refine the shared header to a single Hotspot/status row and full `Hotspot` 8N1
  waveform: 18 + 6 pixels on LCD, 52 + 6 CSS pixels on web. Home content and routes
  are unchanged. Approved μ/π artwork is still blocked on the original image files;
  no substitute logo is embedded.
- Keep LCD/web headers dark with white lettering; use antialiased DejaVu Sans
  Bold for the LCD title only, leaving home fields and wheel lettering unchanged.
  LCD Wi-Fi color now reflects STA RSSI: dark red below -80 dBm, red to -70,
  orange to -60, yellow to -40, then green. Disconnected/unknown RSSI is gray;
  web retains its dBm status text.

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
