# MyPi-Hotspot — project status

Snapshot: 2026-10-18, firmware **v0.9.8**. See `CHANGELOG.md` for the per-version list
and `README.md` for wiring, flashing and usage.

## Verified baseline checkpoint — 2026-10-17

The user reports that all implemented core functions have been verified on real
hardware and explicitly approves the current baseline for commit.

This checkpoint preserves firmware **v0.7.0** at main commit
[`f51d4dd6f9a7b07e8440a131082d9223f2348042`](https://github.com/Bokbacken/MyPi-Hotspot/commit/f51d4dd6f9a7b07e8440a131082d9223f2348042).
The hardware-tested functionality listed below is already committed on `main`;
this is a documentation-only checkpoint, not a new firmware release.

UI, character-wheel, page-navigation, logo and header follow-ups in PRs
[#4](https://github.com/Bokbacken/MyPi-Hotspot/pull/4),
[#5](https://github.com/Bokbacken/MyPi-Hotspot/pull/5) and
[#6](https://github.com/Bokbacken/MyPi-Hotspot/pull/6) remain separate and untouched.
No follow-up code is imported or newly marked hardware-tested by this checkpoint.
Firmware sources, versions, build configuration, partition tables, bootloader and
P4/C5 OTA paths are unchanged.

## v0.9.8 — 2026-10-18 (builds, not hardware-tested)

- Revert: `sdkconfig.defaults` and `c5_slave/sdkconfig.defaults.esp32c5` restored to v0.9.6
  values (host SDIO RX queue 64, RX staging 4, cache TX default 32; C5 buffers 16/64/64,
  BA 32, SDIO queues 40/40). Host IRAM opts left at defaults (no explicit Kconfig overrides).
- Retained: the log line in `wifi.c` that prints the Wi-Fi buffer config sent to the C5 (added
  in v0.9.7; useful for diagnostics in future tuning rounds).
- C5 tuning: on hold until an official ESP-Hosted release (3.0.10+). Do not attempt new buffer
  experiments with the current 3.0.9 C5.
- v0.9.7 HW result: AP disappeared ~30 s after boot (download ~1.5 Mbit/s, upload 0, then link
  lost). Likely DMA buffer starvation from larger RX queue (128) and staging (6).
- v0.9.6 HW result: HT40 links at 40 MHz stably, but is slower than ac/HT20 and disturbs
  Wi-Fi 7 APs. ac/HT20 is the recommended 5 GHz setting. PR #18 merged to main (467e628).
- Build: 0 warnings; all 4 host test suites passed. Not hardware-tested.

## v0.9.7 — 2026-10-18 (HW-tested: unstable — reverted in v0.9.8)

- C5 buffer / upload throughput tuning: host SDIO RX queue 64 → 128, RX staging 4 → 6,
  cache TX 32 → 64, IRAM opts pinned; C5 slave buffers 16/32 → 32/128 dynamic, BA 32 → 64,
  SDIO TX queue 40 → 64. All reverted in v0.9.8.
- HW result: not usable (AP link lost ~30 s after start). See v0.9.8 note above.

## v0.9.6 — 2026-10-18 (HW-tested: HT40 functional; ac/HT20 recommended)

- 5 GHz HT40 fix: C5 / IDF v6.1 only allows 40 MHz in 11an mode. When ac/ax are enabled,
  the driver silently stays at 20 MHz. `apply_proto()` now strips ac/ax from the 5 GHz
  protocol mask when `bw_5g = HT40`; HT20 retains the selected protocols.
- Width re-applied after the post-start protocol re-apply (a protocol change can reset it).
- HW result: HT40 links up stably at 40 MHz, but throughput is lower than ac/HT20. HT40
  also disturbs Kent's Wi-Fi 7 APs. Recommended: ac/HT20 (`bw_5g = HT20`, select manually);
  the firmware default is still HT40.
- UI: Optimize dropdown shows "HT40 - 11n" / "HT20 - ac/ax allowed" with an explanation.

## v0.9.5 — 2026-10-18 (HW-tested: HT40 result invalid — fixed in v0.9.6)

- Two new Optimize dropdowns (both require reboot):
  - `bw_5g`: 5 GHz channel width HT40 (default) / HT20. 2.4 GHz stays locked to HT20.
  - `sdio_mhz`: SDIO clock 40 / 50 MHz with auto-fallback (50 MHz trial; reverts to 40 if C5
    fails). Real bus clock logged at boot and returned by `GET /api/opt` as `sdio_real_khz`.
- 50 MHz wrapper (`main/sdio_clk.c`, `--wrap=sdmmc_io_enable_hs_mode`) lifts the IDF 40 MHz
  card limit when the configured host clock exceeds it.
- HW result (HT40): link stayed at HT20 despite an 80 MHz AP — root cause found in v0.9.6.

## v0.9.0 — 2026-10-18 (builds, not hardware-tested)

- New Optimize module (`opt_config`) and web tab: SDIO 40/50 MHz with auto-fallback to 40 MHz,
  TX power cap, TCP window (stored only, see limitations), NAPT master switch, log level,
  emergency flag with Clear. API `GET/POST /api/opt`, `POST /api/clear_emergency`.
- Emergency flag in NVS (`mypi_opt/emerg`) is cleared automatically after a successful C5 init.
- 2.4 GHz hard-locked to HT20 in `apply_bw()`; 11ax still off. lwIP IRAM optimization on.
- RAM/PSRAM review: LVGL flush buffers in internal RAM, small static text/profile tables in PSRAM
  (see `ARCHITECTURE.md`).
- Build: 0 warnings; host tests (4 suites) green. Not yet run on hardware.

## v0.8.7 — 2026-10-18 (builds, not hardware-tested)

- Text input rebuilt as a reusable LVGL module, `mypi_kbd` (Companion-style scroll wheel,
  placement and size given by the caller, glyph set filtered against `mypi_font_14`).
- The Wi-Fi password page now uses it full screen; the old framebuffer input is removed.
- Font coverage: only `å ä ö Å Ä Ö` of the Latin list are available in `mypi_font_14`.
- Next: test on hardware — drag feel (step = half row height), swipe/tap, BACK/DEL/NEXT/OK,
  connecting with a password and with an empty password (open network).

## v0.8.6 — 2026-10-18 (HW-tested, merged to main: PRs #12–#15)

- Hardware result: P4 OTA and C5 OTA both succeeded over the Wi-Fi STA uplink.

- v0.8.5 hardware result: Wi-Fi OTA OK on the first attempt (17.4 s, 0.65 Mbit/s, no SDIO DMA
  allocation failures, min free internal heap 90 KB).
- After a restart (before v0.8.5) the device sometimes came up in emergency mode; cause not
  confirmed (no log). v0.8.6 restarts once automatically on a C5 failure and always saves a log.
- LVGL heap, draw buffer and the (now 32 KB) crash tail moved to PSRAM: ~83 KB internal RAM freed.
- Next: flash v0.8.6 via `/update`; if emergency mode or a "C5 auto restart" row appears, fetch
  the saved log. Possible follow-ups: faster OTA (0.65 Mbit/s), display DMA bounce buffers in
  PSRAM (~38 KB more), `esp_wifi_set_band_mode` called before Wi-Fi start.

## v0.8.5 — 2026-10-18 (HW-tested: Wi-Fi OTA OK)

- Wi-Fi OTA has failed on almost every attempt since v0.8.3/v0.8.4. The v0.8.4 upload log
  showed free internal heap 218K → 50K, `eh_sdio: dma_alloc(14336) failed; dropping read`,
  then `httpd_req_recv` returned -1 (errno 11 / EAGAIN) after ~15 s at 0.06 Mbit/s.
- **Hypothesis 1:** received TCP segments hold internal RAM until the app reads them; with a
  128 KB window and flash writes slowing the reader, SDIO DMA buffers run out and packets drop.
  Mitigation: TCP window 64 KB, 64 KB internal RAM reserved for DMA.
- **Hypothesis 2:** `httpd_sock_err()` logs before reading errno; the log hook clobbered errno so
  a timeout became a hard failure. Mitigation: errno preserved, `httpd_txrx` warning silenced.
- Persistent logs (`/api/logstore`): OTA uploads always saved to flash so the log can be read
  after the reboot that follows a failed upload; session capture on demand.
- Next: flash v0.8.5, repeat Wi-Fi OTA (and Ethernet OTA as reference), fetch the saved log.

## What the device does today

| Area | Status |
|---|---|
| ST7789 LCD + CST816D touch, 180° rotation | HW-tested |
| Web UI, JSON API, log buffer, debug mode | HW-tested |
| Wi-Fi APSTA via ESP32-C5 (ESP-Hosted 3.0.9, SDIO 4-bit 40 MHz) | HW-tested |
| NAPT, STA/AP profiles, DHCP server | HW-tested |
| Ethernet LAN (L2 bridge to AP) / WAN (DHCP uplink) | HW-tested |
| OTA of the P4 (`/update`) and of the C5 slave (`/c5update`) | HW-tested |
| Traffic stats, peak rates, CSV log, iperf2 on port 5001 | HW-tested |
| OTA progress on the LCD (v0.6.3) | HW-tested |
| P4 SDIO RX staging slots = 4, tuned C5 slave queues (v0.6.3) | HW-tested; throughput vs. baseline not yet recorded here |
| Ethernet emergency mode (v0.7.0) | HW-tested |
| LCD menu + web theme (v0.8.0) | HW-tested |
| μ/π logo header, antialiased title, RSSI-colored Wi-Fi icon (v0.8.1) | HW-tested as part of the v0.8.2 LVGL build |
| LVGL 9.4.0 home status viewport, `CONFIG_MYPI_UI_LVGL=y` (v0.8.2, `sdkconfig.lvgl`) | HW-tested (display and normal operation OK) |
| v0.8.2 default build (`CONFIG_MYPI_UI_LVGL=n`) | Builds; not separately flashed (option removed in 0.8.3) |
| v0.8.3: LVGL standard, touch → LVGL pointer indev, MyPi font (å ä ö μ π), 20-step RSSI icon color, full-width `Hotspot` LCD divider, paged web UI with dBm header | Builds (0 warnings), host tests pass; not yet hardware-tested |
| On-screen LVGL scroll-wheel keyboard, `mypi_kbd` (v0.8.7) | HW-tested |
| Optimize page (v0.9.x): web only, dropdowns/checkboxes, NVS `mypi_opt` | Builds, host tests pass; individual options tested as listed below |
| TCP window 32/64/128 KB applied in lwIP (v0.9.4) | HW-tested |
| 5 GHz width HT40/HT20 + SDIO clock 40/50 MHz with auto-fallback (v0.9.6) | HW-tested; HT40 works but is slower, HT20 recommended |
| Revert of the v0.9.7 buffer tuning to v0.9.6 values (v0.9.8) | Builds (0 warnings), host tests pass; not yet hardware-tested |

## Architecture in short

```
            Wi-Fi uplink (STA)          Wi-Fi clients (AP)
                    \                     /
                 ESP32-C5 (one radio, ESP-Hosted 3.0.9 slave)
                              | SDIO 4-bit, 40 MHz
                 ESP32-P4 (this firmware: lwIP, NAPT, web, LCD)
                              | RMII
                      IP101 Ethernet (LAN bridge or WAN)
```

- The P4 runs the whole IP stack. The C5 is a Wi-Fi radio driven over RPC.
- LAN mode bridges Ethernet frames into the AP subnet at L2 (no own IP on Ethernet).
- WAN mode uses Ethernet as a DHCP uplink instead of the STA.
- Emergency mode switches Ethernet to a standalone netif at runtime and does not
  depend on the C5.

## Hard facts and decisions

- **The C5 has one radio.** In APSTA, AP and STA always share one channel and band;
  2.4 GHz and 5 GHz cannot run at the same time. Dual radio is on hold.
- **11ax is selectable but not the default.** The UI shows a warning: known C5 issue
  ESP-IDF #19238 (upload can drop to ~0). Focus is b/g/n/a/ac.
- **40 MHz only in 11n/11an** on the C5. With 11ac/11ax on 5 GHz the driver stays at 20 MHz,
  so HT40 drops ac/ax. 5 GHz HT20 is recommended; the firmware default is still HT40.
- **2.4 GHz is always HT20.**
- **SDIO runs at 40 MHz by default**; 50 MHz is optional (Optimize) with auto-fallback to 40 MHz.
- **C5 firmware and buffer tuning are on hold** until an official ESP-Hosted 3.0.10+ release.
- ESP-IDF **v6.1**, ESP-Hosted **3.0.9** on both host and C5 slave.
  ESP-Hosted v2 and v3 RPC are incompatible; host and slave must match.
- The C5 is updated permanently through `/c5update` (flash path A).
- The P4 does not reboot when the C5 link fails; emergency mode takes over instead.
- All rates are Mbit/s with 2 decimals (`*_mbps`). Old CSV files with `*_bps` are bytes/s.
- Preferred flashing: OTA via `/update` (keeps NVS). A merged binary at 0x0 likely
  erases NVS (profiles, settings).

## Buffer findings (v0.9.7 investigation — defer tuning until ESP-Hosted 3.0.10+)

Investigated in v0.9.7; reverted in v0.9.8. Do **not** retry C5 buffer changes with ESP-Hosted 3.0.9.

**RPC v2 routing (confirmed):** `esp_wifi_init()` sends a `wifi_init_config_t` built from
`WIFI_RMT_*` Kconfig values to the C5 via ESP-Hosted RPC v2. The C5 slave applies most of
them as-is. **Exceptions:**
- `cache_tx_buf_num`: C5 has no PSRAM → IDF replaces with 0 regardless of host value.
- `tx_ba_win`: not part of the RPC struct; only the C5's own sdkconfig value is used.
- `amsdu_tx_enabled`: forwarded but requires PSRAM on C5 → leave off.

**v0.9.7 HW failure (AP lost ~30 s):** raising host SDIO RX queue 64 → 128 and RX staging
4 → 6 added too much internal DMA pressure (similar to the v0.8.5 `dma_alloc failed`
pattern with a 128 KB TCP window).

**Baseline to compare against** (v0.5.3, ch36 STA, ch1 AP, 20 MHz/ac):
STA RX 16.32 / TX 20.76, AP RX 20.61 / TX 16.26 Mbit/s (peak).

**When 3.0.10+ is available:** re-evaluate SDIO RX queue (try 96 before 128), re-measure
against the baseline, and watch the boot log for `dma_alloc failed` and `eh_sdio` errors.

## Lessons learned

- Power the LCD module from 3V3 only; 5V on VCC plus 3V3 caused a dark screen and
  touch I2C timeouts.
- The CST816D gesture register ignores rotation; use rotated touch coordinates instead.
- Avoid strapping pins 34–38 for peripherals; LCD SPI moved to GPIO23/GPIO5.
- OTA over a 128K TCP window needs a larger recvmbox, otherwise uploads stall.

## Regression checklist (run for every new build)

All builds up to v0.8.0, plus the v0.8.2 LVGL variant, have passed on hardware. Repeat this list after each change.


1. Flash via `/update`; confirm the new version on the status page.
2. Emergency mode: swipe down within 10 s → emergency screen with 5 buttons.
3. DHCP option with a cable to a router → IP shown on LCD, web UI reachable.
4. Static `192.168.4.32` with a laptop set to `192.168.4.x/24` directly on the port.
5. Switch between options while emergency mode is active.
6. Simulate a dead C5 (hold it in reset at boot) → emergency mode within ~30 s.
7. OTA progress view on the LCD for both `/update` and `/c5update`.
8. Run iperf (port 5001) and compare with the v0.5.3 baseline and the previous build.

## Next steps

### PR #4 scope clarification — approved MyPi identity (2026-10-17)

This updates the planned scope of [PR #4](https://github.com/Bokbacken/MyPi-Hotspot/pull/4)
(`copilot/upgrade-lcd-web-interface`), not the v0.7.0 hardware-tested snapshot above.

- **Canonical artwork:** the two μ/π logo variants uploaded and approved by the
  user in the task conversation are the primary visual reference for the MyPi
  family and the Hotspot LCD/web redesign. Both are approved variants of the same
  identity, not proposals to redraw or replace.
- **Asset handoff:** the uploaded image files are not present in this checkout.
  Before embedding artwork, obtain those exact files through an accessible
  attachment or repository asset and record their source and filenames. Do not
  invent replacement artwork, approximate the mark with font glyphs, or depend
  on inaccessible private Companion assets. Approval is settled; importing and
  verifying the actual files remains pending.
- **LCD target:** use an approved variant in the home/header treatment, preserving
  its proportions and legibility at the small display's resolution. Any scaled
  or device-format export must derive from the approved artwork, not a new logo.
- **Web target:** use the same approved identity in shared page chrome across
  the overview, separate task pages, OTA pages and pins page. Embed assets locally
  in firmware; branding must not depend on a CDN or an Internet connection.
- **Family language:** retain the serial-bit-pattern divider with the project
  name beneath it: `H` on the LCD and `Hotspot` on the web, framed as 8N1 (start
  bit, eight LSB-first data bits, stop bit). The divider complements the approved
  μ/π identity; it does not replace the logo.
- **Existing follow-up stays in scope:** repair character-wheel input using the
  Companion user guide as a behavioral reference where accessible, including
  touch interaction, editing, UTF-8 storage and character display. Split web
  navigation into actual pages rather than section-anchor scrolling, with
  logical task names and a consistent flow. Branding is a scope clarification,
  not a replacement for either feature.
- **Delivery constraints:** retain native ESP-IDF/C and the existing OTA/rescue
  paths. No bootloader, partition-table or NVS-layout changes are required by
  this branding decision. Future firmware builds must still fit the existing OTA
  app partition and pass the regression checklist above.

Completion requires the exact approved assets to be available and embedded,
consistent LCD/web branding checked against both source variants, and the
character-wheel, page navigation and OTA/rescue behavior validated. This scope
note does not claim that those implementation or hardware checks are complete.

### Existing performance follow-up

- Throughput tuning, experiment #1: `WIFI_RMT_STATIC_TX_BUFFER_NUM` 32–64 on the P4,
  measured with iperf against the baseline.
- Further buffer/queue experiments based on the buffer findings above.
