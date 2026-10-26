
# NeonOS

NeonOS connects a BLE RGB light to Apple Home using an ESP32. An SSD1306 OLED and the onboard BOOT button provide local controls.

The ESP32 communicates with HomeKit over Wi-Fi and sends power, RGB and brightness commands to the light over Bluetooth Low Energy.

## Features

- Power, brightness and color control from Apple Home
- Seven RGB presets accessible from the OLED menu
- Native BLE brightness control independent of RGB values
- Automatic BLE reconnection and state replay
- OLED screen-off mode and 180° display orientation saved across restarts
- Local controls disabled while the screen is off, except for the wake gesture
- Dim GPIO2 LED breathing effect while Wi-Fi or BLE is disconnected

BLE writes are not physical-state confirmations. The interface shows the requested light state.

## Hardware

- ESP32-WROOM-32 development board
- SSD1306 128×64 I²C OLED (`0x3C`)
- BLE RGB light controller using service `FFF0` and characteristic `FFF3`
- Four jumper wires

### OLED wiring

| OLED | ESP32 |
| --- | --- |
| VCC | 3V3 |
| GND | GND |
| SDA | GPIO21 |
| SCL | GPIO22 |

The BOOT button uses GPIO0. The onboard blue status LED uses GPIO2. The power LED near USB is connected to the board's power circuit and cannot be dimmed by this firmware.

## Project structure

```text
neon-os/
├── neon-os.ino        Setup and main loop
├── config.h           Pin mapping, timing and BLE address
├── colors.h           RGB presets and color conversion
├── ble-bridge.h       BLE connection, commands and worker
├── homekit-light.h    HomeKit accessory and state handling
├── interface.h        OLED, BOOT button and status LED
├── README.md
├── LICENSE
└── .gitignore
```

The sketch and header files are kept together at the repository root.

## Installation

Install [Arduino IDE](https://www.arduino.cc/en/software) and the ESP32 board package, then install these libraries:

- [HomeSpan](https://github.com/HomeSpan/HomeSpan) 2.1.8
- [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) 2.x
- [Adafruit GFX](https://github.com/adafruit/Adafruit-GFX-Library)
- [Adafruit SSD1306](https://github.com/adafruit/Adafruit_SSD1306)

The project uses Arduino-ESP32 3.3.x and targets **ESP32 Dev Module**.

```bash
git clone https://github.com/amansanoj/neon-os.git
```

Open `neon-os.ino` from the `neon-os` directory in Arduino IDE.

1. Connect the OLED using the wiring table above.
2. Check the BLE controller address in `config.h`. The included address is specific to the original light.
3. Select **ESP32 Dev Module** and the correct serial port.
4. Set **Flash Size** to **4MB**, keep **PSRAM** disabled, and select **Partition Scheme → Minimal SPIFFS (1.9MB APP with OTA/128KB SPIFFS)**. The HomeKit/BLE firmware exceeds the default 1.2MB application partition.
5. Keep **Erase All Flash Before Sketch Upload** set to **Disabled** when updating an already configured device.
6. Upload the sketch over USB when changing the partition scheme (OTA cannot change the partition table), and open Serial Monitor at **115200 baud**.
7. On first setup, follow the HomeSpan serial prompts to configure Wi-Fi and pair the accessory in Apple Home.

Erasing the entire flash removes stored Wi-Fi credentials, HomeKit pairing information and local preferences.

## OLED interface

The 128×64 menu uses a small FreeSans heading and a compact bitmap font for the controls. The heading is left-aligned; labels sit on the left and their values are right-aligned.

```text
NeonOS
─────────────────────
● Power             On
○ Brightness       75%
○ Color          Purple
```

The menu uses **Power**, **Brightness** and **Color** without abbreviations. A filled circle marks the selected row. The startup screen shows the same `NeonOS` heading centered on the display.

The Color row shows the preset name when RGB matches a preset; otherwise it shows the RGB bytes, for example `Color    23,54,87`.

The custom heading uses `FreeSans9pt7b`, included with Adafruit GFX; the rows use the default 6×8 bitmap font. These are the intended UI settings for the latest `interface.h` changes.

### BOOT controls

| Action | Screen on | Screen off |
| --- | --- | --- |
| Single click | Select next row | Ignored |
| Double click | Change the selected setting | Ignored |
| Four clicks | Toggle display rotation between 0° and 180° | Ignored |
| Hold for 2 seconds | Turn off the OLED | Wake the OLED |

Click actions run after a 330 ms pause following the last release. Start each subsequent press within that window. Four clicks flip only the display; they do not change the selected light setting. Three clicks or more than four clicks are ignored. A long hold cancels pending clicks. Display orientation survives restarts.

The Brightness row cycles through **25%, 50%, 75% and 100%**. Color cycles through the presets below. Turning off the OLED doesn't interrupt Apple Home or the BLE bridge. Screen-off mode survives restarts.

Don't hold BOOT while resetting or powering on the ESP32. GPIO0 is a bootstrapping pin; doing so may start firmware-download mode.

## Color presets

| Preset | RGB |
| --- | --- |
| Warm | `255, 170, 84` |
| Neutral | `255, 224, 196` |
| Cool | `204, 219, 255` |
| Red | `255, 0, 0` |
| Green | `0, 255, 0` |
| Blue | `0, 0, 255` |
| Purple | `128, 0, 128` |

Presets are defined in `colors.h`. Local selections send their exact RGB bytes to the controller. Apple Home uses Hue and Saturation, which NeonOS converts to RGB. Brightness is sent separately using the controller's native brightness command.

## Validation

Run the native regression checks with Python 3 and a C++17 compiler:

```bash
python3 tests/run_native.py
```

These checks compile the actual color, HomeKit-light and interface headers against host substitutes. They cover preset RGB values and wraparound, single/double/four-click gestures, ignored click counts, bounce, long holds, screen-off behavior, timing boundaries and timer rollover. BLE worker simulations also check initial address-type discovery, retention of the working public/random address type after link loss, capped reconnect delays and periodic signal sampling. They do not emulate the ESP32, BLE controller, HomeKit network or physical OLED.

With the documented board package and libraries installed, compile using Arduino CLI:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32:FlashSize=4M,PSRAM=disabled,PartitionScheme=min_spiffs --jobs 4 .
```

Before release, verify on hardware:

- Cycle all seven presets through Blue and Purple and back to Warm; confirm the OLED and physical light agree.
- On each menu row, four-click BOOT and confirm only the display flips. Repeat to restore orientation, then reboot to verify it is retained.
- Check single/double clicks, hold-to-sleep/wake, and ignored clicks while asleep in both orientations.
- Check Apple Home power/brightness/color control and BLE disconnect/reconnect state replay after these gestures.

## BLE link troubleshooting

The ESP32 shares its 2.4 GHz radio between Wi-Fi and BLE. Keep its antenna end clear of the OLED PCB, metal, breadboard contacts and jumper wires; test with several centimetres of separation and the antenna projecting beyond the breadboard. If movement triggers a restart, also check the USB cable, supply and breadboard connections.

Serial logs report BLE transmit power at startup, negotiated connection parameters at connection, and BLE RSSI every five seconds while connected. The Wi-Fi RSSI printed by HomeSpan measures the access point, not the light controller. More negative BLE RSSI indicates weaker reception, but RSSI alone cannot identify interference or prove a reliable link. A zero/failed RSSI read is reported as unavailable.

`Disconnected reason=520 (0x208)` is a BLE connection supervision timeout: packets were not received before the negotiated deadline. It does not by itself identify the cause. Compare stationary operation near the light with the original placement, and check whether the controller remains powered and another phone/app is connecting to it.

Reconnects retain the address type after the first successful connection. Before that, public/random types are tried alternately. The retry delay backs off from 750 ms to a maximum of five seconds, in addition to each connection attempt's timeout. Successful reconnection replays the requested light state. Firmware logs and host tests cannot establish physical BLE range; verify placement and recovery on the device.

## Notes

NeonOS is built for a MELK-style controller using writable BLE characteristic `FFF3` in service `FFF0`. Other RGB light controllers may use different commands or services.

The optional HomeSpan `/status` web log is not enabled. Serial output remains available for troubleshooting.

The firmware has not been independently compiled or validated on every supported device. Check behavior on your own controller before relying on it for unattended use.

## License

[MIT](LICENSE)
