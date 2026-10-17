# MyPi-Hotspot — project status

Snapshot: 2026-10-16, firmware **v0.7.0**. See `CHANGELOG.md` for the per-version list
and `README.md` for wiring, flashing and usage.

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
- **11ax is disabled** until a new C5 radio firmware fixes ESP-IDF #19238
  (TCP TX hangs). Focus is b/g/n/a/ac. **Max bandwidth 40 MHz.**
- ESP-IDF **v6.1**, ESP-Hosted **3.0.9** on both host and C5 slave.
  ESP-Hosted v2 and v3 RPC are incompatible; host and slave must match.
- The C5 is updated permanently through `/c5update` (flash path A).
- The P4 does not reboot when the C5 link fails; emergency mode takes over instead.
- All rates are Mbit/s with 2 decimals (`*_mbps`). Old CSV files with `*_bps` are bytes/s.
- Preferred flashing: OTA via `/update` (keeps NVS). A merged binary at 0x0 likely
  erases NVS (profiles, settings).

## Buffer findings (basis for the next tuning round)

- The P4 decides most of the C5's Wi-Fi buffers: the slave builds its
  `wifi_init_config_t` from values sent by the host over RPC. The C5's own
  `ESP_WIFI_*` buffer settings are mostly ignored; SDIO queues, SW aggregation and the
  TX BA window on the C5 still apply.
- Hypothesis: with `SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y` IDF forces static TX buffers, so
  `WIFI_RMT_STATIC_TX_BUFFER_NUM=16` is what is used, not the dynamic 128. This may
  limit uplink/AP TX. Verify via the C5 boot log (`default | host | final` table).
- Baseline to compare against (v0.5.3, ch36 STA, ch1 AP, 20 MHz/ac):
  STA RX 16.32 / TX 20.76, AP RX 20.61 / TX 16.26 Mbit/s (peak).

## Lessons learned

- Power the LCD module from 3V3 only; 5V on VCC plus 3V3 caused a dark screen and
  touch I2C timeouts.
- The CST816D gesture register ignores rotation; use rotated touch coordinates instead.
- Avoid strapping pins 34–38 for peripherals; LCD SPI moved to GPIO23/GPIO5.
- OTA over a 128K TCP window needs a larger recvmbox, otherwise uploads stall.

## Regression checklist (run for every new build)

All builds up to v0.7.0 have passed on hardware. Repeat this list after each change.


1. Flash via `/update`; confirm the new version on the status page.
2. Emergency mode: swipe down within 10 s → emergency screen with 5 buttons.
3. DHCP option with a cable to a router → IP shown on LCD, web UI reachable.
4. Static `192.168.4.32` with a laptop set to `192.168.4.x/24` directly on the port.
5. Switch between options while emergency mode is active.
6. Simulate a dead C5 (hold it in reset at boot) → emergency mode within ~30 s.
7. OTA progress view on the LCD for both `/update` and `/c5update`.
8. Run iperf (port 5001) and compare with the v0.5.3 baseline and the previous build.

## Next steps

- Throughput tuning, experiment #1: `WIFI_RMT_STATIC_TX_BUFFER_NUM` 32–64 on the P4,
  measured with iperf against the baseline.
- Further buffer/queue experiments based on the buffer findings above.
