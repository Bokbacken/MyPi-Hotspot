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

LVGL migration is separate future work. It must preserve OTA safety, rescue access,
and existing firmware behavior, with regression and hardware validation before
release. Font selection, glyph coverage, display/touch integration, and migration
sequencing belong to that work; no new dependencies, UI refactor, or runtime
changes are introduced by this decision.
