# Dashboard diagnostic build

The controller now uses direct `Serial.println` statements in its existing source files for function calls, branches, HTTP responses, and Bluetooth operations. The Bluetooth stream wrapper and `trace_controller.h` have been removed. PacketSerial attaches directly to `SerialBT`, with no per-loop receive-byte budget. The verbose prints may block on USB Serial and change timing; they are for tracing this diagnostic run. Receiver code is unchanged by this update. Only the controller firmware needs uploading for these new printouts; the website has not changed.

Targeted heap/file diagnostics are also in the existing files. `[HEAP]` and `[HEAP INTERNAL]` report free/minimum/largest allocations once per second from the controller task, including while HTTP is blocked. `[FILE BEFORE]`/`[FILE AFTER]` report heap integrity (1 means intact), client connection, expected/sent bytes, and elapsed streaming time. Bluetooth discovery/connect calls print their returned result and heap before/after. `[ALLOC FAIL]` reports failed request size, capability mask, allocator function and core immediately, using allocation-free ROM printing because the allocator hook must not use String, heap queries or HardwareSerial locks. Hook registration result 0 means success. The internal `btc_transfer_context` status is not exposed by Arduino; distinguishing its exact non-success result still requires SDK-level instrumentation, and no SDK files were changed. Absence of an allocation-failure line alone does not prove queue exhaustion.

This pass adds instrumentation, not proposed fixes. No new libraries, timeout changes, polling-rate changes, packet-format changes or motor-output changes. No headless verification. Earlier unverified HTTP startup experiments (explicit AP bind, listener retries, changed initialization order and minimum-tick delay) were removed to retain the original dashboard startup behavior. The small health route and filesystem/startup logs are diagnostics only.

## Possible issues found

| Hypothesis | Evidence | Useful observations |
|---|---|---|
| JavaScript errors masquerade as connection failures | `poll()` catches fetch, JSON parsing and `render(latest)` together and sets `online=false`. If the fallback `render(null)` also throws, the next polling timer is never scheduled. | Browser error phase `render` or rejection phase `fallback-render`; firmware remains alive while HTTP status requests stop. |
| An idle HTTP connection delays new requests | Installed Arduino WebServer services one current client, allowing 5,000 ms waiting for request data. Browser fetches abort after 1,800 ms. | HTTP `state=1`, rising `age`, status counter stalls. A blocked parser/send instead produces large `handle` time. The mismatch is real; its role in this failure is unconfirmed. |
| Browser rendering work | Every poll updates all tabs, rebuilds diagnostic lists and draws the graph, even while hidden. Entering telemetry draws the graph again. | High browser `renderMaxMs` or a canvas exception. No specific canvas bug or leak is confirmed. |
| Tab switch is local | Switching tabs changes visibility and draws the graph; it does not increase polling rate or send Bluetooth commands. | Compare request rate before/after `[BROWSER VIEW] telemetry`. Selected `refreshMs` is in browser logs. |
| SoftwareSerial RX/TX interference | The Uno receives drive packets while transmitting framed telemetry. Four-byte chunks do not guarantee reliable full duplex. | Receiver `ov`, `bad`, `wd` or drive gaps increase; controller rejects packets or telemetry/config responses stop arriving. No overflow flag does not exclude timing-induced corruption. |
| Blocking controller work | BluetoothSerial can wait for TX queue space; existing display flush runs every loop; discovery/connect calls can block. | Rising controller `write`/`loop` times and receiver watchdog events. |
| Heap, stack or shared-radio pressure | Temporary HTTP Strings, Bluetooth allocations and concurrent Wi-Fi/BT exist. No exhaustion/leak has been measured. | Falling HTTP `heap`, `block` or `stack`, or failures correlated with BT connection. Flash usage is not runtime free RAM. |
| Old receiver has no telemetry | Original reference receiver accepts drive/stop but provides no new telemetry/config replies. | `rx=0`, age `-1` and config discovery timeouts are expected. This explains missing measurements, not a browser crash by itself. |
| RTOS tick rounding | Original HTTP task uses `pdMS_TO_TICKS(2)`. | Startup `tick_ms` greater than 2 means that delay may round to zero. This is conditional, not a confirmed scheduling fault. |

The installed WebServer already sends `Connection: close`; do not assume keep-alive responses are the cause. An idle/speculative/incomplete incoming request can still occupy the one active client. No ESP32 reboot is assumed.

## Upload and reproduce

From the top-level Controller folder in PowerShell, close each monitor before uploading that board:

```powershell
$pio = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
& $pio run -d controller-firmware -e esp32dev -t upload --upload-port COM4
& $pio run -d controller-firmware -e esp32dev -t uploadfs --upload-port COM4
& $pio run -d reciever-firmware -e uno_receiver -t upload --upload-port YOUR_UNO_PORT
```

Both firmware uploads are required for both sets of logs; uploadfs supplies browser logging and the view marker. Open both USB serial monitors at 115200 before reproducing. Opening the Uno monitor may auto-reset the Uno normally, so allow startup to settle. Boot banners identify the diagnostic builds.

Record ten seconds on overview while disconnected, connect with the physical button, record ten seconds on overview, then tap telemetry without changing refresh rate. Record another ten seconds and note whether the UI freezes, displays Reconnecting, or the entire browser tab closes/reloads. Save both logs from the same run; uptime clocks are not synchronized.

If accessible, preserve the browser developer console through its normal remote inspector or a desktop browser reproduction. No automated browser tools are required or run.

## ESP32 log fields

Periodic diagnostics alternate line types at one-second intervals. Config events are infrequent; individual drive/telemetry payloads are not printed.

- `[CTRL]`: `t` uptime ms; `bt` 0 disconnected/1 connecting/2 connected/3 disconnecting; controller stop request; cumulative drive sends, accepted telemetry and rejected diagnostics; reception age ms (`-1` before a sample); sequence.
- `[CTRL IO]`: window maximum controller-work `loop` duration in microseconds; maximum RX queue `rxq` bytes; last bad telemetry `type/decodedSize`; cumulative config timeouts. Wrapper-dependent byte and write-duration counters were removed with the wrapper. Rejected config responses affect the total rejection count, but not last bad telemetry type/size.
- `[CFG SEND]`, `[CFG ACK]`, `[CFG TIMEOUT]`: IDs, operations and result/timeout, useful for distinguishing an old receiver from functioning bidirectional communication.
- `[HTTP]`: client `state` 0 none/1 waiting for request data or processing/2 waiting for close; `age` ms since that client's state transition (meaningful only for nonzero state); cumulative requests and status/config/static-file counts.
- `[HTTP IO]`: maximum handler-call duration in microseconds; maximum HTTP task loop `gap` in ms; last generated JSON bytes; free heap and largest free allocation `block` in bytes; task stack watermark in the installed FreeRTOS API's units; Wi-Fi station count. Idle waits can span many short handle calls, so examine state/age too.
- `[BROWSER VIEW]`: view from the normal status request's `?view=` argument. No extra request is added. A crash before another poll may leave no view marker.
- `[HTTP ERROR]`: HTTP errors >=400. Existing startup/filesystem logs remain.

If controller `drive` and receiver `drv` advance while HTTP `status` stops, control traffic is still active and the failure is more likely HTTP/browser-side. If HTTP status requests continue while drawing stops, browser console output becomes particularly useful. A request count alone does not prove full delivery to the phone.

## Uno log fields

One periodic line is attempted each second, cycling through three types. Existing four-per-second LPower/RPower prints remain unchanged.

- `[RX]`: uptime, real STATE pin, stop latch, valid drive count, malformed/unknown packets, watchdog expiries while previously unlatched.
- `[RX IO]`: SoftwareSerial overflow flags; completed outbound frames (telemetry plus config ACK); current TX position/length; pending frame age ms; dropped ACKs from a full response queue.
- `[RX TIME]`: maximum interval between loop entries in microseconds; maximum interval between valid drives in ms; age since watchdog refresh in ms. Intentional soft-stop pauses can enlarge drive gaps. The legacy clear packet also refreshes the watchdog timestamp.
- `[RX LINK]`: STATE pin changes.
- `[RX ACK]`: generated configuration response ID/operation/result.

## Browser console

- `[BROWSER POLL ERROR]` includes view, phase, exception name/message/stack.
- `[BROWSER CONFIG ERROR]` distinguishes config fetch failure from status fetch failure.
- `[BROWSER ERROR]` and `[BROWSER REJECTION]` catch uncaught event errors and rejected promises for logging; they do not suppress or repair them.
- `[BROWSER]` gives request/failure totals, round-trip time, maximum render duration, refresh interval and telemetry sequence approximately every two seconds while polling runs.

The broad catch and fallback-render behavior remain unchanged. The logs intentionally expose failures rather than implement recovery.

## Instrumentation limits

`shared/include/debug_serial.h` uses fixed stack buffers and AVR flash-resident format strings. It checks USB serial output capacity and skips lines that will not fit or exceed its buffer; no flush or deliberate UART wait. Serial driver contention between ESP32 tasks is still possible, and instrumentation has some timing cost. Window maxima reset after attempted reports even when a line is skipped. Logs are observations, not real-time guarantees. Bluetooth framing/data and pre-existing serial output remain unchanged.

Define `LEAN_DEBUG_ENABLED=0` in build flags to disable the new helper's output; measurements and existing non-helper startup prints remain. No new libraries or automatic EEPROM writes. The Teensy reference is unchanged.
