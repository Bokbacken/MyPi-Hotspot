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

## First LVGL migration increment

LVGL 9.4.0 is now a pinned ESP-IDF managed dependency. The default firmware still
uses the existing UI. In `idf.py menuconfig`, enable **MyPi-Hotspot → Display →
Experimental MyPi LVGL home status viewport** (`CONFIG_MYPI_UI_LVGL`) to evaluate
the first read-only region. Build and flash/OTA using the existing workflow;
there are no partition, endpoint, networking, profile, or update-format changes.
This is MyPi platform groundwork, not a replacement Hotspot UI or a
hardware-validated release.

### Boundaries

- `main/mypi_ui/state.h` is the hardware/framework-independent entry point:
  borrowed UTF-8 status lines with semantic presentation colors, no credentials
  or backend commands. Product code owns the snapshot until rendering returns.
- `main/mypi_ui/lvgl_view.*` copies that state into LVGL labels. It knows neither
  ESP-IDF nor Hotspot, and sizes itself to its parent so future Arduino or other
  ports can reuse it on different screens.
- `main/ui_lvgl_port.*` alone connects LVGL to the current display framebuffer
  and ESP timer. Its partial RGB565 buffer is 9,600 bytes. The existing display
  driver retains rotation, locking, SPI/DMA completion and wire byte swapping.
  Flush-ready means pixels have been copied into the framebuffer; the existing
  caller flushes that framebuffer to the panel afterwards.
- `main/display_ui.c` adapts existing public backend getters to the shared state.
  Only the home status area (y=32..231) uses LVGL when enabled. The canonical logo,
  8N1 serial dividers, status header, menu button, other pages, OTA and rescue
  continue using the existing paths. Failure to create the LVGL display falls back
  to the existing home view; this is not a general out-of-memory recovery guarantee.

The existing `display_ui` task owns **all** LVGL calls, including init/deinit.
The opt-in build gives it an 8 KiB stack instead of 4 KiB. Rendering is synchronous
via `lv_refr_now()` at the existing status cadence, with a full viewport invalidation
on each render because legacy pages may have overwritten it. No new task, timer
handler, animations, or LVGL input device is introduced yet. Touch remains owned
by the existing main loop; do not poll the controller a second time from LVGL.
OTA and rescue retain precedence in that render loop, including the boot rescue
swipe.

UTF-8 bytes are passed to `lv_label_set_text()` without normalization, transliteration,
or byte-based abbreviation. Long labels are visually clipped, not rewritten.
The initial LVGL default font does **not** cover all Unicode glyphs; missing-glyph
boxes are a known evaluation limitation, not a change to stored/API text.

### Next increments and validation

1. Hardware-test both build configurations: home/menu/back, rotation, boot rescue
   swipe and options, P4/C5 OTA progress/failure, and unchanged web/API behavior.
2. Choose shared fonts with explicit glyph coverage, then migrate the identity
   header/dividers and navigation widgets without changing their meaning.
3. Feed rotation-corrected press/release samples from the existing touch owner
   through a thin adapter to LVGL, preserving rescue interception. Add regular
   tick/timer handling on the same UI owner before introducing interactive widgets.
4. Migrate one functional page at a time; keep backend jobs/commands outside the
   renderer. Remove legacy rendering only after parity and hardware validation.

`tests/test_mypi_ui.c` exercises the shared view using real LVGL at two viewport
sizes, including UTF-8 byte preservation, copied snapshot lifetime, row clearing,
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
  -Imain -I<lvgl-source> tests/test_mypi_ui.c main/mypi_ui/lvgl_view.c \
  /tmp/mypi-lvgl-host/lib/liblvgl.a -lm -o /tmp/test_mypi_ui
/tmp/test_mypi_ui
```

Existing C, JavaScript and logo tests remain applicable; hardware validation is
still required before release.
