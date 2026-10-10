# Jot validation

## Automated checks

| Check | Result |
|---|---|
| `bash test/run-native.sh` | Passed with AddressSanitizer and UndefinedBehaviorSanitizer |
| `pio run -e m5stack-cardputer` | Compiled |
| `pio run -e cardkb2-host` | Compiled for ESP32-S3; renamed external-host target |
| `pio run -e cardkb2` | Compiled with ESP-IDF 5.5.5, including the LVGL 9.2.2 OLED renderer (2026-10-09); C61 upload flags and Jot partition table verified |
| ESP-IDF v5.5.3: `idf.py -C firmware/cardkb2 build` | Compiled for ESP32-C61 |

Core tests interrupt a new snapshot write at every byte boundary and verify the previous note survives restart. They cover failed acknowledgement writes, preserving notes submitted during an upload, migration without resurrecting legacy notes, invalid storage, capacity, JSON escaping, and one action per hold across timer wraparound.

The LVGL display adapter also passed a temporary native harness using the real LVGL 9.2.2 renderer and simulated I²C. It checked blank frames, cursor visibility on the first/last rows, the sixteenth character, no refresh for unchanged text, absent OLEDs, transmit failures, and 30 start/stop cycles with a 16KB LVGL pool. The harness and logs are in `/tmp/jot-lvgl-check` for this workspace session. These checks do not validate physical OLED output, electrical behavior, or device stack/heap margins.

## Physical acceptance — pending

Successful compilation does not satisfy these checks. Record device, firmware, desktop OS, and mobile OS versions when executing them.

- Compose, delete, trim, submit, and restart on both primary devices. Hit the 512-byte/100-note limits and confirm feedback retains rejected drafts.
- Remove power during submission and acknowledgement. Confirm a complete previous/new snapshot loads and storage is never automatically formatted after corruption.
- Simulate unavailable storage and failed writes. Confirm drafts/pending notes survive and BLE remains usable.
- Test multiple Wi-Fi credentials, unavailable networks, timeouts, non-2xx responses, recovery, and typing during blocked uploads. Verify trusted HTTPS succeeds and untrusted certificates fail.
- Against the deployed service, repeat a batch using stable IDs and simulate losing a successful response; confirm one stored note per device/message ID.
- Pair each primary device with desktop and mobile hosts. Test printable keys, available modifiers, Enter, Backspace, Escape, arrows, restart/reconnection, pairing reset, and replacement hosts.
- Type while disconnected and hold keys across reconnect/mode changes. Confirm no replay, stuck keys, or shortcut leakage into host/editor.
- Switch repeatedly during uploads; preserve the session draft and pending notes, and acknowledge successful batches only. Confirm remembered mode survives restart.
- On CardKB2, verify Aa one-shot/locked/held capitalization, Sym toggle, factory navigation, RGB indications, I²C address/version register, UART frames, ESP-NOW broadcasts, and production-test routing. Upgrade from each factory mode and confirm numeric mode compatibility.
- Disconnect OLED for wired modes; reconnect before selecting Jot/BLE. Confirm exclusive pin ownership, absent/NACK behavior, layout/scrolling, 0x3C/0x3D configuration, and readable feedback.
- Measure Grove power with the operating OLED and verify SDA/SCL signal compatibility under USB/intended power.
- Fill all 100 slots with maximum-length notes and measure free heap/typing responsiveness during persistence and delivery on actual devices.
- Verify existing host device/message IDs survive migration. Exercise backup/restoration before releasing firmware.

## Release boundary

This is a firmware implementation candidate, not a hardware-qualified release. No device was flashed or test notes submitted to the deployed service during implementation. Build artifacts remain in ignored local build directories.
