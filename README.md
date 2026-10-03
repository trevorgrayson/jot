# Jot

Capture short notes offline and deliver them to txtin.gs when configured Wi-Fi is available. Cardputer and standalone CardKB2 also offer a separate BLE HID keyboard mode.

## Repository layout

This repository contains the Jot product firmware, a shared note/queue engine, hardware-specific adapters, and the documents used to steer the CardKB2/OLED work.

| Path | Purpose |
|---|---|
| `platformio.ini` | Top-level PlatformIO entry point for all supported builds. `m5stack-cardputer` and `cardkb2-host` are Arduino builds; `cardkb2` redirects into the ESP-IDF project in `firmware/cardkb2`. |
| `src/main.cpp` | Arduino firmware for the M5Stack Cardputer and the ESP32-S3 external CardKB2 host build. It handles UI, Wi-Fi delivery, LittleFS storage adapters, and Cardputer mode switching. |
| `src/CardputerHid.h` | BLE HID keyboard adapter for the Cardputer Arduino build. |
| `lib/JotCore/src/` | Hardware-independent Jot engine. It owns note editing rules, queue limits, JSON payloads, crash-tolerant queue snapshots, and storage/network abstractions used by both firmware families. |
| `include/jot_config.example.h` | Example private build configuration for Wi-Fi networks, API URL/token, CA certificate, and OLED address. Copy to ignored `include/jot_config.h` for real credentials. |
| `firmware/cardkb2/` | Standalone CardKB2 ESP32-C61 firmware. This is an ESP-IDF project based on M5Stack OEM CardKB2 firmware, extended with Jot mode, OLED output, BLE mode handling, and factory transport preservation. |
| `firmware/cardkb2/main/app/` | CardKB2 application state machine: mode selection, shortcut handling, RGB state, key repeat, and factory/Jot/BLE transitions. |
| `firmware/cardkb2/main/bsp/` | CardKB2 board support adapted from OEM code: keyboard scanner, RGB LED, I²C peripheral mode, UART mode, ESP-NOW mode, and BLE HID. |
| `firmware/cardkb2/main/jot/` | CardKB2 Jot adapter: LittleFS mount, Wi-Fi worker, OLED SSD1306 driver on G26/G25, Jot key routing, and serial diagnostics. |
| `firmware/cardkb2/partitions.csv` | ESP32-C61 4MB partition table. It keeps NVS/PHY regions and adds a 1MB `jot` LittleFS partition at `0x300000`. |
| `scripts/cardkb2_project.py` | PlatformIO pre-script that makes the top-level `cardkb2` environment build the nested ESP-IDF project. |
| `test/core_test.cpp` and `test/run-native.sh` | Host-side tests for `JotCore`, including capacity, torn writes, migration, failed saves, acknowledgements, JSON escaping, and hold timing. |
| `docs/PRD.md` | Product requirements and acceptance criteria for Jot, BLE HID mode, standalone CardKB2 support, and OLED behavior. |
| `docs/VALIDATION.md` | Validation record for builds, tests, and hardware acceptance gaps. |
| `device_messages.py` | Legacy/reference server script retained with the repo; deployed server behavior still needs separate validation. |
| `_/prd/` | A separate worktree/snapshot used for PRD-side development. It mirrors the ESP32-C61 build changes and can build `cardkb2` independently. |
| `_/mqtt/`, `cmake-build-debug/`, `.idea/`, `cscope.files` | Local scratch/editor/build artifacts in the working tree; they are not part of the main firmware flow. |

The important dependency direction is one-way: device adapters call into `JotCore`, while `JotCore` knows nothing about Cardputer, CardKB2, displays, BLE, Wi-Fi stacks, or filesystems. That split is what lets the same note behavior run on both Arduino and ESP-IDF targets.

## Build targets

| Target | Build | Hardware |
|---|---|---|
| Cardputer | `pio run -e m5stack-cardputer` | Built-in keyboard/display |
| External CardKB2 host | `pio run -e cardkb2-host` | ESP32-S3 M5 host plus I²C CardKB2 |
| Standalone CardKB2 | `pio run -e cardkb2` | ESP32-C61 CardKB2 plus 128×64 SSD1306 |

The `cardkb2` PlatformIO environment builds the keyboard's internal ESP32-C61 controller. The previous ESP32-S3 external-host build is now named `cardkb2-host`. Arduino builds use pinned pioarduino/Arduino 3.3.11 and library versions. Keep the existing host partition layout when upgrading with pending notes; do not upload a filesystem image over the queue.

Standalone firmware is in `firmware/cardkb2`. Its scanner, mappings, RGB indications, BLE HID implementation, and factory peripheral modes are adapted from M5Stack OEM code. See the [source revision and license](firmware/cardkb2/OEM.md). Cardputer uses Arduino's ESP32 BLE library through a separate adapter.

### Configuration

Copy `include/jot_config.example.h` to `include/jot_config.h`; edit Wi-Fi credentials, endpoint, optional bearer token, and OLED address. This private file is ignored by Git and shared by both builds. Multiple Wi-Fi entries are tried in order. Unconfigured builds support offline capture.

For HTTPS, provide a PEM trust root in `JOT_CA_CERT` (a C++ raw string works). Certificates are verified; an HTTPS endpoint without a configured trust root is not uploaded to. The default remains `http://txtin.gs/api/v1/messages` for compatibility.

POST bodies contain `device_id` and `messages`, with `id` and `text` per note. Batches contain up to five notes to limit RAM use. Retries preserve IDs. Delivery is at least once: the server must deduplicate IDs, including when it accepts a batch but the response is lost.

### Standalone CardKB2 build and restoration

PlatformIO installs ESP-IDF 5.5.5 and the ESP32-C61 tools for the `cardkb2` target automatically. For the alternative direct-IDF commands below, activate ESP-IDF v5.5.3 with C61 tools installed; that build has also been verified. The component manager resolves dependencies using `dependencies.lock`.

From this repository, PlatformIO builds the same ESP-IDF project:

```sh
pio run -e cardkb2
pio run -e cardkb2 --target upload --upload-port PORT
```

Alternatively, with the IDF environment activated:

```sh
idf.py -C firmware/cardkb2 set-target esp32c61
idf.py -C firmware/cardkb2 build
```

The partition table assumes the documented 4MB CardKB2 flash, retains the factory NVS region, and adds a 1MB LittleFS partition at `0x300000`. Before changing device firmware, confirm the flash capacity and back up the complete device:

```sh
python -m esptool --chip esp32c61 -p PORT flash_id
python -m esptool --chip esp32c61 -p PORT read_flash 0 0x400000 cardkb2-before-jot.bin
idf.py -C firmware/cardkb2 -p PORT flash monitor
```

Replace `PORT` with the actual serial device. These are instructions; no device was flashed during implementation. Do not erase flash during updates: it contains pairing, mode, sequence, and pending-note data.

Restore the complete backup with:

```sh
python -m esptool --chip esp32c61 -p PORT write_flash 0 cardkb2-before-jot.bin
```

Restoration replaces flash contents with the backup's state, including data from its backup date. Alternatively, build factory firmware using the upstream project's instructions for its pinned revision.

## Wiring and controls

Connect a **128×64, 5V SSD1306 I²C module**:

| CardKB2 Grove | OLED |
|---|---|
| GND | GND |
| 5V | VCC |
| G26 | SDA |
| G25 | SCL |

Default OLED address: `0x3C`; override with `JOT_OLED_ADDRESS` for `0x3D`. The 400kHz bus drives a status row, feedback row, and six scrolling editor rows.

Check SDA/SCL pull-ups for compatibility with 3.3V GPIO. A 5V VCC rating alone does not establish signal compatibility. Verify Grove voltage with the OLED operating under the intended power source, including USB if used. An absent or unresponsive OLED disables rendering without stopping the keyboard; a mode change/restart probes it again.

| Device | Control | Action |
|---|---|---|
| Cardputer | Fn+B | Toggle Jot/BLE |
| Cardputer in BLE | Fn+Shift+B held 3 seconds | Clear pairing |
| Cardputer in BLE | Fn+grave / Fn+; / Fn+, / Fn+. / Fn+/ | Escape / Up / Left / Down / Right |
| CardKB2 | Fn+Sym+1/2/3/4 | Factory I²C / UART / ESP-NOW / BLE |
| CardKB2 | Fn+Sym+5 | Jot |
| CardKB2 in BLE | Fn+Sym+0 held 3 seconds | Clear pairing |
| CardKB2 in BLE | Fn+1 / Fn+D / Fn+Z / Fn+X / Fn+C | Escape / Up / Left / Down / Right |

Disconnect the OLED before using CardKB2's wired I²C or UART modes: they share its Grove pins. The firmware releases OLED master I²C before starting a wired transport. ESP-NOW exclusively owns Wi-Fi in its mode. Jot networking is suspended in every other mode.

New installations default to Jot; upgrades retain existing valid factory mode selections. Selected modes survive restart. Session drafts survive mode switches. Release held keys after switching before typing again; a transition may wait for an in-flight upload to settle.

In BLE mode, pair `Jot-Cardputer-xxxx` or the OEM-style `CardKB2-xxxx` in host Bluetooth settings. Keystrokes go exclusively to the host. Disconnected keystrokes are discarded. After clearing pairing, forget the old host device entry before pairing again.

### Standalone CardKB2 mode-change debugging

Use the USB serial monitor at 115200 baud while testing mode changes:

```sh
pio device monitor -e cardkb2
```

The keyboard scanner prints raw key events with physical key indexes. A correct Jot shortcut press should include this sequence:

```text
KEY[33] Fn PRESS
KEY[34] Sym PRESS
KEY[ 4] '5' PRESS
KEY[ 4] '5' RELEASE
KEY[33] Fn RELEASE
KEY[34] Sym RELEASE
```

This confirms the scan matrix sees Fn, Sym, and the physical `5` key. It does not by itself prove the mode switch completed. The standalone firmware also prints plain serial breadcrumbs for the mode path because ESP-IDF log routing can be filtered while `printf` output remains visible. After flashing the diagnostic build, reset should print:

```text
BOOT: CardKB2 Start
JOT: device init
MODE: startup JOT (6)
```

Pressing `Fn+Sym+5` should then print:

```text
MODE: shortcut Fn+Sym+key[4] -> JOT queue=sent
MODE: queue received JOT while current is ...
MODE: change requested ... -> JOT save=true
MODE: enter hook JOT (6)
JOT: enter mode 6
JOT: OLED start SDA=26 SCL=25 address=0x3c
JOT: active true
MODE: change complete JOT transport=ESP_OK
Unit CardKB2 MODE: JOT
```

If the `KEY[...]` lines appear but `MODE: shortcut...` does not, the key events are reaching the scanner but the app state does not think Fn and Sym are both held when `5` arrives. If `MODE: shortcut... queue=sent` appears but `MODE: queue received...` does not, the mode queue or app loop is blocked. If `JOT: enter mode 6` appears and the OLED stays blank, check the OLED lines: `JOT: OLED probe failed...` means the keyboard entered Jot but did not find an SSD1306 at the configured address on G26/G25. If `JOT: active true` appears, text input should be routed to Jot; later keypresses should print `JOT: key ...`.

If absolutely nothing appears after reset, including `BOOT: CardKB2 Start`, verify that the flashed image is the ESP32-C61 `cardkb2` build, that the monitor is attached to the same serial port used for upload, and that the monitor baud is 115200. The upload log should identify the chip as ESP32-C61 and write the app at `0x10000`.

## Notes and persistence

Printable ASCII entry supports 512 bytes per note and 100 pending notes. Backspace removes the last character; Enter trims outside whitespace and saves. Blank submissions are ignored. A failed save or full queue retains the draft and displays feedback.

`lib/JotCore` alternates checksummed, generation-numbered queue snapshots. Writes are flushed and read back before confirmation; interrupted updates can fall back to the previous valid snapshot. Existing host `/messages.tsv` queues import when no journal exists, preserving IDs. The legacy file remains for recovery but is never imported again once a journal exists.

Only completely erased filesystem partitions are initialized automatically. Mount errors and corrupt journals never trigger formatting: submission/upload remain disabled until recovery. Back up the device before attempting recovery. Unsubmitted drafts survive mode changes, but not power loss.

## Validation

Run `bash test/run-native.sh` with GCC/Clang and sanitizers. Tests exercise capacity, failed saves, every torn-write boundary, restart, migration, acknowledgement during continued typing, JSON escaping, and hold timing.

All three targets compile. Physical pairing, electrical behavior, factory transports, interrupted-power recovery, and deployed-server deduplication remain release acceptance work. See [validation](docs/VALIDATION.md) and the [PRD](docs/PRD.md).
