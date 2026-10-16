# ESP32-C5 co-processor firmware (ESP-Hosted 3.0.9 slave)

Based on `examples/wifi/apsta/cp` from esp_hosted 3.0.9, with the partition layout of the
ESP-Hosted 2.x C5 slave (table at 0xC000, OTA slots 0x1E0000) so the image can be installed
by host-driven OTA from the MyPi-Hotspot web UI (`/c5update`).

    idf.py set-target esp32c5 && idf.py build     # ESP-IDF v6.1

- `build/network_adapter.bin` -> upload at `/c5update` (P4 must run a 2.x host build, e.g. v0.5.5)
- full flash over USB: `idf.py -p PORT flash`

Note: a 3.x slave only talks to a 3.x host (MyPi-Hotspot v0.6.0+).
