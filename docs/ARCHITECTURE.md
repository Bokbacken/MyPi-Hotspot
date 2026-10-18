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
