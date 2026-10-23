# quirc (vendored)

QR-code recognition library by Daniel Beer, ISC licence (see `LICENSE`).

- Upstream: <https://github.com/dlbeer/quirc>
- Imported: `lib/` at commit `927d680904dc95fdff4cd9d022eb374b438ff8f2` (2025-05-20), unmodified.
- Used by `main/qr_decode.c` to decode Wi-Fi QR codes from the MyPi camera on the device
  (no browser decoder, no external service).

Build notes: compiled with `QUIRC_FLOAT_TYPE=float` (the ESP32-P4 FPU is single precision) and
the default `QUIRC_MAX_REGIONS` (254), so the working image is one byte per pixel. `quirc_decode()`
keeps a ~9 KB structure on the stack; `main/qr_decode.c` therefore runs it in a dedicated task.
To update, copy `lib/*.c lib/*.h` and `LICENSE` from a newer upstream commit and update the
commit above.
