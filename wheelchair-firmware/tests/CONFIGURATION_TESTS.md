# Configuration and debug viewer test report

Run on 2026-11-11. The initial work added tests and test support only. A follow-up
fixed the two dashboard validation issues described below. These tests do not
exercise the motor control implementation.

## Results

564 new, individually named test cases were added. Including the existing debug
viewer tests, the commands below run 581 cases: **581 passed, 0 failed** after
the two dashboard fixes. The initial run had 579 passes and 2 real failures,
without skips, expected-failure annotations, or weakened assertions. Both original
regression tests now pass unchanged.
The sanitizer run repeats the firmware configuration cases and is not counted
again in these totals.

| Suite | Cases | Passed | Failed |
| --- | ---: | ---: | ---: |
| Native registry, persistence, and configuration protocol (new) | 278 | 278 | 0 |
| Actual firmware configuration declarations (new) | 81 | 81 | 0 |
| Debug viewer JavaScript (165 new, 15 existing) | 180 | 180 | 0 |
| Debug viewer Python (40 new, 2 existing) | 42 | 42 | 0 |
| Total | 581 | 581 | 0 |

## Original failures, fixes, and their limits

### COBS decoder accepts an embedded zero

`debugViewer/tests/config-wire-exhaustive.test.cjs`:
`COBS rejects a zero embedded inside an encoded data block`.

Before the fix, `ConfigWire.decodeFrame(Uint8Array.of(2, 0))` returned a decoded zero byte instead
of rejecting the frame with `null`. An encoded COBS block cannot contain zero
bytes. The test supplies invalid external data and expects the same rejection
behavior already used for other malformed COBS inputs.

The decoder now rejects zero bytes while copying encoded block data. Zeros
reconstructed between blocks remain valid.

This establishes a decoder validation gap. The normal Bluetooth receiver splits
frames on zero bytes, so this particular array would not normally reach the
decoder intact. CRC and packet validation still apply. This test does not show
that an invalid configuration can be applied through the normal connection.

### Numerical validation accepts whitespace as zero

`debugViewer/tests/configuration-controls.test.cjs`:
`numeric editor validation / whitespace`.

Before the fix, the numerical validation helper accepted three spaces as the number zero when
zero is in range. An input containing no numerical value should fail validation,
like an empty string; JavaScript's `Number()` conversion instead supplies zero.

The helper now trims surrounding whitespace before checking for an empty value
and parsing it, including the 64-bit integer path.

This tests the helper directly. Real HTML number inputs normally sanitize this
value to an empty string, which the existing helper already rejects. The test
therefore establishes a helper-level gap, not a demonstrated interaction bug in
a standard browser number input. The DOM adapter deliberately does not implement
browser input sanitization, so that the helper's own contract is tested.

## Coverage

- Numeric field types, Boolean wire codes, enum membership, exact 64-bit browser
  values, metadata, bounds, truncated values, and bounded writer behavior.
- Signed power and piecewise profiles, amplified knee points, endpoints,
  nonfinite values, editor identifiers, and point ordering.
- Stable identifiers across renamed, reordered, added, and removed fields;
  incompatible types and narrowed ranges; unknown and truncated TLV records.
- EEPROM CRC corruption, header corruption, fallback to a valid bank, and an
  interrupted save at each changed-byte write through the commit boundary.
- All configuration operations, malformed requests, response CRCs, invalid IDs,
  cross-field rollback, persistence actions, and the existing 1 RPM command gate.
- Actual settings declarations, discovery packet size, group hierarchy, PID
  preview references, CAN ID conflicts, and EEPROM reload behavior.
- COBS boundaries, response parsing, serial timeouts, connection changes,
  concurrent requests, and failed writes.
- Dynamic controls, units and help, integer/float validation, precision retention,
  sliders, per-value reset, confirmation before defaults, and profile validation.
- Viewer CSV parsing, legacy telemetry, nonfinite data rejection, history bounds,
  clock resets, console bounds, disconnected clients, and configuration assets.

The native EEPROM adapters simulate byte-addressable storage and interrupted
updates. They do not emulate Teensy flash timing or electrical power failures.
The native tests compile only sources from `src/config`; the declaration suite
provides Arduino/EEPROM stubs under `tests/native/config_stubs` and does not link
drive modes or motor control.

The viewer tests execute the real JavaScript and Python modules. The controls
suite injects a test-only export into an in-memory copy of the UI source to reach
private helpers. No production source file is rewritten. The DOM adapter covers
control behavior, not layout, pointer dragging, or canvas rendering. Existing
PID preview tests exercise the dashboard simulator, not the firmware PID.
No wheelchair or Bluetooth hardware was available for this run.

## Reproduce on Windows / WSL

Run from `wheelchair-firmware` in PowerShell. Requirements: C++17 `g++`, Node.js
with `node:test`, and Python with the viewer requirements installed. The WSL
Python command below uses this workspace's existing viewer environment.

```powershell
New-Item -ItemType Directory -Force .pio | Out-Null

g++ -std=c++17 -Wall -Wextra -Werror -I src tests/native/config_exhaustive_test.cpp src/config/Registry.cpp src/config/Persistence.cpp src/config/Protocol.cpp -o .pio/config_exhaustive_test.exe
if ($LASTEXITCODE -ne 0) { throw 'Configuration test compilation failed' }
& ./.pio/config_exhaustive_test.exe
# Expected: TOTAL 278 / PASS 278 / FAIL 0

g++ -std=c++17 -Wall -Wextra -Werror -I tests/native/config_stubs -I src tests/native/drive_settings_test.cpp src/config/DriveSettings.cpp src/config/Registry.cpp src/config/Persistence.cpp src/config/Protocol.cpp -o .pio/drive_settings_test.exe
if ($LASTEXITCODE -ne 0) { throw 'Settings test compilation failed' }
& ./.pio/drive_settings_test.exe
# Expected: TOTAL 81 / PASS 81 / FAIL 0

node --test --test-reporter=tap debugViewer/tests/*.test.cjs
# Expected: 180 passed, 0 failed.

wsl -d Ubuntu --exec env PYTHONPATH=debugViewer/.venv/lib/python3.12/site-packages /usr/bin/python3 -B -m unittest discover -s debugViewer/tests -v
# Expected: 42 tests, OK.
```

The full run's output files are local, ignored build artifacts:
`.pio/config_exhaustive_results.txt`, `.pio/drive_settings_results.txt`,
`.pio/debug_viewer_tests.tap`, and `.pio/viewer_python_results.txt`.

## Sanitizers

The 278 generic native configuration cases also passed with GCC 13 AddressSanitizer
and UndefinedBehaviorSanitizer in WSL, with no diagnostics:

```powershell
wsl -d Ubuntu --exec g++ -std=c++17 -g '-fsanitize=address,undefined' -fno-omit-frame-pointer -I src tests/native/config_exhaustive_test.cpp src/config/Registry.cpp src/config/Persistence.cpp src/config/Protocol.cpp -o /tmp/wheelchair-config-sanitized
if ($LASTEXITCODE -ne 0) { throw 'Sanitizer test compilation failed' }
wsl -d Ubuntu --exec /tmp/wheelchair-config-sanitized
```

The Windows declaration run uses 32-bit `unsigned long`, matching Teensy. The
generic suite is also run on 64-bit Linux and derives native scalar widths rather
than assuming `unsigned long` is always four bytes. This is host testing, not a
substitute for a Teensy build or device testing.
