# Jot PRD: portable note capture and BLE keyboard

## Product goal and current baseline

Jot is a small keyboard device for capturing short notes immediately, including while offline, and delivering them to txtin.gs when configured Wi-Fi becomes available. The next release also lets it act as a Bluetooth keyboard.

Present functionality in `src/main.cpp`:

- Compose text, delete characters, and press Enter to submit; whitespace-only notes are ignored.
- Limit each note to 512 characters and hold up to 100 pending notes.
- Store submitted notes in LittleFS; restore pending notes after restart.
- Assign stable device and message identifiers, then upload queue snapshots in the background without blocking typing.
- Try configured Wi-Fi networks, retry failed uploads, and remove a submitted batch after an HTTP 2xx response.
- Display Wi-Fi status, sending activity, pending count, recent pending-note previews, and the current input.
- Provide Cardputer firmware and an ESP32-S3 host build that reads an external CardKB2 over I²C.

Current limitations must be documented separately from requirements: drafts exist only in memory; storage failures are only logged; queue-full feedback is absent; Wi-Fi credentials are currently an empty source-defined list; HTTPS certificate verification is disabled. The included server file identifies itself as moved elsewhere and is a reference, not proof of deployed server behavior.

## Supported devices and modes

| Target | Display | Required modes |
|---|---|---|
| M5Stack Cardputer | Built-in display | Jot, BLE HID |
| Standalone CardKB2, ESP32-C61 | Attached 5V SSD1306, 128×64 | Jot, BLE HID, factory I²C, UART, ESP-NOW |
| Existing ESP32-S3 host with external CardKB2 | Host display | Preserve current Jot behavior |

The standalone CardKB2 target is distinct from the existing host build. It scans its own keyboard and requires no separate controller.

Use M5Stack’s published [factory firmware](https://github.com/m5stack/M5Unit-CardKB2-UserDemo) as the basis for preserving keyboard scanning, key mappings, RGB indications, and transport behavior. Its documented build uses ESP-IDF for ESP32-C61; the existing ESP32-S3 Arduino target cannot serve as the standalone firmware.

OLED wiring follows the [CardKB2 pin map](https://docs.m5stack.com/en/unit/Unit_CardKB2): G26 to SDA, G25 to SCL, 5V to VCC, and GND to GND. Hardware acceptance includes checking that the module’s signal voltage is compatible with the keyboard.

- OLED operation is required in Jot and BLE modes.
- I²C peripheral and UART modes reuse those pins. Disable the OLED driver before entering either mode and document that the OLED must be disconnected for wired peripheral operation.
- Preserve factory ESP-NOW behavior; no new display behavior is required in that mode.
- A missing OLED must not prevent BLE HID or factory keyboard modes from working.

## User experience and functional requirements

**Jot mode**

- Preserve the existing editing limits, submission flow, background delivery, and API compatibility.
- Enter confirms a note only after successful local persistence. On storage failure or a full queue, retain the input and display the reason.
- Submitted notes survive restart. An unfinished draft survives mode switches in the current session; draft recovery after power loss is outside this release.
- On the OLED, prioritize a status row showing mode, connectivity, and pending count; use the remaining space for a wrapped editor that scrolls to the newest text. Show submission and error feedback without obscuring the editor indefinitely.
- Retain recent pending-note previews on Cardputer where space permits; they are not required on the OLED.

**BLE HID mode**

- Send live keystrokes exclusively to the paired host. Do not capture or upload those keystrokes as notes.
- Support printable keys, Enter, Backspace, available modifiers, Escape, and navigation keys appropriate to each keyboard.
- Display advertising, connected, and disconnected states. Persist pairing information and reconnect to the previously paired host.
- Provide a documented local control to clear pairing and advertise for a replacement host.
- Release all pressed keys when leaving BLE mode or losing the connection; never replay disconnected keystrokes.
- Pause Jot uploads while BLE mode is active; resume pending delivery when returning to Jot.

**Mode selection**

- CardKB2 retains Fn+Sym+1/2/3/4 for its factory modes; add Fn+Sym+5 for Jot.
- Cardputer uses Fn+B to toggle Jot/BLE; consume the shortcut locally.
- Remember the selected mode across restart. New installations default to Jot.
- Clear BLE pairing with a three-second Fn+Sym+0 hold on CardKB2 or Fn+Shift+B hold on Cardputer.
- Mode controls must not type into the host or modify the Jot draft.

## Configuration, interfaces, and boundaries

- Keep configuration at build time: multiple Wi-Fi credentials, API endpoint, and optional bearer token. Provide an example configuration without committing real credentials.
- Preserve `POST /api/v1/messages` payloads containing `device_id` and `messages`, each with `id` and `text`; preserve stable identifiers across retries.
- Preserve existing identities and pending queues when updating existing supported builds. Give the standalone CardKB2 its own stable identity.
- Separate note/queue behavior from device keyboard, display, and transport adapters so both primary devices follow the same product rules.
- Require safe queue updates that do not acknowledge failed writes or discard pending notes during interrupted persistence.
- Defer mobile apps, on-device provisioning, cloud note browsing, queued-note editing, sending composed notes through HID, and simultaneous note capture plus HID typing.

## Acceptance and delivery

Deliver the PRD as `docs/PRD.md`, clearly distinguishing implemented behavior, new requirements, and known limitations. Implementation follows in stages: shared Jot behavior, standalone CardKB2 with OLED, BLE mode on both devices, then compatibility validation.

Release acceptance requires:

- Both primary devices capture and upload notes, remain responsive during network failures, and restore submitted notes after restart.
- Queue-full, 512-character limit, storage failure, retry, and interrupted-write scenarios preserve user input or pending notes as applicable.
- Retry testing against the actual server confirms duplicate submissions do not create duplicate notes.
- BLE pairing, reconnection, replacement pairing, modifier handling, disconnects, and mode changes pass on a desktop host and a mobile host.
- Mode persistence and draft retention during switching pass on both devices.
- CardKB2 factory transports and shortcuts remain functional; wired modes pass with the OLED disconnected.
- The OLED layout is readable at 128×64, with missing-display handling verified.
- Existing Cardputer and external-CardKB2 host builds still compile and retain their established behavior.
- Provide reproducible builds, wiring instructions, mode controls, and a factory-firmware restoration procedure. Hardware behavior remains unverified until exercised on physical devices.
