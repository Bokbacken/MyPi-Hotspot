# MyPi interface and next steps

This document separates the Hotspot interface work in this branch from ideas that
still need design, implementation, and testing. The UI changes have not been run on
the ESP32-P4 display or on device hardware; they are not evidence of a hardware test.

The accepted [MyPi platform direction](ARCHITECTURE.md) governs future migration:
LVGL for supported local graphical interfaces, UTF-8 text input/output, shared
identity, and preserved backend/API contracts. The implementation notes below
describe the transitional UI, not an LVGL implementation.

## Implemented in this Hotspot update

- The 240×320 ST7789 LCD home screen shows uplink state, AP/client state, Ethernet,
  traffic, and a web-management address separately from the active uplink address.
- The local menu opens Wi-Fi uplink, AP, clients, traffic, Ethernet, and system/display
  views. Leaving the AP view does not stop the AP. Stopping it requires a separate
  confirmation because connected clients will be disconnected.
- The Wi-Fi flow scans in a worker task, lets the user select a network, then uses a
  character wheel to save and request a connection. Saved networks can be selected
  directly. The wheel masks the password, supports printable ASCII and Latin Unicode
  code points U+00A0–U+024F, and encodes selected characters as UTF-8. It accepts
  WPA passphrases of 8–63 UTF-8 bytes and has a separate open-network action. No
  password is drawn, returned by an API, or logged by this flow.
- Normal-screen touch coordinates use the existing rotation-corrected input. OTA
  retains touch priority, and the existing boot rescue swipe and rescue screen remain
  ahead of normal menu handling.
- Web pages use local, offline CSS/JavaScript shared by the dashboard, P4/C5 OTA, and
  pins pages. All pages share one header (logo, `Hotspot`, right-aligned STA signal in
  dBm from `/api/status`), a `Hotspot` divider generated from ASCII bytes framed as 8N1
  UART data, and page buttons. Since 0.8.3 the dashboard is one HTML shell with
  hash-routed pages (`/#overview`, `#wifi`, `#access-point`, `#traffic`, `#ethernet`,
  `#system`, `#debug`; most used first, technical/debug last) — no extra URI handlers.

The first scan result page shows up to eight networks; additional results require a
future paging/scrolling interaction. If all ten STA profile slots are occupied, use
the web interface to free a slot before saving a new network. The LCD font is ASCII;
for a selected non-ASCII wheel character it shows its `U+` code point while storing
the correct UTF-8 bytes.

## Visual identity and asset maintenance

The Hotspot implementation keeps the existing ESP-IDF C/`esp_lcd` renderer and uses
semantic RGB565 names in `main/ui_layout.h`. Shared web colors, spacing, controls, and
responsive rules live in `main/web_ui.css`; serial framing and SVG path construction
live in `main/web_ui.js`. ESP-IDF embeds these source assets through
`main/CMakeLists.txt`, so the checked-in CSS and JavaScript are the source of truth and
the firmware build reproduces the served assets without a separate generator, CDN, or
network dependency.

UART dividers are data, not decorative bit strings: each ASCII byte is framed by one
low start bit, eight data bits least-significant-bit first, and one high stop bit.
Since 0.8.3 both the LCD and the web divider encode the case-sensitive word `Hotspot`
(7 bytes = 70 bits; `H` is `0000100101` in left-to-right transmission order). On the
LCD the 70 bits span the full 240 px width (`mypi_uart_x_span()`, 3–4 px per bit).
Future MyPi projects should use their own identifying first letter/word and preserve
this framing rule.

The approved μ/π logo is checked in as `MyPi.png` and `MyPi-transparent.png` in the
repository root. `tools/render_header_logo.py` (Pillow) turns the transparent artwork's
alpha channel into the 16 px LCD mark and the 32 px web data URI in `main/header_logo.h`;
`tools/render_header_font.py` renders the antialiased LCD `Hotspot` title in
`main/header_font.h` (DejaVu Sans Bold, see `assets/DejaVu-LICENSE.txt`).
`tests/test_header_logo.py` checks that the generated header matches the artwork.

## Proposed cross-project policy

- Keep a small shared MyPi token and asset specification, then render it with each
  platform's display and web stack. LVGL is the chosen future local graphical UI
  direction on supported devices; identical screen geometry or a common application
  implementation is not required.
- Use the character-wheel keyboard on small local displays. Prefer a conventional
  full keyboard when a larger display has enough room and its input method makes one
  practical. Keep password entry masked, provide deliberate reveal only where useful,
  and preserve Unicode credentials without silent conversion or truncation.
- Keep the serial divider meaningful and project-specific. A project's initial uses
  the framed 8N1 character; longer web dividers frame each character in the project
  name. Generate patterns from bytes and keep bit-order/framing tests with the code.
- Keep persistent connectivity visible during navigation, distinguish management
  addresses from uplink addresses, and separate temporary action feedback from
  connection state. Navigation must not implicitly stop a service.
- Add readable text and focus/touch states alongside color, and keep web assets local
  so setup and recovery remain usable without Internet access.

## Validation ideas before a Hotspot release

1. Build with the supported ESP-IDF v6.1 environment and verify the generated
   `/ui.css` and `/ui.js` resources on the device.
2. Exercise the LCD at normal and 180-degree rotation: home, every menu row, scan
   selection, password-wheel boundaries, UTF-8 entry/backspace, profile-slot-full
   feedback, AP start/stop confirmation, and Back while AP clients remain connected.
3. Measure touch target comfort and text legibility on the actual 240×320 LCD. Confirm
   the management address is distinct and easy to find with and without an uplink.
4. Test Wi-Fi scan/connect latency, C5 unavailable behavior, existing profile password
   preservation, web polling during edits, mobile/desktop layouts, and both P4 and C5
   OTA pages.
5. Retest the existing OTA touch priority, all five Ethernet rescue choices, rescue
   entry window, rescue coordinate rotation, and C5-failure automatic rescue.

Host-runnable primitive checks (no ESP-IDF installation required):

```sh
cc -std=c11 -Wall -Wextra -Werror -I main tests/test_ui_primitives.c -o /tmp/mypi-ui-primitives
/tmp/mypi-ui-primitives
node --check main/web_ui.js
node tests/test_web_ui.js
```

## Deferred vision: Companion Wi-Fi emergency recovery

This is design work only; it is not implemented in Hotspot or Companion. Companion's
future recovery path should be Wi-Fi-oriented rather than reusing Hotspot's Ethernet
rescue. Design it to:

- require a deliberate, recognizable recovery entry gesture or boot-time choice;
- expose a clearly named recovery access point and an easy-to-find local management
  address, with an explicit explanation that normal connectivity is being repaired;
- let the owner inspect and repair saved Wi-Fi profiles without displaying stored
  credentials by default, writing them to logs, or requiring a destructive reset;
- make disruptive actions explicit and recoverable; and
- provide a safe, visible exit/reversion path that returns to normal Wi-Fi operation
  and confirms the result.

The SSID, address, entry gesture, timeout, profile-editing workflow, credential
protection, and failure/reversion behavior all need review and usability testing
before that separate Companion task is implemented.
