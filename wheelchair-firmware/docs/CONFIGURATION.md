# Wheelchair configuration

The Teensy owns the registry in `src/config`. `DriveSettings.cpp` declares the
initial settings. `Registry` supplies types and bounds, `Protocol` handles
requests, and `Persistence` owns the EEPROM banks. On the dashboard,
`bluetooth.js` separates CSV from
PacketSerial replies, `config-protocol.js` decodes the replies, and
`configuration.js` builds the forms. The HC-06 link works in either drive mode
and can connect to one Bluetooth client at a time.

## Declaring a field

1. Pick an unused nonzero field ID and place the declaration in the subsystem
   that owns the setting. Use `settings::registry()` to register it with the
   dashboard. Construct the group before its fields.
2. Supply the default, constraints, units, and help text. Replace the old
   constant at its point of use with `field.get()`. If the subsystem caches a
   derived value, recompute it in the settings-application path.
3. Rebuild the Teensy firmware and reconnect the dashboard. The field appears
   from the firmware schema. To check migration, load a saved configuration
   from the previous build and confirm the new field starts at its default.

```cpp
config::Group steering(settings::registry(), 20, "Steering");
config::ConfigField<float> gain(settings::registry(), 100,
    {steering, "Steering gain", "Scales the normalized steering input.", "×"},
    1.0f, 0.25f, 1.0f);
```

Group IDs and field IDs each have their own namespace. Use a parent group to
nest a group. The root group determines whether the dashboard shows it under
Configuration or Advanced Configuration. Set the last `FieldInfo` member to
`true` for a restart-only setting, then read that setting only when initializing
the affected subsystem. Keep descriptions short enough for the 392-byte
description payload, including the value and other metadata.

For a nested advanced group, a Boolean, and an enum:

```cpp
config::Group diagnostics(settings::registry(), 21, "Diagnostics", true);
config::Group logging(settings::registry(), 22, "Logging", true, &diagnostics);

config::ConfigField<bool> capture(settings::registry(), 101,
    {logging, "Capture readings", "Include sensor readings in diagnostic capture."},
    false);

enum class Detail : uint8_t { Summary = 0, Full = 1 };
constexpr config::Choice details[] = {{0, "Summary"}, {1, "Full"}};
config::EnumField<Detail, 2> detail(settings::registry(), 102,
    {logging, "Capture detail", "Select how much information is written to the capture."},
    Detail::Summary, details);
```

Keep enum byte values stable when changing their labels or display order.

Scalar fields support booleans, IEEE float and double, and signed and unsigned
integers up to 64 bits. Integer encoding follows the declared width on the
Teensy. Enum declarations supply stable byte values and display labels.
Firmware enforces type, width, range, enum membership, and profile constraints.

`ProfileField<N>` exposes a reusable graph type and the bounds for each
coordinate. `Editor::SignedPower` uses an input/output knee with
`0 < input < 1` and `0 < output < 1`. LEAN calculates the exponent as
`log(output) / log(input)` and uses the signed function across −1 to +1.
The dashboard plots its positive quadrant from 0 to 1 on each axis.
`Editor::PiecewiseLinear` describes up to four ordered input/output knots.
The browser supplies the graph and editor for either type. A future acceleration
controller can declare a piecewise-linear field and consume its points without
adding a dashboard component.

For example, a new subsystem could register this normalized three-knot profile:

```cpp
constexpr float initial[] = {0.0f, 0.0f, 0.5f, 0.3f, 1.0f, 1.0f};
constexpr config::Parameter knots[] = {
    {"Start input", 0, 0}, {"Start output", 0, 1},
    {"Midpoint input", 0.1f, 0.9f}, {"Midpoint output", 0, 1},
    {"End input", 1, 1}, {"End output", 0, 1},
};
config::ProfileField<6> acceleration(settings::registry(), 103,
    {steering, "Acceleration profile", "Maps normalized input to a fraction of the acceleration cap."},
    config::Editor::PiecewiseLinear, initial, knots);
```

The consuming subsystem would interpolate the points returned by
`acceleration.get()` and still enforce its hard acceleration limit. The example
only adds a setting when wired into a subsystem. If a new mathematical model
needs a different graph, add a reusable editor type and its firmware validator
once. Individual fields can then use that type from their declaration.

## Applying settings

Switch to DISARM mode before changing settings. Set, Save, Revert, and Restore
Defaults return a `NotDisarmed` error (status 5) in every other mode, even when
wheel commands are zero. The current mode is checked when the queued request
executes; wheel RPM is not used. Reads and discovery remain available in every
mode. Accepted commands execute as soon as they are serviced.
Runtime settings, including VESC IDs and wheel circumference, take effect immediately.
Save explicitly commits the current values to EEPROM.

The running PID controllers receive normal-profile updates through
`setTuning()`, which preserves RPM, acceleration, integral, and derivative
history. The transition profile is retained as a configurable profile for
future integration, matching the original firmware's unused transition
tuning declaration.

The default startup selection is REMOTE. A valid controller packet is required
for remote propulsion. LEAN remains calibration-gated. The default sensor
floor is the pre-calibration telemetry baseline and the seat-load reference on
both sensor layouts.

The sensor layout (ID 90) selects the lean math; see
`src/driveModes/lean_layout.h`. On the PLUS layout, after subtracting floors and
applying sensor gains (IDs 10-13), forward/back and right/left differences are
each divided by the calibrated user weight estimate and clamped to [-1, 1].
Individual sensor values are not saturation-capped. On the CORNER layout
(default since 2026-11-22), each channel's force is its count above the
empty-plate baseline (IDs 100-103) times its gain (IDs 104-107), the centre of
pressure is located in plate inches, and its offset from the neutral captured at
calibration is mapped to speed and turn with the deadzone and per-direction
full-lean reach (IDs 91-95). A second calibrate press within 10 s, made relaxed,
records the rider's relaxed posture: speed then stays zero from upright to
relaxed plus the relaxed-posture margin (ID 96), and forward reach is measured
from the relaxed point. The relaxed posture, a hold-calibrated reach and the
automatic empty-seat zero of the baselines live in RAM only. Sensor positions
are compile-time constants in `lean_layout.h`. A layout change takes effect at
the next calibration; until then LEAN stays stopped. The corner fields use 109
of each profile's 415 bytes, leaving about 7 bytes free on the host build (15 on
the Teensy), as the "every setting fits one saved profile" test checks. The
next batch of settings needs a storage-format change. The former directional saturation fields
(IDs 20-23) and their group (ID 4) are retired; those IDs must not be reused.
Older saved records for these fields are ignored when loading configuration.

Selecting a profile (the profile knob) first resets every field to its default,
then applies the profile's records, the same as loading the active bank. A
profile saved by older firmware therefore gets defaults for fields it lacks,
not whatever the previously active profile held.

The watchdog can only be shortened from its 100 ms default, the control
period stays within 4–10 ms, and configurable jerk cannot exceed the original
400 RPM/s² limit. The hard speed restriction remains 45% of the 11 mph design
speed, and the hard acceleration cap remains 2.85 m/s². RPM and RPM/s limits
use the shared `LINEAR_TO_RPM` value. Loading or applying configuration assigns
`LINEAR_TO_RPM = 60.0f / settings::wheelCircumference.get()` directly in
`applyRuntimeSettings()`. All motor and PID conversions read that same value.
The default circumference, propulsion limits, and configurable field ranges can
be changed in the firmware source before rebuilding.

## EEPROM and migration

EEPROM addresses 0–1023 hold the active configuration. Active format version 2
starts from firmware defaults when upgrading from the previous two-bank format.
Addresses 1024–4152 hold seven independent 447-byte profile slots. Each slot
supports a 24-byte UTF-8 name and 415 bytes of settings. Both record types use
CRC-32 and write their commit marker last. An interrupted active save can revert
to defaults, and an interrupted profile overwrite can leave that slot empty.

The payload stores little-endian `[field ID u16, type u8, length u8, value]`
records. Keep IDs stable when renaming or moving fields and never reuse retired
IDs. Unknown records are skipped, and incompatible type, width, or range
changes use the new field default. Saving a change stores values for the
current schema.
Group names, help, ranges, and editor metadata are compiled into the firmware.

The dashboard lists onboard profile names at Bluetooth connection. Loading a
profile stages its settings locally. Saving a named profile includes draft
values and leaves the active configuration untouched. All device writes
require Disarm.

## PacketSerial protocol v1

Requests are COBS-framed with the ordinary trailing zero. Responses add a
leading zero so the dashboard can demultiplex them from the existing newline
CSV telemetry. There is one outstanding request per connection.

| Packet | Bytes before COBS encoding |
| --- | --- |
| Request | `C1 version sequence operation body crc16le` |
| Response | `C2 version sequence operation status flags body crc16le` |

CRC-16/CCITT-FALSE covers every byte before the checksum. Flag bit 1 means
the running configuration differs from the saved settings. Other flag bits
are reserved and sent as zero.

| Operation | Request body | Successful response body |
| --- | --- | --- |
| 1 Hello | Empty | Group and field counts, two u16s |
| 2 Group | Group index u16 | ID u16, parent ID u16 or 0, advanced u8, name |
| 3 Describe | Field index u16 | ID u16, group ID u16, type u8, restart u8, name, description, units, type metadata, current value |
| 4 Read | Field ID u16 | Current value |
| 5 Set | Field ID u16 and typed value | Accepted value |
| 6 Save | Empty | Empty |
| 7 Revert | Empty | Empty |
| 8 Restore Defaults | Group ID u16, or 0 for all | Empty |
| 9 Preview | Group ID u16 | Preview type u8, field count u8, field IDs u16[], constant count u8, float32 constants[] |

| 10 Profile List | Empty | Slot count u8, then per slot a presence u8 and 24 name bytes when present |
| 11 Profile Read | Slot u8, offset u16 | Total size u16, up to 384 payload bytes |
| 12 Profile Begin | Slot u8, total size u16, 24 zero-padded name bytes | Empty |
| 13 Profile Chunk | Offset u16, up to 40 payload bytes | Empty |
| 14 Profile Commit | Empty | Empty |
| 15 Profile Move | Source slot u8, adjacent destination slot u8 | Empty |

Strings are UTF-8 with a one-byte length prefix. Scalar metadata is default,
minimum, maximum, in the same encoding as the value. Booleans supply only a
default. Enums supply a default, a byte choice count, and byte value/label
pairs. Profiles supply the editor ID, parameter count, and each parameter's
name, default, minimum, and maximum (three float32s). Profile values start with
their editor ID and carry float32 coordinates.

## PID step-response previews

PID groups attach a `GroupPreview` descriptor for the dashboard. Preview type 1
binds eight field IDs in order to low/high time constants, integral gain,
derivative gain, derivative filter, jerk, control period, and wheel circumference.
Its two float32 constants are maximum linear speed and maximum linear
acceleration. `PidProfile` constructs this descriptor from its fields so moving
or renaming a setting does not break the graph.

The browser simulates the firmware's command smoother from rest, stepping to
half the maximum wheel RPM at 250 ms. Each slider or numerical edit redraws the
five-second simulation using the local draft. The plotted output represents
command smoothing. Wheel mechanics and measured motor-speed feedback are
outside this model.

Preview metadata is requested separately from the existing group and field
schema. Firmware without this optional operation returns Unknown, and the
dashboard continues to render its ordinary controls.

## Host tests

```sh
g++ -std=c++17 -Wall -Wextra -Werror -I src tests/native/config_test.cpp \
  src/config/Registry.cpp src/config/Persistence.cpp src/config/Protocol.cpp -o /tmp/config_test
/tmp/config_test
node --test debugViewer/tests/*.test.cjs
```
