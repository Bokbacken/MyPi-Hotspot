# MyPi platform direction

Status: **Accepted — 2026-10-17**. This records the direction before UI migration;
it does not implement LVGL or claim that the current firmware already meets every
platform standard.

## Decisions

- **Platform scope:** MyPi is a family of smart, primarily ESP32-based tools, not
  just Hotspot. ESP-IDF and Arduino are the main environments today; future
  hardware may use other platforms.
- **Graphical UI:** LVGL is the chosen direction for future local graphical
  interfaces on supported devices, across small and larger displays. Hardware
  display/touch drivers and integration remain device-specific. Web interfaces
  remain HTML/CSS-based, sharing design concepts rather than the renderer.
- **Text:** UTF-8 is the standard for all text input/output across local UI, web,
  APIs, and diagnostics. Preserve text without silent conversion or truncation;
  byte limits and glyph coverage must be handled explicitly during migration.
  This does not authorize exposing credentials or other secrets.
- **Identity:** Keep the MyPi name, approved μ/π logo, serial-divider motif, and
  shared status/navigation concepts as canonical design elements. Retain the
  meaningful, project-specific 8N1 framing described in
  [the UI conventions](NEXT_STEPS.md#visual-identity-and-asset-maintenance).
  Adapt layouts to display size and input method rather than copying fixed geometry.
- **Compatibility:** Preserve existing backend contracts, APIs, and functionality
  through the migration. Local and web interfaces should use the same backend
  state and commands rather than duplicate product logic.

## Migration boundary

Keep the existing backend, connectivity, profiles, status, OTA, and recovery
functionality. Current Hotspot/Companion-style LCD and web presentation code is a
transitional layer: keep it working now, but allow its rendering and layout to be
replaced later without discarding product functionality or MyPi identity.

## LVGL migration

LVGL 9.4.0 is a pinned ESP-IDF managed dependency and, since 0.8.3, the standard and
only local UI path (the `CONFIG_MYPI_UI_LVGL` option and the `sdkconfig.lvgl` overlay
were removed; `sdkconfig.defaults` requires `CONFIG_LV_TXT_ENC_UTF8` and
`CONFIG_LV_USE_LABEL`, enforced by an `#error` in `ui_lvgl_port.c`). There are no
partition, endpoint, networking, profile, or update-format changes.

### Boundaries

- `main/mypi_ui/state.h` is the hardware/framework-independent entry point:
  borrowed UTF-8 status lines with semantic presentation colors, no credentials
  or backend commands. Product code owns the snapshot until rendering returns.
- `main/mypi_ui/lvgl_view.*` copies that state into LVGL labels. It knows neither
  ESP-IDF nor Hotspot, and sizes itself to its parent so future Arduino or other
  ports can reuse it on different screens.
- `main/ui_lvgl_port.*` alone connects LVGL to the current display framebuffer,
  ESP timer and touch samples. Its partial RGB565 buffer is 9,600 bytes. The existing
  display driver retains rotation, locking, SPI/DMA completion and wire byte swapping.
  Flush-ready means pixels have been copied into the framebuffer; the existing
  caller flushes that framebuffer to the panel afterwards. Pixels are only written
  during `ui_lvgl_port_render()`; a refresh started from `lv_timer_handler()` never
  draws over menus, OTA or rescue views.
- `main/display_ui.c` adapts existing public backend getters to the shared state.
  The home status area (y=32..231) uses LVGL. The canonical logo, 8N1 serial dividers,
  status header, menu button, other pages, OTA and rescue continue using the
  transitional renderer. If LVGL cannot be initialized, the home view falls back to
  plain-text status lines; this is not a general out-of-memory recovery guarantee.

The `display_ui` task (8 KiB stack) owns **all** LVGL calls: init/deinit, rendering
via `lv_refr_now()` at the status cadence (full viewport invalidation on each render
because transitional pages may have overwritten it) and `lv_timer_handler()` every
UI tick (200 ms) while the normal UI is shown. OTA and rescue keep precedence in that
loop (OTA view > rescue > normal UI), including the boot rescue swipe.

### Touch input (step 4, 0.8.3)

The main loop remains the single owner of the CST816D: it calls `touch_read()`
(rotation-corrected coordinates) and publishes each sample through
`ui_lvgl_port_touch_feed()` into a spinlock-protected latest-sample slot. The LVGL
pointer indev (`LV_INDEV_TYPE_POINTER`) reads that slot in its `read_cb`, which runs
inside `lv_timer_handler()` on the UI task — the controller is never polled twice.
Coordinates are translated into the 240×200 viewport; touches outside it (header,
menu button) are reported as released. While the OTA view is visible the feed reports
released. Tap navigation, swipe-to-rescue and rescue option hits stay in the main loop
(`display_ui_handle_tap()`, `display_ui_rescue_hit()`), so behavior is unchanged until
pages are migrated to interactive LVGL widgets.

### Text input module (0.8.7)

`main/mypi_kbd.h` is the single text-input entry point for local LVGL screens:

```c
typedef void (*mypi_kbd_cb_t)(const char *text, bool confirmed, void *user);
typedef enum { MYPI_KBD_STYLE_WHEEL = 0 /* MYPI_KBD_STYLE_FULLKEY reserved */ } mypi_kbd_style_t;
lv_obj_t *mypi_kbd_create(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h,
                          mypi_kbd_style_t style, const char *label, const char *initial_text,
                          size_t max_len, bool mask, mypi_kbd_cb_t cb, void *user);
void mypi_kbd_delete(lv_obj_t *kbd);              // async, idempotent, wipes the text
void mypi_kbd_set_hint(lv_obj_t *kbd, const char *message);
const char *mypi_kbd_get_text(lv_obj_t *kbd);
```

- The caller gives placement and size; all parts scale from w/h (field 14 %, tabs 10 %,
  hint 7 %, wheel ~51 % with rows 20:20:32:20:20, buttons 18 %). The font is `mypi_font_14`;
  the current row is emphasized with an accent box since no larger font is built in.
- Pure logic (ranges, glyph filter hook, wrap, UTF-8 edit, byte limit) lives in the header-only
  `main/mypi_kbd_core.h` (no LVGL) and is host-tested.
- The style parameter lets large screens get a full keyboard later without changing callers;
  unknown styles fall back to the wheel.
- Hosting on the Hotspot: `ui_lvgl_port_kbd_open/close/active/set_hint/tick()` put the keyboard
  on a second, full-screen LVGL display and move the pointer indev to it. While open,
  `ui_lvgl_port_kbd_tick()` (UI task, every 20 ms, under `display_lock()`) runs LVGL and copies
  rendered areas into the framebuffer; `ui_lvgl_port_poll()` is skipped so no invalidation is lost.

### Status font

`main/mypi_font_14.c` is DejaVu Sans 14 px, 4 bpp (license: `assets/DejaVu-LICENSE.txt`),
covering ASCII, `°`, `µ`, `Å Ä Ö å ä ö`, Greek `μ π` and `—`. It is set on the status
view root, so all labels inherit it. Regenerate with
[lv_font_conv](https://github.com/lvgl/lv_font_conv) 1.5.3:

```sh
npx lv_font_conv --no-compress --font DejaVuSans.ttf --size 14 --bpp 4 \
  --range 0x20-0x7E,0xB0,0xB5,0xC4,0xC5,0xD6,0xE4,0xE5,0xF6,0x3BC,0x3C0,0x2014 \
  --format lvgl --lv-include lvgl.h --lv-font-name mypi_font_14 -o main/mypi_font_14.c
```

UTF-8 bytes are passed to `lv_label_set_text()` without normalization, transliteration,
or byte-based abbreviation. Long labels are visually clipped, not rewritten. Glyphs
outside the subset (e.g. other accented letters) render as missing-glyph boxes; stored
and API text is never changed.

### Steps and validation

1. Done (0.8.2, HW-tested): read-only LVGL home status viewport.
2. Done (0.8.3): LVGL is the standard build; shared font with explicit glyph coverage.
3. Next: migrate the identity header/dividers and navigation widgets without changing
   their meaning.
4. Done (0.8.3, not yet HW-tested): rotation-corrected touch samples from the existing
   touch owner feed an LVGL pointer indev through a thin adapter; rescue/OTA intercept
   preserved; tick/timer handling on the UI task.
5. Next: migrate one functional page at a time (menu first); keep backend
   jobs/commands outside the renderer. Remove the transitional renderer only after
   parity and hardware validation.

Hardware checklist for 0.8.3: home/menu/back taps, rotation, boot rescue swipe and
options, P4/C5 OTA progress/failure, Swedish/μπ glyphs, RSSI icon color, full-width
divider, unchanged web/API behavior.

`tests/test_mypi_ui.c` exercises the shared view using real LVGL at two viewport
sizes and the MyPi font glyph coverage (å ä ö Å Ä Ö µ μ π), including UTF-8 byte preservation, copied snapshot lifetime, row clearing,
invalid state rejection and partial rendering. It follows the existing host C
assert-test approach. With the pinned LVGL source extracted outside the repository,
the host commands are (run the final two commands from the repository root):

```sh
printf '#define LV_CONF_H\n#define LV_COLOR_DEPTH 16\n' > /tmp/mypi-lv-conf.h
cmake -S <lvgl-source> -B /tmp/mypi-lvgl-host \
  -DLV_BUILD_CONF_PATH=/tmp/mypi-lv-conf.h \
  -DCONFIG_LV_BUILD_DEMOS=OFF -DCONFIG_LV_BUILD_EXAMPLES=OFF \
  -DCONFIG_LV_USE_THORVG_INTERNAL=OFF
cmake --build /tmp/mypi-lvgl-host -j4
cc -std=c11 -Wall -Wextra -Werror -DLV_CONF_PATH='"/tmp/mypi-lv-conf.h"' \
  -Imain -I<lvgl-source> tests/test_mypi_ui.c main/mypi_ui/lvgl_view.c main/mypi_font_14.c \
  /tmp/mypi-lvgl-host/lib/liblvgl.a -lm -o /tmp/test_mypi_ui
/tmp/test_mypi_ui
```

Existing C, JavaScript and logo tests remain applicable; hardware validation is
still required before release.


## RAM/PSRAM strategy (0.9.0)

Rule: time-critical buffers live in internal RAM; large static allocations and
buffers where the receiver is the bottleneck live in PSRAM. When unsure, choose
the safe option ("better safe than sorry").

| Buffer | Placement | Reason |
|---|---|---|
| ESP-Hosted SDIO DMA buffers | Internal (driver-managed, DMA-capable) | DMA/latency critical |
| lwIP TCP socket rx/tx (`TCP_SND_BUF` 65535, `TCP_WND` 32 KB default, 32/64/128 KB via Optimize since v0.9.4) | Internal heap | throughput critical; `CONFIG_LWIP_IRAM_OPTIMIZATION=y` puts lwIP hot paths in IRAM |
| LVGL flush buffers `s_pixels` + `s_kbd_pixels` (2 x 9.6 KB, `ui_lvgl_port.c`) | Internal `.bss` (moved from PSRAM in 0.9.0) | flush path, feeds the display DMA |
| Display DMA bounce buffers (`display.c`) | Internal DMA heap | DMA requirement |
| LVGL heap (64 KB), framebuffer-sized buffers | PSRAM | large, not latency critical |
| Log ring buffer, log store chunks, net log, web log list | PSRAM (`heap_caps_malloc(MALLOC_CAP_SPIRAM)`) | large, consumer (flash/web) is the bottleneck |
| Pin test report `s_report` (~1.5 KB, `pintest.c`) | PSRAM (`EXT_RAM_BSS_ATTR`, 0.9.0) | static, rarely used |
| STA profile table `s_sta` (~1.6 KB, `profiles.c`) | PSRAM (`EXT_RAM_BSS_ATTR`, 0.9.0) | static, not time critical |

Always-on build options in `sdkconfig.defaults`: `CONFIG_LWIP_IRAM_OPTIMIZATION=y`,
`CONFIG_LWIP_TCP_SND_BUF_DEFAULT=65535`. The Wi-Fi static RX buffer count is
already 32 (`WIFI_RMT_STATIC_RX_BUFFER_NUM`, higher than the suggested 10) and
`CONFIG_SPIRAM_SPEED=80` is the applicable P4 symbol (`CONFIG_ESP32P4_SPIRAM_SPEED`
does not exist in IDF 6.1). 2.4 GHz is locked to HT20 at runtime
(`esp_wifi_set_bandwidths`). 802.11ax is selectable again since v0.9.3 but not in the default
set; the UI warns about the known C5 upload issue
([ESP-IDF #19238](https://github.com/espressif/esp-idf/issues/19238): TX hang under TCP uplink
load, EDCA/TX buffer exhaustion in the IDF v6.1 Wi-Fi libraries; a fix will come with a new IDF
release, not with ESP-Hosted 3.0.10). Host SDIO RX queue 64 and RX
staging 4 are the v0.9.6 values restored in v0.9.8; C5 buffer tuning is on hold until an official
ESP-Hosted 3.0.10+.

## opt_config module (0.9.0)

Files: `main/opt_config_core.h` (pure logic, host-testable via an `opt_store_t`
mock), `main/opt_config.h`, `main/opt_config.c` (NVS binding and apply hooks).

NVS namespace `mypi_opt` (`u8` unless noted):

| Key | Field | Values | Default | Applied |
|---|---|---|---|---|
| `tx_dbm` | `wifi_tx_power_dbm` | 0 (radio default), 8..21 | 0 | runtime |
| `ps_mode` | `ps_mode` | 0 none / 1 min modem / 2 max modem | 0 | runtime (`esp_wifi_set_ps`); Off recommended in AP+STA |
| `beacon_ms` (`u16`) | `ap_beacon_ms` | 100 / 200 / 300 | 100 | boot (`wifi_config_t.ap.beacon_interval`) |
| `country` (string) | `country` | SE / EU / US / JP | SE | boot (`esp_wifi_set_country`, manual policy) |
| `tcp_wnd_kb` | `tcp_wnd_kb` (v0.9.4) | 32 / 64 / 128 | 32 | boot (`mypi_tcp_wnd`, lwIP `TCP_WND` via `main/lwip_tune.h`) |
| `lwip_core` | `lwip_core` (v0.9.4) | 0 auto / 1 core 0 / 2 core 1 | 0 | boot (`__wrap_sys_thread_new` pins the tcpip task) |
| `napt_max` | `napt_max` (v0.9.4) | 5 / 10 / 20 = 512 / 1024 / 2048 entries | 5 | boot (`IP_NAPT_MAX`) |
| `napt_tcp_to` | `napt_tcp_to` (v0.9.4) | 2 = 2 h / 1 = 1 h / 0 = 30 min | 2 | runtime (`IP_NAPT_TIMEOUT_MS_TCP`) |
| `sta_last_ch` | internal (v0.9.3) | 1-14, 36-64 / 100-144 / 149-177 (step 4) | - | written on STA connect when changed; AP channel at boot |
| `bw_5g` | `bw_5g` (v0.9.5) | 40 = HT40 / 20 = HT20 | 40 | boot (`opt_bw_5g()` -> `apply_bw()` for STA and AP; 2.4 GHz always HT20) |
| `sdio_mhz` | `sdio_mhz` (v0.9.5) | 40 / 50 | 40 | boot (`opt_sdio_begin()` -> `esp_hosted_sdio_set_config`), auto-fallback to 40 |
| `sdio_try` | internal | 1 = 50 MHz boot not yet confirmed | 0 | boot |
| `emerg` | `emergency_active` | 1 = emergency mode entered | 0 | read-only in API |

Removed (keys erased at boot, fixed values used): `napt` (always on, per AP profile NAT flag) and
`log_lvl` (Info) since v0.9.3; `tcp_kb` since v0.9.1. `lwip_core`, `napt_max` and `napt_tcp_to`
were removed in v0.9.3 and are settings again since v0.9.4, together with the new `tcp_wnd_kb`.

TCP window (v0.9.4): lwIP is built with `-include main/lwip_tune.h`, which redefines
`CONFIG_LWIP_TCP_WND_DEFAULT` (and so lwipopts.h `TCP_WND`) as `mypi_tcp_wnd`. In preprocessor
`#if` checks the expression evaluates to 131072 (the 128 KB maximum, allowed by `WND_SCALE` /
`RCV_SCALE=2`); in C code it is the boot value. Only TCP terminated on the P4 uses it.

Radio blob (`mypi` / `radio`, `wifi.c`): since v0.9.3 only `band_mode` and one protocol mask
shared by STA and AP are used (`sta_proto == ap_proto`; old blobs are merged with
`opt_proto_merge`: STA value if non-zero, else AP). The blob's `sta_bw` / `ap_bw` are ignored:
2.4 GHz is always HT20 and the 5 GHz width comes from `bw_5g` (v0.9.5, HT40 default), one value
for STA and AP since they share the radio. The C5 does 40 MHz only in 11n/11an, so HT40 drops
11ac/11ax on 5 GHz (v0.9.6); HT20 is the recommended setting (HW-tested faster than HT40). TX power comes from `tx_dbm`, DTIM is the driver default.

AP channel (`opt_ap_channel_pick`): current STA channel > current AP channel > `sta_last_ch`
if usable for the band mode / country > 1 (36 in 5 GHz-only mode). `ap.csa_count = 3`.

Boot: `main.c` calls `opt_config_load()` (applies log level). `wifi.c` initialises
ESP-Hosted manually (`CONFIG_ESP_HOSTED_AUTO_CALL_INIT_BEFORE_APP_MAIN` off) so the
SDIO clock from `opt_sdio_begin()` can be set before the transport starts.

SDIO clock (v0.9.5): `sdio_mhz` is used again. IDF v6.1's `sdmmc_io_enable_hs_mode()` caps an
SDIO card at 40 MHz (`SDMMC_FREQ_HIGHSPEED`), so `main/sdio_clk.c` wraps it
(`-Wl,--wrap=sdmmc_io_enable_hs_mode`) and raises the card limit to the host's configured clock
(max 50 MHz; the P4 makes exactly 50 MHz from its 200 MHz SDIO PLL). The real bus clock is read
back with `sdmmc_host_get_real_freq()`, logged at boot and returned as `sdio_real_khz`.

SDIO 50 MHz auto-fallback:
1. A 50 MHz boot writes `sdio_try=1` before the transport starts.
2. When the transport is up and `esp_wifi_init` succeeds, `opt_sdio_confirm()`
   writes `sdio_try=0`.
3. If the C5 fails at 50 MHz (`rescue_request`), `sdio_mhz=40`, `sdio_try=0` is
   saved and the device restarts without consuming the rescue restart budget.
4. If a 50 MHz boot hangs or crashes before confirmation, the next boot sees
   `sdio_try=1` and falls back to 40 MHz, saving it in NVS.

Emergency flag: set by the rescue task when emergency mode is entered (existing
second-failure logic unchanged); cleared automatically by `rescue_c5_init_ok()`
after a successful C5 init, or manually via the web UI.

Web API: `GET /api/opt`, `POST /api/opt` (returns `{"reboot_required": bool}`,
true when `ap_beacon_ms`, `country`, `tcp_wnd_kb`, `lwip_core`, `napt_max`, `bw_5g` or `sdio_mhz` changed;
GET also returns `sdio_active_mhz`, `sdio_fell_back` and `sdio_real_khz`), `GET/POST /api/radio` (`band_mode`, shared `proto`,
`last_sta_channel`), `POST /api/clear_emergency`,
`POST /api/reboot`. The Optimize tab uses only radio buttons, checkboxes and
dropdowns.

v0.9.1: the TCP window option (`tcp_kb`) was removed and the obsolete key is erased at boot
(v0.9.4 brings the window back as `tcp_wnd_kb`, now applied at runtime). The Wi-Fi
protocol / band / width controls moved from System to the top of Optimize; 2.4 GHz stays HT20.
