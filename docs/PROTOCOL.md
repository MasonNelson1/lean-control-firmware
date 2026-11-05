# Dashboard API and additive receiver protocol v1

The implementation is for an ESP32 and Uno test receiver. The same additive protocol is defined for a future Teensy port; Teensy source is untouched. Headers in `shared/include/` are the authoritative registry/codec definitions; both firmware projects include the same files.

## Existing drive protocol: unchanged

Transport remains PacketSerial default **COBS encoding with a zero delimiter**. All packet lengths below refer to decoded payloads, before COBS and delimiter. Multibyte integers are little-endian.

| Type | Payload | Length |
|---|---|---|
| `0x01` DRIVE | type, left signed int16, right signed int16 | 5 |
| `0x02` SOFT_STOP | type only | 1 |
| `0x03` SOFT_STOP_CLEAR | type only | 1 |

Drive values map `[-32767,32767]` to `[-1,1]`; `-32768` is invalid. No version or CRC is added to these legacy packets. Receivers without new protocol support ignore new types and continue receiving the same drive format. The new Uno starts/latches stopped on boot, watchdog expiry, and link loss. Only the physical controller's legacy clear packet releases its latch. No telemetry/config message refreshes the drive watchdog.

## Telemetry `0x80`: 132 bytes

All numeric fields use explicit encoding, never a native padded struct. Floats are IEEE-754 binary32, little-endian.

| Offset | Size | Meaning |
|---:|---:|---|
| 0 | 1 | Type `0x80` |
| 1 | 1 | Version `1` |
| 2 | 2 | Group validity mask, below |
| 4 | 4 | Unsigned sample sequence, wraps modulo 2^32 |
| 8 | 1 | State: 0 unknown, 1 stopped, 2 ready, 3 moving, 4 fault |
| 9 | 1 | Receiver soft-stop latch: 0 unknown, 1 latched, 2 clear |
| 10 | 2 | Reserved, must be zero |
| 12 | 116 | 29 binary32 values, ordered below |
| 128 | 4 | CRC-32 of bytes 0–127 |

CRC is reflected CRC-32/ISO-HDLC: polynomial `0xEDB88320`, initial `0xFFFFFFFF`, final XOR `0xFFFFFFFF`. Check vector ASCII `123456789` => `0xCBF43926`. CRC is integrity checking, not authentication.

| Value indices | Mask bit | Meaning / units |
|---|---:|---|
| 0–3 | 0 | Raw sensors front/back/left/right, ADC counts |
| 4–7 | 1 | Calibrated front/back/left/right, counts (may be signed) |
| 8–11 | 2 | Baselines front/back/left/right, ADC counts |
| 12 | 3 | Estimated mass, kg; do not use an uncalibrated ADC estimate as kg |
| 13–14 | 4 | Speed and turning commands, normalized -1 to +1 |
| 15–16 | 5 | Left/right motor duty, -1 to +1 |
| 17–21 | 6 | Left VESC: electrical RPM, signed motor current A, input voltage V, MOSFET temperature °C, fault code (integer 0–255) |
| 22–26 | 7 | Right VESC, same ordering/units |
| 27–28 | 8 | Battery voltage V and estimated charge 0–100% |
| — | 15 | SIMULATED: sensor, weight, motor and battery measurements are synthetic |

Mask bits 9–14 must be zero. State and soft-stop fields are always present, using unknown values where needed. Validity applies to complete groups; omit a group if any required measurement is unsupported, not fresh, or not calibrated. A future version may split groups further. The decoder discards NaN/infinite/out-of-range valid values and requires the exact packet length, known version, zero reserved bytes, supported bits and matching CRC. Ignored invalid-group bytes cannot reach JSON as NaN.

The Uno sets `0x81FF`: all groups available, synthetic measurement marker on. It sends at 10 Hz by default, configurable 5–20 Hz. Commands and latch/state in these messages are real receiver state; simulated motor behavior follows the effective commands. Nonzero fault/state fixtures are not currently generated.

ESP32 considers receiver feedback stale after 500 ms without a valid telemetry packet and invalidates it across Bluetooth state transitions. JSON stale values are `null`. Age measures local reception time, not sensor measurement age. Sequence is a diagnostic counter, not a sender timestamp or guaranteed loss detector. After receiver integration, add sender sample/boot timestamps in a versioned extension if required.

One decoded telemetry frame is 132 bytes; COBS + delimiter makes 134 bytes here. At 10 Hz this is approximately 1,340 bytes/s before Bluetooth overhead. Configuration responses are prioritized over the next telemetry frame; a partially transmitted frame is never interleaved with another frame. The receiver sends in four-byte chunks and checks RX between chunks. SoftwareSerial still has physical half-duplex/interrupt limitations.

## Configuration requests `0x20`: 16 bytes

| Offset | Size | Meaning |
|---:|---:|---|
| 0 | 1 | `0x20` |
| 1 | 1 | Version `1` |
| 2 | 1 | Operation |
| 3 | 1 | Parameter ID (zero for nonparameter operations) |
| 4 | 4 | Request ID |
| 8 | 4 | Binary32 value, zero unless APPLY |
| 12 | 4 | CRC-32 of bytes 0–11 |

Operations: 0 DISCOVER, 1 READ, 2 APPLY, 3 SAVE, 4 DEFAULTS, 5 CALIBRATE. The test receiver responds UNSUPPORTED for CALIBRATE and never calibrates fake sensors. DISCOVER OK returns `1.0`, identifying this exact receiver-test registry version. It is not a dynamic Teensy tuning catalog. An expanded future registry must negotiate a new registry/protocol version before exposing parameters.

Parameter IDs: 0 `telemetry_hz` integer 5–20, default 10; 1 `watchdog_ms` integer 100–250, default 250. Every APPLY is validated again by the receiver. READ and DISCOVER do not require a stop. APPLY/SAVE/DEFAULTS require a latched receiver stop, zero effective outputs, and connected STATE. Save work is bounded to one ready EEPROM byte per iteration; successful acknowledgment follows readback validation.

## Configuration responses `0x81`: 20 bytes

| Offset | Size | Meaning |
|---:|---:|---|
| 0 | 1 | `0x81` |
| 1 | 1 | Version `1` |
| 2 | 1 | Echoed operation |
| 3 | 1 | Result code |
| 4 | 4 | Echoed request ID |
| 8 | 1 | Echoed parameter ID |
| 9 | 1 | Flags: bit 0 unsaved settings, other bits zero |
| 10 | 2 | Reserved, zero |
| 12 | 4 | Binary32 value: read/applied value, or registry version for DISCOVER; zero otherwise |
| 16 | 4 | CRC-32 of bytes 0–15 |

Results: 0 OK, 1 UNSUPPORTED, 2 INVALID, 3 UNSAFE, 4 BUSY, 5 STORAGE_ERROR. ESP32 additionally reports local outcomes 6 TIMEOUT and 7 DISCONNECTED through HTTP; these are never receiver response codes. Frames with bad CRC/length/version are dropped without a response. Malformed well-framed requests return INVALID when possible. Response buffers are bounded; an overloaded request stream can time out instead of creating an unbounded queue.

ESP32 matches request ID, operation and parameter before accepting a response. Only one command is in flight. DISCOVER retries at most every two seconds; supported parameter READs populate the cache. Browser writes use IDs with bit 31 set; automatic discovery/reads do not overwrite the last browser result. A missing acknowledgment times out after 1.5 s. Mutations are never automatically retried, and an uncertain write invalidates the configuration cache so READs fetch current values. Apply/default/save are idempotent at the state level; unchanged saves do not wear EEPROM.

## HTTP interface

Same origin, no CORS, no external resources. JSON responses and static files have `nosniff`, no-store and restrictive Content Security Policy headers. Status polling reads snapshots only and does not itself transmit Bluetooth requests.

| Method/path | Behavior |
|---|---|
| GET `/api/status` | Combined controller, telemetry, browser-request status and diagnostic snapshot |
| GET `/api/config` | Cached receiver capabilities/settings, allowed bounds, current values, dirty flag and per-boot mutation token |
| POST `/api/config/apply` | `application/x-www-form-urlencoded`, exactly `id` and `value` |
| POST `/api/config/save` | No arguments; persist applied settings |
| POST `/api/config/defaults` | No arguments; restore runtime defaults |
| POST `/api/calibrate` | 501, no fake calibration or Bluetooth calibration sent |
| GET `/`, `/index.html`, `/dashboard.css`, `/dashboard.js` | Whitelisted LittleFS website files |

POSTs require `X-LEAN-Token` from GET `/api/config`. No drive or stop-clear HTTP route exists. Other `/api/` paths return JSON 404 rather than a captive redirect. Wrong methods return 405. Non-API captive probes redirect to the dashboard. Missing static files return 503 without formatting storage.

Apply accepts a single-digit known ID and an integer digit-only value of at most eight characters; duplicate/missing/unknown fields, signs, decimals, exponent notation, NaN and out-of-range values return 400. Missing/invalid token returns 403. Unavailable/incompatible receiver returns 503. Missing fresh stop confirmation or a busy command returns 409. Accepted requests return **202, not success**:

```json
{"requestId":2147483648,"status":"queued"}
```

Poll `/api/status` until `request.id` matches and `request.pending` is false. Result 0 confirms the receiver action; it does not imply a SAVE unless the operation was SAVE. A timeout/disconnect means the result is uncertain, so read current settings before retrying. `request.pending` also covers internal discovery/reads, so a completed browser result may be visible after those finish. The last browser result is shared by connected clients, not a per-client event history.

Status JSON fields:

- `apiVersion`, `uptimeMs`, `snapshotAgeMs`, `bluetooth` (`disconnected`, `connecting`, `connected`, `disconnecting`).
- `controller`: `softStopRequested`, raw `[joystickX, joystickY, averagedPot]`, `joystick` normalized raw `[x,y]`, `sensitivity`, `lastDrive` `[left,right]`, `driveAgeMs` (null before first drive in the session).
- `wheelchair`: `fresh`, `ageMs` (null before any packet in the session), `sequence`, `state`, `stop`, `validMask`, `values` (29 entries in wire order). Stale state/stop/values are null and stale validity is zero.
- `request`: last browser `id`, transport `pending`, last browser `result`.
- `diagnostics`: `drivePackets`, `maxDriveGapMs` (between drive sends within one Bluetooth session, including soft-stop pauses), `maxLoopUs` (includes discovery), `received` (valid telemetry only), `rejected` (invalid/unmatched diagnostic frames), `freeHeap`, `filesystemReady`, `filesystemTotal`, `filesystemUsed`.

Counters are cumulative unsigned 32-bit values and may wrap. No battery state-of-charge chemistry, travel speed, physical E-stop circuit or VESC fault-name mapping is inferred.

## Future Teensy integration requirements

1. Keep existing drive/soft-stop framing and watchdog semantics. Receive diagnostics without extending the drive watchdog or bypassing local safety interlocks.
2. Serialize telemetry with the shared wire layout, using actual fresh data and appropriate validity masks. Clear SIMULATED only when measurements are real. The current reference `userWeightEstimate` is ADC-derived, not calibrated kg, so omit the kg group until calibrated.
3. Report local operational state, confirmed stop latch and real commanded/measured duty separately. Map real VESC electrical RPM/current/input voltage/MOSFET temperature/fault fields; omit unsupported groups.
4. Implement bounded receive/transmit service on a hardware UART to the Bluetooth module. Prioritize drive/watchdog/safety over outgoing telemetry and storage. Start at 10 Hz and measure timing under concurrent Wi-Fi traffic.
5. Own configuration in Teensy RAM and EEPROM. Add only parameters that are actually used by its control logic, with a negotiated registry version and independently justified safe ranges/cross-field constraints. Pin mappings/direction/IDs should not become freely editable web constants.
6. Use versioned CRC-checked records, factory defaults, power-loss recovery, explicit save, write minimization and a Teensy-appropriate nonblocking storage worker. Measure EEPROM-emulation latency rather than copying the AVR writer.
7. Require authoritative standstill/stop/interlocks on Teensy before any apply/default/save/calibration. Browser/ESP32 checks are supplemental. Calibration needs a real, nonblocking sensor workflow and explicit results; keep it unavailable until implemented.

No Teensy firmware modifications or hardware validation are included in this controller/Uno testing implementation.
