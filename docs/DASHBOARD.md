# LEAN controller dashboard and Uno test receiver

This implementation targets the ESP32 controller and an Arduino Uno + HC-06 test receiver. `leanFirmware.cpp` is unchanged. No physical seat sensors or VESCs are needed: the Uno generates marked synthetic measurements for the dashboard. It never sends motor commands to motor hardware.

## Build and upload

From this project, with PlatformIO installed:

```sh
pio run -d controller-firmware -e esp32dev
pio run -d controller-firmware -e esp32dev -t buildfs
python3 controller-firmware/tools/website_size.py
pio run -d reciever-firmware -e uno_receiver
```

Upload to the selected board/serial port explicitly:

```sh
pio run -d controller-firmware -e esp32dev -t upload --upload-port YOUR_ESP32_PORT
pio run -d controller-firmware -e esp32dev -t uploadfs --upload-port YOUR_ESP32_PORT
pio run -d reciever-firmware -e uno_receiver -t upload --upload-port YOUR_UNO_PORT
```

The ESP32 and Uno now have separate PlatformIO project roots, `controller-firmware/` and `reciever-firmware/`. Their defaults are `esp32dev` and `uno_receiver`, respectively. Each compiles only its own `src/`. Both include `../shared/include`; preserve that sibling directory when copying into a repo. Open the desired firmware folder in VS Code for PlatformIO. The Teensy reference is excluded from both projects.

Join `Wheelchair-Controller` Wi-Fi and open `http://192.168.4.1` if a captive portal does not open. The AP name and captive DNS behavior are preserved. Firmware upload includes the partition table; upload the filesystem again when changing layouts. The filesystem remains LittleFS even though the bundled partition CSV is named `min_spiffs.csv`. A missing filesystem does not prevent the status API or Bluetooth controller from starting. Filesystem mounting never automatically formats storage.

## Uno wiring and startup

- HC-06 TX to Uno pin 2 (SoftwareSerial RX).
- Uno pin 3 (SoftwareSerial TX) to HC-06 RX with the level conversion required by your module.
- HC-06 STATE to Uno pin 4; common ground and the module's required supply.
- HC-06 baud must be 115200 and its advertised name must match the existing ESP32 target, `LEANReciever` (spelling preserved).

Use the ESP32's existing physical connect button to connect/disconnect. The browser has no drive, connect, disconnect, stop-clear, or remote-drive route.

The new test receiver starts with its soft stop latched, and latches it on disconnection or expired drive watchdog. This is deliberately stronger than the reference sketch, which cleared its latch on a dropped STATE pin. After connection, the ESP32 may initially show its local stop request clear while the receiver reports latched. Press the physical soft-stop button once to assert/synchronize the stop, and again to clear it for a drive-command test. A browser connection never clears the latch.

`setMotorPower()` remains a test output placeholder. Serial output at 4 Hz shows the effective left/right commands. The watchdog is refreshed only by valid drive packets or an explicit legacy clear packet; configuration and telemetry traffic do not keep drive alive.

## What is real and what is synthetic

Real ESP32 values: Bluetooth state machine, raw joystick ADC, rolling-average potentiometer ADC, normalized joystick axes, sensitivity, last transmitted drive commands and their age, software timing maxima, packet counters, free heap, and filesystem usage.

Real Uno values: Bluetooth STATE, effective received drive commands, soft-stop latch, watchdog behavior, configuration values and save acknowledgments.

Synthetic Uno values: front/back/left/right raw sensors; calibrated sensors; baseline counts; estimated weight; motor duty; left/right VESC electrical RPM, current, voltage, MOSFET temperature and fault code; battery voltage and percentage. Sensor values oscillate smoothly. Simulated VESC activity follows effective received commands, so the stop latch reduces synthetic duty/RPM/current to zero. Fault codes are zero in this fixture. Battery and weight are fixed fixtures. The SIMULATED bit is always set by the Uno. No synthetic load values are used to calculate drive output.

The dashboard displays zero as a valid reading. Missing groups and stale measurements become JSON `null` and UI dashes. Receiver state is not inferred from joystick position. A controller stop request is distinct from confirmed receiver stop status; neither is described as a verified hardware emergency-stop circuit.

## Execution and safety boundaries

The existing joystick mixing, drive bytes, button state machine, Bluetooth discovery/reconnect, display and LED behavior are retained. Joystick sampling also runs while disconnected, without transmitting drive commands. Per-drive debug printing was removed to reduce serial pressure. Repeated soft-stop packets are paced at the existing 10 ms command interval instead of flooding every loop; the initial physical stop request still sends immediately.

HTTP and LittleFS serving run in a dedicated FreeRTOS task on core 0. The Arduino controller task publishes a fixed-size snapshot through a short critical section; HTTP never calls Bluetooth or changes controller globals. PacketSerial reads directly from BluetoothSerial after control/stop transmission, with no stream wrapper or receive-byte budget. One configuration command can be queued/in flight. Additional writes are rejected busy. Partial PacketSerial frames are discarded across connection-state changes. The current diagnostic build contains verbose direct Serial printouts that affect timing.

Both the HTTP boundary and controller dispatch reject mutations unless there is fresh stopped receiver telemetry and a physical controller stop request. The receiver independently requires its own stop latch, zero effective outputs, and a connected STATE pin. Integer type, finite values, bounds, packet lengths, protocol versions, reserved bytes and CRC are checked. The watchdog can only be shortened from the original 250 ms maximum.

This separation avoids synchronous HTTP waits in the controller loop. It is **not a hard real-time guarantee**: the existing discovery/connect calls and BluetoothSerial writes may block, Wi-Fi/Bluetooth share resources, and Uno SoftwareSerial cannot guarantee full-duplex reception. At 115200 baud the Uno sends at most four bytes before checking incoming traffic again. Use the default 10 Hz receiver rate first, test packet loss and command gaps, and prefer a hardware UART for a future Teensy integration. Browser refresh supports 10/20/50/100 Hz without overlapping fetches; 50/100 Hz are experimental request targets, not proven measurement rates. Receiver telemetry supports 5–20 Hz for this Uno implementation. Invalid input and diagnostic traffic must never be repurposed as motor commands.

The existing AP is open and the Bluetooth setup retains its original security mode. A per-boot custom-header token and no CORS permissions protect against simple cross-origin form submissions; this is not user authentication or protection against an AP participant. The software is a bench-testing dashboard, not a validated production wheelchair configuration service. Harden access and validate motion safety before integrating real motors.

## Configuration and persistence

The shared registry in `shared/include/config_protocol.h` defines only parameters implemented by the receiver:

| ID | Key | Range | Default | Effect |
|---|---|---|---|---|
| 0 | `telemetry_hz` | integer 5–20 Hz | 10 | Synthetic feedback transmission rate |
| 1 | `watchdog_ms` | integer 100–250 ms | 250 | Maximum interval without a valid drive packet |

Apply updates receiver RAM; Save is separate. Restore defaults changes RAM only, followed by Save if desired. No calibration command is enabled because the Uno has no physical seat sensors. Motor gains, wheel directions, pin assignments, VESC IDs and Teensy tuning constants are not exposed as editable settings.

The Uno's EEPROM implementation uses 48 bytes, with two 24-byte records. Each record has magic `LCFG`, version 1, zero reserved bytes, generation number, two explicit little-endian IEEE-754 binary32 values and CRC-32. On boot, choose the valid record with the newest generation (wrap-safe comparison); if neither validates, use factory defaults without writing EEPROM. Unknown versions and bad bounds/CRC are invalid. No implicit migration is attempted.

Save writes the inactive slot. Its first magic byte is invalidated first and committed last. The active slot is retained until the new record validates. At most one byte is updated when the EEPROM controller is ready, after control reception/watchdog handling; unchanged bytes are skipped. Settings cannot be edited during a save. No write occurs for repeated Save of an unchanged valid record. A save aborts if stop is cleared or the link drops. SAVE acknowledges success only after readback validation. Test power-loss recovery on hardware before relying on it.

This bounded AVR EEPROM writer is specific to the Uno. A future Teensy port must own its authoritative configuration, retain version/CRC/bounds checking and power-loss recovery, and supply a storage implementation whose timing is measured on Teensy EEPROM emulation. The proposed 48-byte record footprint is well within the approximately 4,282-byte Teensy budget, but no Teensy EEPROM code is modified or timing validation claimed here.

## File organization

| Path | Purpose |
|---|---|
| `controller-firmware/platformio.ini` | ESP32 project root and LittleFS partition settings |
| `controller-firmware/data/` | Complete deployed HTML/CSS/JavaScript website |
| `controller-firmware/src/main.cpp` | Controller state machine, snapshots, direct Bluetooth receive and configuration dispatch |
| `controller-firmware/src/webserver.cpp` | Captive portal, static/HTTP routes and HTTP task |
| `controller-firmware/src/dashboard_state.h/.cpp` | Shared snapshot and one-command mailbox between controller/HTTP tasks |
| `controller-firmware/src/controller_webserver.h` | Web-server initialization interface |
| `controller-firmware/tools/` | Size report and optional manual preview/browser utilities |
| `controller-firmware/tests/browser_frame.html` | Development-only browser fixture, never deployed |
| `reciever-firmware/platformio.ini` | Standalone Uno test receiver project |
| `reciever-firmware/src/main.cpp` | Receiver-only drive/stop/watchdog, fake telemetry, configuration and EEPROM implementation |
| `reciever-firmware/tests/` | Receiver host tests with serial/EEPROM shims |
| `reciever-firmware/tools/run_host_tests.py` | Portable host-test compilation runner |
| `shared/include/telemetry_protocol.h` | Portable telemetry decoder, validity masks and CRC |
| `shared/include/config_protocol.h` | Single parameter registry and wire encoders |
| `shared/include/packet_reset.h` | Safe partial-frame discard helper used by both builds |
| `docs/PROTOCOL.md` | HTTP/Bluetooth contract and future Teensy requirements |
| `docs/VALIDATION.md` | Build, size and test results and remaining hardware checks |

Neither project's `src/` contains the other's firmware, docs, tools, tests or website. The controller website is in `controller-firmware/data/`, beside the ESP32's `platformio.ini`. The old root `data/` was emptied during the move; its temporary Windows directory lock came from an active process, not a PlatformIO requirement.

`leanFirmware.cpp` is unchanged; its SHA-256 is `0be5fd21208bc656a06cf00d9693d7aea3ac8c94500e25ed7bc40915ecd70da2`. No files outside this Controller project are modified.
