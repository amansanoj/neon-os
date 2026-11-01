# NeonOS

An ESP32-WROOM-32 bridges a BLE RGB light to Apple Home, with a 128×64 SSD1306 OLED and BOOT-button controls. The firmware includes a HomeSpan-independent, signed HTTPS update client and boot-health rollback.

**Deployment status:** the firmware builds and has native regression/fault tests. Real-device OTA, recovery, memory headroom and prolonged operation still need the [hardware acceptance checks](docs/OTA.md#hardware-acceptance). Network updates are disabled until your signing public key is configured. Releases are hosted on the public GitHub repository; no custom server or domain is needed. No production signing key or release is provisioned by this repository.

## Features

- Apple Home power, brightness, hue and saturation control over Wi-Fi
- BLE FFF0/FFF3 commands, reconnect backoff, cached service discovery and requested-state replay
- Seven exact RGB presets; other colors display numeric RGB values
- Persistent OLED sleep and 180° rotation
- Coalesced light-state persistence instead of flash writes for every slider sample
- Dirty-only OLED transfers and bounded I²C waits
- Main-loop watchdog, BLE/update-worker supervision and periodic memory diagnostics
- Two-slot, signed HTTPS OTA with trial validation, rollback and failed-release quarantine

BLE writes acknowledge requested commands, not the physical light state. Range depends on antenna placement, the controller, interference and power quality.

## Hardware and wiring

- ESP32-WROOM-32 development board, **4 MB flash**, no PSRAM
- SSD1306 128×64 I²C OLED at `0x3C`
- Compatible BLE RGB controller with service `FFF0` and writable characteristic `FFF3`

| OLED | ESP32 |
| --- | --- |
| VCC | 3V3 |
| GND | GND |
| SDA | GPIO21 |
| SCL | GPIO22 |

BOOT is GPIO0. The dim status LED is GPIO2; it breathes while Wi-Fi or BLE is disconnected. The board's separate power LED is not controlled by firmware.

Check `C::MAC` in [config.h](config.h) against your light. On a 400-contact breadboard, orient the antenna end outward and, where possible, over the breadboard edge. Put the OLED and jumpers toward the USB end rather than next to the antenna. Keep USB/programming access available for recovery. Use a stable power supply and mechanically secure the wiring before mounting.

## Build and first installation

For copy/paste commands on macOS, use [Mac setup, rebuild and release commands](docs/MAC_COMMANDS.md).

Pinned, tested dependencies:

| Component | Version |
| --- | --- |
| Arduino CLI, Linux x86-64 tooling | 1.3.1 |
| Arduino-ESP32 / underlying ESP-IDF | 3.3.12 / 5.5.5 |
| HomeSpan | 2.1.8 |
| NimBLE-Arduino | 2.3.7 |
| Adafruit GFX | 1.13.0 |
| Adafruit SSD1306 | 2.5.17 |
| Adafruit BusIO | 1.17.4 |

In Arduino IDE, open `neon-os.ino` and select **ESP32 Dev Module**, CPU **240 MHz**, flash **4 MB**, **No FS 4MB (2MB APP x2)**, PSRAM **Disabled**, and **Erase All Flash Before Sketch Upload: Disabled**. The included `partitions.csv` defines the same two-slot layout and is authoritative for this sketch. Upload speed 115200 is fine.

Install initially over USB, especially when changing partition layouts. OTA cannot change this firmware's partition table. Leave flash erasure disabled to preserve Wi-Fi, HomeKit pairing and preferences. Serial Monitor uses 115200 baud with newline line endings; first-time Wi-Fi/HomeKit setup uses HomeSpan's prompts.

For the cloud environment / Linux x86-64:

```bash
bash tools/setup.sh
python3 tools/build.py
```

`NEON_TOOLS_DIR` selects an external toolchain cache (default `/workspace/.neon-tools`). The setup script verifies the CLI checksum and uses registry package checksums; it does not disable TLS/package verification. Builds export application/bootloader/partition artifacts under ignored `dist/`. `tools/build.py --production` additionally requires a configured non-example OTA endpoint.

The selected layout provides **2,031,616 bytes per app slot**. Settings live in NVS; there is no filesystem. Firmware size and binary identity are checked after every scripted build. Use **only `neon-os.ino.bin`** as the OTA application image, never the merged image, bootloader or partition table.

## Permanent installation and updates

Read [OTA deployment and recovery](docs/OTA.md) before mounting. The updater uses ESP-IDF's HTTPS client and native OTA partition APIs directly; it does not depend on HomeSpan OTA or ArduinoOTA. Retaining the Arduino build preserves library compatibility without requiring an ESP-IDF project migration. The framework's rollback-enabled bootloader is required and Arduino's early auto-validation is disabled by `SpanRollback.h`.

Approved releases are served from GitHub Releases at `https://github.com/amansanoj/neon-os/releases/latest/download/release.ota`. Each stable release carries `firmware.bin` and `release.ota`; the signing tool pins each image to its exact release tag. The downloader follows bounded HTTPS redirects to GitHub asset storage with certificate verification on every hop. The device checks automatically, downloads into the inactive slot, verifies a signed manifest and image hash/identity, and only then switches boot slots. No inbound update port is opened. Normal checks are approximately every 15 minutes; failed checks retry with backoff. All future firmware must retain the updater and health protocol.

The release signing key stays off the ESP32 and outside Git. This software signature verification is **not hardware Secure Boot**; no eFuses are programmed. See the deployment guide for signing, trust-key rotation, GitHub publication and additional hardware security considerations.

## OLED and BOOT controls

```text
NeonOS
─────────────────────
● Power             On
○ Brightness       75%
○ Color          Purple
```

The Color row displays a preset name for an exact RGB match; otherwise it displays a value such as `23,54,87`. The longest value `255,255,255` fits beside the unabridged Color label.

| Action | Screen on | Screen off |
| --- | --- | --- |
| Single click | Select next row | Ignored |
| Double click | Change the selected setting | Ignored |
| Four clicks | Toggle 0° / 180° orientation | Ignored |
| Hold 2 seconds | Sleep the OLED | Wake the OLED |

Each following press must begin within 330 ms of the preceding debounced release. The action resolves after the click window closes. Three clicks or more than four clicks are ignored; a long hold cancels pending clicks. A four-click gesture never changes light settings. OLED state and rotation survive restarts. Do not hold BOOT while resetting/powering on: GPIO0 can enter the chip's serial-download mode.

Brightness cycles through 25%, 50%, 75%, 100%. Color cycles through:

| Preset | RGB |
| --- | --- |
| Warm | 255,170,84 |
| Neutral | 255,224,196 |
| Cool | 204,219,255 |
| Red | 255,0,0 |
| Green | 0,255,0 |
| Blue | 0,0,255 |
| Purple | 128,0,128 |

The title uses Adafruit GFX's FreeSans9pt7b; rows use its 6×8 bitmap font. Screen sleep affects only the display/local clicks, not HomeKit, BLE or update checks.

## State persistence and diagnostics

Light state is saved after 5 seconds without changes, at most once per minute, with an extra flush before a verified OTA reboot. Sudden power loss can therefore lose recent light changes. The versioned record preserves exact preset RGB values. The first v2 boot reads old HomeSpan CHAR values and the old preset key; existing pairing, accessory IDs and OLED preferences remain in place. Rolling back to pre-v2 firmware can restore its older saved light values because that firmware does not understand the new state record.

Health logs report free/minimum heap, largest free block and main/BLE/OTA task stack headroom once per minute. BLE RSSI is sampled once per minute while linked. HomeSpan's Wi-Fi RSSI describes the access point, not the BLE light. `reason=520 (0x208)` is a supervision timeout, not proof of a particular range problem. BLE address types are tried alternately until one works, then retained. Reconnect delays cap at 5 seconds plus the connection-attempt time.

OLED absence and a powered-off light are degraded operation, not automatic boot failures. The main task and BLE/update workers must continue making progress. I²C transfers are time-bounded. [The audit report](docs/RELIABILITY.md) records changes and remaining limits.

## Validation

```bash
python3 tests/run_native.py
uv python install 3.12
bash tools/setup-native.sh
uv run --python 3.12 --with-requirements tools/requirements.txt python tests/run_security.py \
  --mbedtls-include /workspace/.neon-tools/native-mbedtls/mbedtls-3.6.6/include \
  --mbedtls-lib-dir /workspace/.neon-tools/native-mbedtls/mbedtls-3.6.6/library
```

The security runner executes the actual updater against deterministic HTTP/flash/NVS mocks, linked to **real Mbed TLS 3.6.6**. It tests cryptographic interoperability, state transitions and rejection paths. It does not emulate RF, TLS on the ESP32, its bootloader or physical flash interruption. GitHub Actions validates tests and compiles a disposable OTA-enabled test profile; it does not sign or publish production releases.

## License

[MIT](LICENSE)
