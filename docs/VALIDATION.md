# Validation report

Validated locally on 2026-11-03 after separating the two PlatformIO projects. No physical ESP32, Uno, Bluetooth module, wheelchair, sensors or VESCs were connected or tested by the agent.

## Builds from the final project paths

| Check | Result |
|---|---|
| `pio run -d controller-firmware -e esp32dev` | PASS |
| `pio run -d reciever-firmware -e uno_receiver` | PASS |
| `pio run -d controller-firmware -e esp32dev -t buildfs` | PASS |
| `python3 controller-firmware/tools/website_size.py` | PASS |
| `python3 reciever-firmware/tools/run_host_tests.py` | PASS |
| `node --check controller-firmware/data/dashboard.js` | PASS |
| Python tooling syntax and final relative paths | PASS |
| Original root `data/` removed, website under controller project | PASS |
| Teensy reference SHA-256 unchanged | PASS |

ESP32 build: Espressif32 platform 8.0.0, Arduino ESP32 3.3.12. The compiler emits the framework's existing BluetoothSerial deprecation warning; it does not fail the build.

| Target | Firmware flash | Static RAM |
|---|---:|---:|
| ESP32, verbose direct-print and allocation-hook build | 1,889,426 / 1,966,080 bytes (96.1%) | 65,304 / 327,680 bytes (19.9%) |
| Uno, diagnostic build | 13,892 / 32,256 bytes (43.1%) | 821 / 2,048 bytes (40.1%) |

Static RAM excludes runtime task stacks, temporary packet buffers and dynamic allocations. The ESP32 app has 76,654 bytes of remaining app-partition space. The website uses its separate filesystem partition. Runtime free heap and observed timing maxima are available through diagnostics, but were not measured on hardware.

## Website filesystem size

| Deployed asset | Bytes |
|---|---:|
| `dashboard.css` | 10,084 |
| `dashboard.js` | 16,179 |
| `index.html` | 9,639 |
| **Total raw assets** | **35,902 (35.06 KiB)** |

There are no bundled images, fonts, framework files or CDN dependencies. Raw assets are well below the 80 KiB target.

The actual LittleFS image was built successfully with PlatformIO's filesystem tool, including real metadata and block overhead. It is 131,072 bytes, exactly matching the data partition parsed from the generated binary partition table at offset `0x3D0000`. The image is partition-sized/padded; its 128 KiB file size does not mean all filesystem space is used.

Diagnostic image SHA-256: `8d3696fd2f72a67b2d23a33e3f291a9180a131af87d056196ce973699bb8c36a`.

The image was packed successfully rather than estimating fit from raw source bytes. Exact runtime filesystem allocated/usable bytes depend on LittleFS metadata; `/api/status` reports `filesystemUsed` and `filesystemTotal` when mounted. This report does not invent an unmeasured free-space figure.

## Logic tests

The host test compiles the actual `reciever-firmware/src/main.cpp` with the same shared protocol headers and PacketSerial codec, replacing serial pins/time/EEPROM with test shims. It checks:

- CRC known vector and decoding actual generated, COBS-framed Uno telemetry.
- Changing synthetic sensor readings, simulated marker, VESC fault values and real stop status.
- Legacy drive normalization, invalid `-32768`, stop-clear, watchdog expiry and disconnected stopping.
- Valid applies, out-of-range/unknown/fractional/nonfinite configuration rejection and stationary-stop gating.
- Save/readback CRC, no writes for unchanged valid saves, choosing newer records, interrupted inactive-slot writes, corrupted newest-slot fallback and runtime defaults.
- Truncated/corrupted telemetry, invalid state, invalid duty, NaN rejection in valid fields, and safe handling of unavailable fields.

These are software tests, not EEPROM power-cut or serial timing measurements on an Uno. HTTP validation logic was reviewed and compiled; malformed HTTP requests against physical ESP32 hardware have not been exercised.

Earlier browser checks against a local synthetic API exercised live, stale, disconnected and unavailable-update display states. The later headless viewport audit was stopped at the user's request; no browser verification runs automatically in any build. The latest copy changes were syntax-checked and packed, and page appearance is being reviewed by the user.

## Remaining hardware checks

1. Upload firmware and LittleFS image to the ESP32; confirm captive portal, static routes and APIs from a phone/tablet.
2. Upload the Uno receiver, verify HC-06 baud/name/STATE wiring and connect with the existing physical controller button.
3. Confirm startup/disconnect/watchdog stop latching and physical stop assertion/clear with no motors attached.
4. Check the SIMULATED banner, four changing sensor readings and two VESC status groups; verify telemetry becomes unavailable after loss or stale reception.
5. Compare command gaps, packet rejection counters, receiver output logs and stop responsiveness while refreshing at 10/20/50/100 Hz and with slow/multiple clients. Start with 10 Hz receiver telemetry. Hardware UART is preferred over Uno SoftwareSerial for robust full-duplex communication.
6. Test HTTP missing/invalid token, duplicate/extra/missing args, nonnumeric/nonfinite/fractional/huge values, unknown IDs, unsafe motion/stop state, disconnect during a request and pending-command rejection.
7. Apply while stopped, save, reboot and read back; exercise both EEPROM slots, interrupted writes and defaults on hardware.
8. Measure free heap/task stack margin, Wi-Fi/Bluetooth coexistence and receiver watchdog timing. Existing Bluetooth discovery/connect calls and writes can block; software task separation is not a hard real-time proof.

Teensy motor tuning, physical calibration and EEPROM timing remain future integration work. The Teensy firmware reference was not edited.

The diagnostic pass was compiled for both targets; receiver host tests, JavaScript syntax checking and the generated LittleFS size check passed. No hardware or browser execution was performed during this pass. See [the diagnostic guide](DEBUGGING.md) for code-review findings and capture instructions. Timeout, polling, rendering recovery and Bluetooth control fixes were intentionally not implemented.
