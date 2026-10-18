# Changelog

All notable changes to the MyPi-Hotspot firmware (ESP32-P4 host). Versions match
`PROJECT_VER` in `CMakeLists.txt`. "HW-tested" means the change has run on the real
board; build and hardware validation status are stated for unreleased changes.
All versions up to and including 0.7.0 have been run on the hardware.

## 0.9.5 — 2026-10-18 (builds, not hardware-tested)

- Optimize page, Wi-Fi radio grid: two new dropdowns, both need a reboot.
  - 5 GHz channel width (`bw_5g`): HT40 (default) / HT20. One value for STA and AP (same radio),
    passed to `apply_bw()` via `opt_bw_5g()`. 2.4 GHz stays locked to HT20. The radio blob's
    `sta_bw` / `ap_bw` stay ignored. The status shows the active width and the current STA link width.
  - SDIO clock (`sdio_mhz`): 40 MHz (default) / 50 MHz. The stored value is used again at boot,
    with the existing trial / auto-fallback: if the C5 fails at 50 MHz, the key is rewritten to 40
    and the page shows "fell back from 50 MHz".
- 50 MHz is now really used: IDF v6.1 capped SDIO cards at 40 MHz in `sdmmc_io_enable_hs_mode()`.
  New `main/sdio_clk.c` wraps it (`-Wl,--wrap=sdmmc_io_enable_hs_mode`) and lifts the card limit
  to the configured host clock (max 50 MHz). The real bus clock is logged at boot and returned by
  `GET /api/opt` as `sdio_real_khz`.
- 50 MHz signal integrity on the P4 <-> C5 wiring is untested; the auto-fallback protects the boot.

## 0.9.4 — 2026-10-18 (builds, not hardware-tested)

- The TCP / LwIP / NAT settings are back on the Optimize page, in a new section "TCP / LwIP / NAT"
  below the radio settings (dropdowns only). The v0.9.1/v0.9.2 "no improvement" hardware results
  were invalid: the test client was connected to a different AP than the MyPi device.
  - TCP window (`tcp_wnd_kb`): 32 / 64 / 128 KB, default 32, reboot required. Now really applied:
    `main/lwip_tune.h` replaces `CONFIG_LWIP_TCP_WND_DEFAULT` inside lwIP with the variable
    `mypi_tcp_wnd`, set by `opt_config_load()` before the tcpip task starts (v0.9.0's `tcp_kb` was
    display-only). Affects only TCP that ends on the P4 (web UI, OTA, iperf to the device); NAT
    forwarded client traffic is not affected. 128 KB pins more internal RAM (see the v0.8.5 OTA note).
  - LwIP tcpip task core (`lwip_core`): Auto / Core 0 / Core 1, reboot required (`sys_thread_new`
    wrapper restored, `-Wl,--wrap=sys_thread_new`).
  - NAPT max sessions (`napt_max`): 512 / 1024 / 2048, reboot required.
  - NAPT TCP timeout (`napt_tcp_to`): 2 h / 1 h / 30 min, applied immediately.
- The four keys are no longer in the obsolete-key list, so they are not erased at boot. `tcp_kb`,
  `napt` and `log_lvl` stay obsolete; SDIO clock and log level stay fixed (40 MHz, Info).
- Unchanged from 0.9.3: band, shared protocol, TX power, power save, beacon, country,
  `sta_last_ch`, `csa_count = 3`, emergency flag on Debug. Partition table unchanged (OTA is enough).

## 0.9.3 — 2026-10-18 (builds, not hardware-tested)

- Wi-Fi APSTA channel handling (single C5 radio, AP always follows the STA):
  - The last channel the STA connected on is stored in NVS (`mypi_opt` / `sta_last_ch`, u8,
    1-14 or a 5 GHz channel 36-177). Written from the Wi-Fi task only when it changes.
  - `ap_configure()` picks the AP channel as: current STA channel > current AP channel >
    persisted last STA channel (only if it matches the band mode and the country's 2.4 GHz
    range) > 1 (36 in 5 GHz-only mode).
  - `ap.csa_count = 3`, so AP clients get channel switch announcements when the AP follows
    the STA to a new channel.
  - `refresh_ap_channel()` reads the live channel with `esp_wifi_get_channel()` and is forced
    once after every `STA_CONNECTED` (dirty flag).
  - `connect_now()` passes the last STA channel as scan hint (`wifi_config_t.sta.channel`) when it
    fits the band mode; no `esp_wifi_set_channel()` and no AP restart. The hint is dropped after a
    failed attempt so a moved upstream AP is still found by a full scan.
- Optimize page trimmed back. Kept: Band (both / 2.4 / 5 GHz), one protocol selector shared by
  STA and AP (checkboxes a / b / g / n / ac / ax), Wi-Fi TX power (default, 8 dBm .. 21 dBm max),
  STA power save, AP beacon interval, Wi-Fi country. Emergency flag status / Clear moved to Debug.
- 802.11ax can be selected again (known C5 firmware issue: upload can drop to ~0, ESP-IDF
  #19238). "None checked" still means b/g/n + a/n/ac without ax. A band left without any checked
  protocol uses its default set.
- Removed settings now run on fixed defaults: SDIO 40 MHz (auto-fallback mechanism kept, no UI),
  LwIP core auto (the `sys_thread_new` wrapper is gone), NAPT 512 sessions / 2 h TCP timeout,
  NAPT always on (per AP profile NAT flag), log level Info, width auto (2.4 GHz HT20 locked,
  5 GHz HT40), driver default DTIM, TX power only from Optimize.
  Old NVS keys `napt`, `log_lvl`, `lwip_core`, `napt_max`, `napt_tcp_to` are erased at boot; a
  stored `sdio_mhz` is ignored. In the radio blob the width, TX power and DTIM fields are ignored.
- Protocol migration: the old separate masks are merged once — the STA value wins if non-zero,
  else the AP value — and the same mask is applied to both interfaces.
- API: `/api/radio` takes `{band_mode, proto}` (old `sta_proto`/`ap_proto` still accepted and
  merged) and reports `proto` and `last_sta_channel`; `/api/opt` only has `wifi_tx_power_dbm`,
  `ps_mode`, `ap_beacon_ms`, `country`.
- OTA is enough (partition table unchanged).

## 0.9.2 — 2026-10-18 (builds, not hardware-tested)

- Optimize: new "LwIP / NAT" section below "C5 Radio / APSTA" (dropdowns only, NVS `mypi_opt`):
  - LwIP task core affinity `lwip_core` (u8 0 auto / 1 core 0 / 2 core 1, default 0); reboot
    required. `CONFIG_LWIP_TCPIP_TASK_AFFINITY` is a Kconfig constant in IDF v6.1, so the
    firmware wraps `sys_thread_new()` (`-Wl,--wrap=sys_thread_new`) and creates the `tcpip`
    task pinned to the selected core when `esp_netif_init()` starts it.
  - NAPT max sessions `napt_max` (u8 5/10/20 = 512/1024/2048 entries, default 5); reboot
    required. lwIP's `IP_NAPT_MAX` is redirected to a variable set at boot (`main/lwip_tune.h`,
    force-included into the lwip component); the table is allocated (PSRAM preferred) on the
    first NAPT enable.
  - NAPT TCP timeout `napt_tcp_to` (u8 2/1/0 = 2 h / 1 h / 30 min, default 2); applies
    immediately (`IP_NAPT_TIMEOUT_MS_TCP` is read by every NAPT GC pass). Note: the lwIP
    default was 30 min; the new default is 2 h as requested.
- sdkconfig.defaults: `CONFIG_LWIP_TCP_MSS=1460` (was the IDF default 1440).
  The requested `CONFIG_LWIP_TCP_SND_BUF_DEFAULT=8192`, `CONFIG_LWIP_TCPIP_RECVMBOX_SIZE=64` and
  `CONFIG_LWIP_TCP_RECVMBOX_SIZE=12` were not applied: the project already uses 65535 / 128 / 96,
  so they would be reductions, and 12 is below the required TCP_WND/MSS + 2 = 25.

## 0.9.1 — 2026-10-18 (builds, not hardware-tested)

- Web: the Wi-Fi protocol / band / width controls moved from System to the top of Optimize
  (before the SDIO setting). Selecting HT40 shows "HT40 on 2.4 GHz is not recommended — AP+STA
  share the same channel. Use HT20." HT40 stays selectable for future 5 GHz use; the firmware
  still always applies HT20 on 2.4 GHz (`apply_bw`, `.ghz_2g = WIFI_BW20`).
- Web: Max TX power and AP DTIM in that block are now dropdowns (no text/number inputs on Optimize).
  The AP beacon field of the radio block is replaced by the Optimize beacon setting below.
- New "C5 Radio / APSTA" settings (NVS namespace `mypi_opt`):
  - STA power save `ps_mode` (u8 0/1/2 = WIFI_PS_NONE / MIN_MODEM / MAX_MODEM, default 0),
    applied immediately via `esp_wifi_set_ps()`; UI tip "Recommended: Off in AP+STA mode".
  - AP beacon interval `beacon_ms` (u16 100/200/300, default 100; u16 because 200/300 do not fit
    in u8), set in `wifi_config_t.ap.beacon_interval`; reboot required.
  - Wi-Fi country `country` (string SE/EU/US/JP, default SE) set with `esp_wifi_set_country()`
    before Wi-Fi start (manual policy; SE/EU 1-13 @ 20 dBm, US 1-11 @ 30 dBm, JP 1-14 @ 23 dBm);
    reboot required.
- Removed the TCP window option (32/64/128 KB): no measurable difference on hardware. The lwIP
  window is fixed at 32 KB (`CONFIG_LWIP_TCP_WND_DEFAULT=32768`, saves internal RAM) and the
  obsolete `tcp_kb` NVS key is erased once at boot.

## 0.9.0 — 2026-10-18 (builds, not hardware-tested)

### Added
- **Optimize settings** (`main/opt_config.c/.h`, pure logic in `main/opt_config_core.h`),
  NVS namespace `mypi_opt` (all `u8`): `sdio_mhz` (40|50), `tx_dbm` (0 = radio setting, 2–21),
  `tcp_kb` (32|64|128), `napt` (0|1), `log_lvl` (0 error .. 3 debug), plus the internal flags
  `sdio_try` (50 MHz trial not yet confirmed) and `emerg` (emergency flag). Loaded in `app_main()`
  right after NVS init; log level, TX power and NAPT apply at runtime, SDIO clock and TCP window at boot.
- **SDIO 50 MHz with auto-fallback**: ESP-Hosted is no longer initialized by its constructor
  (`CONFIG_ESP_HOSTED_AUTO_CALL_INIT_BEFORE_APP_MAIN=n`); `wifi_task()` sets the SDIO clock from
  NVS (`esp_hosted_sdio_set_config()`) before `esp_hosted_init()` + `esp_hosted_connect_to_slave()`.
  A 50 MHz boot is a trial (`sdio_try=1` before the transport starts). Confirmed when the transport
  and `esp_wifi_init()` succeed. On any C5 failure at 50 MHz (`rescue_request()`: init failure/timeout,
  transport failure, no response) 40 MHz is stored and the P4 restarts — without consuming the
  automatic-restart budget. If a 50 MHz boot hangs/crashes before confirming, the next boot sees
  `sdio_try=1` and falls back to 40 MHz as well.
- **Emergency auto-clear**: emergency mode (entered on the second failed C5 init, logic unchanged)
  now also sets `emerg=1`. After a successful C5 init (`rescue_c5_init_ok()`) a set flag is cleared
  and `Emergency flag cleared after successful C5 init` is logged.
- **Web "Optimize" tab** (radio buttons, dropdowns, checkbox only — no text input): SDIO clock,
  TX power, TCP window, NAPT, log level, emergency status with **Clear**, **Apply & Save** and a
  yellow "Some changes require reboot" banner with **Reboot now**.
- API: `GET /api/opt`, `POST /api/opt` (→ `{"ok":true,"reboot_required":bool}`),
  `POST /api/clear_emergency`. JSON fields: `sdio_freq_mhz`, `wifi_tx_power_dbm`, `tcp_window_kb`,
  `napt`, `log_level`, `emergency_active` (read-only).

### Changed
- 2.4 GHz is hard-locked to HT20: `apply_bw()` always uses `esp_wifi_set_bandwidths()` with
  `ghz_2g = WIFI_BW20`; the width selection only applies to 5 GHz. 11ax stays off.
- `sdkconfig.defaults`: `CONFIG_LWIP_IRAM_OPTIMIZATION=y`, `CONFIG_LWIP_TCP_SND_BUF_DEFAULT=65535`.
  `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=10` was not added: the P4 uses `esp_wifi_remote`
  (`CONFIG_WIFI_RMT_STATIC_RX_BUFFER_NUM=32`, kept; 10 would lower it). `CONFIG_ESP32P4_SPIRAM_SPEED`
  does not exist; PSRAM already runs at 80 MHz (`CONFIG_SPIRAM_SPEED_80M`).
- RAM/PSRAM: LVGL flush buffers (status + keyboard, 2 × 9.6 KB) moved to internal RAM
  (time-critical); pin-test report (1.5 KB) and STA profiles (1.6 KB) moved to PSRAM.
- Optimize log level is the base level for the app tags; debug mode still wins.

### Known limitations
- `tcp_window_kb` is stored and reported, but lwIP's `TCP_WND` is a build-time constant
  (`CONFIG_LWIP_TCP_WND_DEFAULT`, 64 KB); the selection does not change the window yet.

## 0.8.9 — 2026-10-18 (bugfix, builds, not hardware-tested)

- Fix: password reveal now works — server populates value attribute (`/api/profiles` and `/api/approfiles` return the stored password; the STA/AP edit forms are filled on page load, on Edit and on slot change; fields stay `type="password"` until Show is clicked).

## 0.8.8 — 2026-10-18 (bugfix, builds, not hardware-tested)

Fixes from the first hardware evaluation of the 0.8.7 keyboard.

### Fixed
- **Wi-Fi password lost on OK (profile saved as "open")**: `password_kbd_done()` in
  `display_ui.c` closed the keyboard before queueing the connect job. Closing wipes the
  keyboard's text buffer, and the callback's `text` pointed into that buffer, so the job
  copied an empty string and `sta_profile_save()` stored an open profile. The callback now
  copies the password first, queues the job, wipes its copy and only then closes. The module
  itself (`mypi_kbd.c`) now also hands callbacks a private copy of the text, so any owner may
  close the keyboard inside its callback. A log line reports the password length (never the
  content) on OK. Regression test added in `tests/test_mypi_ui.c`.
- **Scroll wheel too fast**: `MYPI_KBD_SCROLL_STEP_PCT` in `mypi_kbd.c` sets the finger travel
  per character step; raised from 50 % to 100 % of a small wheel row (about 14 px to 29 px on
  the 240x320 panel), so each step needs a deliberate movement.

### Changed
- The Wi-Fi password on the LCD is shown in plain text during evaluation (`mask=false` in the
  `ui_lvgl_port_kbd_open()` call). The module still honors `mask=true` for other callers.
- Web UI: every password field (STA and AP profile forms) gets a small "Show"/"Hide" button
  that toggles the input between `type="password"` and `type="text"` (vanilla JS in
  `web_ui.js`, added automatically to any `input[type=password]`).

## 0.8.7 — 2026-10-18 (builds, not hardware-tested)
- **New reusable scroll-wheel keyboard module (`main/mypi_kbd.c/.h`, LVGL):** Companion-style
  text input — field with label, text and cursor; range tabs `az AZ 09 #+ åä`; hint row
  `< range >   ^ char v`; 5-slot wrapping wheel (row ratio 20:20:32:20:20, current row
  highlighted with an accent box) with `^`/`v` arrows; bottom row **BACK / DEL / NEXT / OK**.
  The caller passes parent, position and size; the whole layout scales from w/h
  (field 14 %, tabs 10 %, hint 7 %, wheel ~51 %, buttons 18 %).
- **Input:** vertical drag on the wheel steps one character per half row height (finger
  down = previous), a horizontal swipe changes range (one per swipe, left = next), tapping a
  tab selects the range, tapping an outer row moves it to the center. NEXT inserts the
  selected character, DEL removes the last UTF-8 character, OK calls back with
  `confirmed=true`, BACK calls back with `confirmed=false` and closes. `max_len` is in bytes;
  `mask` shows one `*` per character.
- **Font-aware:** every range is filtered against the glyphs in `mypi_font_14`
  (`lv_font_get_glyph_dsc`), so missing glyphs are never offered. With the current font the
  Latin range contains only `å ä ö Å Ä Ö`; é è ü ß etc. are listed but filtered out.
- **Styles:** `MYPI_KBD_STYLE_WHEEL`; `MYPI_KBD_STYLE_FULLKEY` is reserved for large screens.
  Unknown styles fall back to the wheel.
- **Wi-Fi password entry replaced:** the old framebuffer PREV/NEXT/ASCII-LATIN/ADD/DEL page
  (`draw_password()`, wheel state in `ui_state_t`) is removed. Choosing a network without a
  profile opens the keyboard full screen; OK with an empty password connects as an open
  network, 1–7 bytes shows a hint and keeps the keyboard open, BACK returns to the scan list.
  OTA/rescue close the keyboard. The UI task ticks every 20 ms while it is open.
- **LVGL port:** the keyboard runs on a second, full-screen (240×320) LVGL display created on
  first use; the pointer indev is switched to it while open. The status viewport is unchanged.
- **Tests:** `test_ui_primitives.c` covers the pure logic (`main/mypi_kbd_core.h`: ranges,
  filtering, wrap, range switching, UTF-8 insert/delete, byte limit); `test_mypi_ui.c`
  creates the keyboard at three sizes and drives it through the buttons and simulated
  pointer drags/taps.

## 0.8.6 — PSRAM for large buffers, automatic C5 restart before emergency mode (HW-tested: P4 OTA and C5 OTA over Wi-Fi STA OK)
- **Internal RAM freed (~83 KB):** the LVGL heap (`work_mem_int`, 64 KB), the LVGL draw buffer
  (`s_pixels`, 9.6 KB) and the crash tail are now placed in PSRAM
  (`SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`, `SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY`,
  `LV_ATTRIBUTE_LARGE_RAM_ARRAY` set from `main/CMakeLists.txt`). Internal DIRAM in use:
  121 KB, 534 KB left. Internal RAM stays available for DMA, Wi-Fi/TCP and fast code.
- **Crash tail raised 8 KB → 32 KB** (no-init PSRAM).
- **Automatic C5 recovery:** a C5 failure (init fail/timeout, SDIO transport failure, health check)
  now first saves the log and restarts the P4 once (the restart also resets the C5 via GPIO54).
  Emergency mode is only entered if the C5 fails again before Wi-Fi has run for 5 minutes.
  The counter survives the restart in no-init RAM and is cleared by power-on/brown-out.
- **Emergency mode is always logged:** entering emergency mode saves a log
  `emergency mode: <reason>` to flash. `/api/status` → `eth.rescue.auto_restarts` /
  `last_auto_reason`; the Status page shows a "C5 auto restart" row only when one happened.
- **v0.8.5 HW result:** Wi-Fi OTA succeeded on the first attempt (1 405 152 B in 17.4 s,
  0.65 Mbit/s, longest gap 0.1 s, 0 timeouts, min free internal heap 90 KB, no
  `eh_sdio dma_alloc` failures).

## 0.8.5 — Persistent logs in flash, OTA receive fixes (HW-tested: Wi-Fi OTA OK)
- **Persistent logs (`logstore.c`):** the unused `storage` partition (FAT, ~3.9 MB) is mounted
  at `/store`; logs go to `/store/logs`. An OTA upload (P4 or C5) is **always** captured
  (8 KB of context before the start, appended every 2 s) and committed as a saved log, so it
  survives the reboot or crash that follows a failed upload. Session capture ("Capture every
  session") is off by default and can be switched on from the Diagnostics page (saved in NVS,
  appended every 15 s, rolls over at 512 KB). The newest 5–10 logs are kept (default 8).
  A capture that was interrupted by a reset is committed at the next boot marked
  `NOT FINISHED` with the reset reason; web-triggered reboots commit the live log first.
- **Crash tail:** the last 8 KB of log output is kept in no-init RAM and saved after a
  panic / watchdog / software reset (not after power-on or brown-out).
- **Log API:** `GET /api/logstore` (status + list), `GET /api/logstore/file?name=…[&dl=1]`,
  `POST /api/logstore/config?enabled=0|1&keep=5..10`, `POST /api/logstore/save`,
  `POST /api/logstore/clear`. RAM log buffer raised to 32 KB (PSRAM).
- **OTA hypothesis fixes (from the v0.8.4 upload log):** free internal heap fell from 218K to
  50K during upload and the SDIO driver could not allocate its 14 KB DMA read buffer
  (`dma_alloc(14336) failed; dropping read`). TCP receive window lowered 128 KB → 64 KB and
  64 KB internal RAM reserved for DMA (`SPIRAM_MALLOC_RESERVE_INTERNAL`, small allocations
  ≤4 KB stay internal). The log hook now preserves `errno`, and the `httpd_txrx` warning is
  silenced, because `httpd_sock_err()` logs before reading `errno` — a receive timeout
  (EAGAIN, errno 11) was turned into a hard error and aborted the upload.
- **OTA diagnostics:** one `OTA mem:` line per second (free internal heap, largest internal
  DMA block, recv code); upload peer/local addresses now printed correctly for IPv4-mapped
  IPv6 sockets.

## 0.8.4 — Wi-Fi OTA: less traffic during upload, upload diagnostics (builds, not hardware-tested)
- **Analysis:** LVGL is not run during OTA (the UI task draws only the OTA view and does
  not call `lv_timer_handler()`; touch is not fed to LVGL), so LVGL is unlikely to slow
  the network directly. Changes below remove the extra traffic that v0.8.3 added
  around the upload and log what happens, so the real cause can be confirmed on hardware.
- **Web header:** the 5 s `/api/status` RSSI poll now runs only on the main page shell
  (and not while the tab is hidden). `/update`, `/c5update` and `/pins` read the header
  RSSI once on load and no longer send requests to the single-task HTTP server while it
  is busy with an upload.
- **Wi-Fi task:** the periodic STA RSSI RPC to the C5 (`esp_wifi_sta_get_ap_info`, every 5 s)
  is skipped while the OTA view is active, keeping the SDIO link free for upload data.
- **OTA diagnostics in `/api/log`:** peer and local IP of the upload (shows Wi-Fi STA, AP or
  Ethernet path), Mbit/s and free internal heap at every 10 %, and a summary on success,
  stall or abort (bytes, seconds, Mbit/s, longest gap between chunks, receive timeouts,
  minimum free internal heap, `httpd_req_recv` return code).

## 0.8.3 — LVGL standard, touch → LVGL, paged web UI (builds, not hardware-tested)
- **LVGL is the standard and only UI path.** Removed `CONFIG_MYPI_UI_LVGL` and the
  `sdkconfig.lvgl` overlay; `ui_lvgl_port.c`, `mypi_ui/lvgl_view.c` are always built and
  the `display_ui` task always has an 8 KiB stack. If LVGL init fails, the home view
  falls back to plain-text status lines.
- **Touch → LVGL (ARCHITECTURE step 4):** the main touch loop publishes rotation-corrected
  samples (`ui_lvgl_port_touch_feed()`, spinlock) to an LVGL pointer indev; `read_cb` and
  `lv_timer_handler()` run only on the UI task. OTA > rescue > normal UI priority and the
  existing tap/swipe navigation are unchanged. LVGL pixels are only written during the
  synchronous status render, never over menus/OTA/rescue.
- **MyPi status font** `main/mypi_font_14.c` (DejaVu Sans 14 px, 4 bpp, generated with
  lv_font_conv 1.5.3): ASCII + `° µ Å Ä Ö å ä ö μ π —`; used by all LVGL status lines.
- **LCD Wi-Fi icon:** 20-step linear red (≤ -80 dBm) → green (≥ -40 dBm) gradient
  (`mypi_rssi_color_step()` in `ui_layout.h`); not-connected handling unchanged.
- **LCD divider:** the 8N1 waveform now encodes the whole word `Hotspot` (70 bits) over
  the full 240 px width (`mypi_uart_x_span()`), same 8 px height, header and home bottom.
- **Web header:** more space around the logo, larger `Hotspot`, right-aligned Wi-Fi icon +
  STA RSSI (e.g. `-56 dBm`, same color gradient, `—` when not connected; polled from
  `/api/status` every 5 s), divider with margin above and below, page buttons below it.
- **Web pages:** the long dashboard is split into hash-routed pages in one HTML shell:
  Overview → Wi-Fi → Access point (incl. clients) → Traffic (incl. iperf) → Ethernet →
  System (radio, display rotation, P4/C5 OTA links, reboot) → Debug (pins, status/log
  API, touch events, log). Same 39 URI handlers; all APIs and `/update`, `/c5update`,
  `/pins` unchanged.
- Tests: new full-width divider and RSSI-step tests (C), routing/RSSI/page-order tests (JS).

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
