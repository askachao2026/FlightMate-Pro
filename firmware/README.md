# Firmware

| File | Size | SHA-256 |
| --- | ---: | --- |
| `FlightMatePro-20261020-merged.bin` | 2,007,424 bytes | `b8c39b60dc835c8713a35065d06f9833c38957343269349dace462ecf49c06c3` |

- Target: ESP32-C3, 8 MB Flash
- ESP-IDF: 5.5.3
- Application image: 1,941,888 bytes
- Flash offset: `0x0`
- Merge image includes bootloader, partition table and application.

```bash
python -m esptool --chip esp32c3 --port COM19 --baud 460800 \
  write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB \
  0x0 FlightMatePro-20261020-merged.bin
```

The merged image overwrites NVS settings and local logs. See `docs/FLASHING.md`.
