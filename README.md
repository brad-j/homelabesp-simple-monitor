# Homelab Panel

A dedicated ESP32 touchscreen for read-only Docker and Beszel monitoring, with a security boundary for future guarded recovery actions.

## Hardware

Hosyond/LCDWiki **E32R40T**:

- ESP32-WROOM-32E / ESP32-D0WD-V3, dual-core 240 MHz
- 4 MB flash, no PSRAM
- 4.0-inch 320×480 TN TFT, ST7796S over SPI
- XPT2046 resistive touchscreen over shared SPI
- CH340C USB serial over USB-C
- PWM backlight on GPIO 27
- Common-anode RGB LED on GPIO 22/16/17
- MicroSD slot, mono speaker output, 3.7 V LiPo charging circuit

The UI runs in **landscape at 480×320**.

## Current status: production dashboard

The flashed and verified production firmware provides:

- LVGL 8 UI with a 480×20-line / 19.2 KB partial draw buffer
- Wi-Fi captive-portal provisioning with no compiled LAN credentials
- Docker host/workload state refreshed every 30 seconds
- Beszel system metrics refreshed every 60 seconds
- Current CPU, memory, disk, temperature, load, and network throughput
- Tappable system cards with 60-minute CPU/memory charts
- Overview, Systems, read-only Actions-boundary, and Device screens
- Backlight dim after 2 minutes, off after 10 minutes, and touch-to-wake
- Wi-Fi modem sleep disabled for predictable latency while USB-powered

The production build has been validated on the target hardware within its
available RAM and dual-OTA application-slot limits.

The Actions screen is intentionally read-only. Control requires a separate authenticated allow-list API, validated transport, scoped credentials, and hold-to-confirm interaction.

Hardware verification remains available as separate PlatformIO environments:

- `e32r40t` — display, mapped touch, backlight, and RGB diagnostic
- `touch-raw` — raw XPT2046 coordinates, pressure, and IRQ diagnostic

## Install

```bash
python3 -m venv .venv
.venv/bin/pip install platformio==6.1.19 esptool==4.12.0
```

## Configure, build, and flash

Create a local configuration file and set the two read-only snapshot URLs for
your network. `include/config.h` is ignored by Git.

```bash
cp include/config.example.h include/config.h
```

Production dashboard:

```bash
.venv/bin/pio run -e app
.venv/bin/pio run -e app --target upload
.venv/bin/pio device monitor -e app
```

Hardware diagnostic:

```bash
.venv/bin/pio run -e e32r40t --target upload
```

Raw touch diagnostic:

```bash
.venv/bin/pio run -e touch-raw --target upload
```

On first production boot, join the temporary network and select the panel's 2.4 GHz LAN:

The display shows a randomly generated SSID and independent password whenever
the provisioning portal starts:

```text
SSID:     HomelabPanel-<random>
Password: panel-<random>
Portal:   http://192.168.4.1/
```

Saved Wi-Fi credentials survive normal firmware uploads and power cycles. The panel reconnects and reloads both snapshots automatically.

## Power and shutdown

No graceful shutdown is required. The ESP32 has no conventional operating system, mounted writable application filesystem, or database to stop. Dashboard state is held in RAM, and Wi-Fi settings are written only during provisioning.

- **Turn off:** disconnect USB-C or switch off its USB supply.
- **Turn on:** restore USB power; startup and reconnection are automatic.
- **Convenient switching:** use an inline USB power switch, switched hub, or smart plug on the USB adapter.
- **Screen sleep:** dim at 2 minutes, backlight off at 10 minutes, touch to wake. The ESP32 remains online.
- **If a LiPo is connected:** removing USB may transfer power to the battery rather than shut the board down.

Do not remove power while flashing firmware, saving new Wi-Fi configuration, or performing a future OTA update. Normal runtime power loss is safe.

## Factory backup

The original 4 MB flash image was captured before the first project flash and
is stored locally as `backups/factory.bin`. Device images can contain embedded
keys, credentials, and proprietary firmware; the entire backup directory is
ignored except for checksum files. Never commit or publish the image.

```text
SHA-256 445f6fcb210442777f04028f3ad2022bd1c7231b5d9907309d8514f863978315
```

Restore only when necessary:

```bash
.venv/bin/python3 -m esptool \
  --port <serial-port> \
  --baud 460800 write_flash 0 backups/factory.bin
```

## Architecture

```text
Docker engines                 Beszel database
      |                               |
      | restricted collectors         | read-only snapshot
      +---------------+---------------+
                      |
                      | atomic, redacted JSON
                      v
           LAN-only snapshot service
                      |
                      | HTTP; no upstream credentials
                      v
                Homelab Panel
```

The deployment-specific endpoints live only in ignored `include/config.h`.
`include/config.example.h` documents the required URL macros.

The dedicated Beszel SSH key is restricted to the redacted snapshot command. The published metrics exclude users, tokens, host addresses, keys, alerts, and container records. A failed collector run leaves the last valid atomic snapshot in place.

The ESP32 stores no upstream administrator credentials. A future control API must use validated transport, scoped authentication, an operation allow-list, and on-device hold-to-confirm for destructive actions.

Current UI:

1. **Overview** — overall state, Docker host reachability, workload count, and active issues.
2. **Systems** — touch cards for configured systems showing CPU/memory. Tap one for CPU, memory, disk, hottest available temperature, load, RX/TX, and a 60-minute CPU/memory graph.
3. **Actions** — explicit read-only security boundary; no infrastructure controls exist yet.
4. **Device** — SSID, IP address, RSSI, uptime, free heap, and PSRAM status.

Production firmware uses LVGL with a 19.2 KB partial draw buffer sized for this no-PSRAM board. Networking uses direct REST; MQTT can be added later.

## ESP32 implementation notes

Applied lessons from [10 Useful ESP32 Concepts](https://www.etechnophiles.com/esp32-useful-concepts/):

- Wi-Fi modem sleep is disabled because the panel is USB-powered and values predictable request latency over power savings.
- The dual-OTA `min_spiffs` partition layout reserves two 1.875 MB application slots. OTA remains disabled until an authenticated, integrity-checked update path exists.
- LVGL uses a partial buffer. TFT DMA remains off until profiling shows display flushes are the bottleneck; enabling DMA speculatively would complicate buffer ownership.
- Network polling remains on the Arduino loop while LAN requests are short. A pinned FreeRTOS worker is the next step only if measured refreshes stall touch or rendering.
- Deep sleep is intentionally avoided. Backlight-only sleep preserves immediate touch wake and current network state.
- ESP-NOW, DAC, Bluetooth coexistence, and an async web server are not needed for this client-only appliance.

## Operations documentation

Collector deployment details and infrastructure-specific runbooks belong in a
separate private operations repository.

## License

[MIT](LICENSE)

## References

- [LCDWiki E32R40T documentation](https://www.lcdwiki.com/4.0inch_ESP32-32E_Display)
- [Amazon product listing](https://www.amazon.com/dp/B0FGJJ24S1)
