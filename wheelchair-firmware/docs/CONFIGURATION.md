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

Stop the chair before editing settings. Set, Save, Revert, and Restore Defaults
return a `Moving` error when either smoothed wheel command has a magnitude above
1 RPM. Commands at exactly ±1 RPM are accepted. The check uses the last
PID-smoothed command sent toward each VESC. Reads and discovery remain
available at any speed. Accepted commands execute as soon as they are serviced.
Runtime settings, including VESC IDs and wheel circumference, take effect immediately.
Save explicitly commits the current values to EEPROM.

The running PID controllers receive normal-profile updates through
`setTuning()`, which preserves RPM, acceleration, integral, and derivative
history. The transition profile is retained as a configurable profile for
future integration, matching the original firmware's unused transition
tuning declaration.

The default startup selection is REMOTE. A valid controller packet is required
for remote propulsion. LEAN remains calibration-gated. The default sensor
floor is the pre-calibration telemetry baseline. Saturation factors scale the
calibrated weight reference independently for the four sensors. Calibration
calculates and caches all four saturation values. Recalibrate after editing a
saturation factor to use it in the drive calculations.

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

EEPROM addresses 0–2047 hold two 1024-byte banks. Each bank contains a commit
marker, magic, storage version, generation, payload length, and CRC-32 over the
metadata and payload. A save invalidates the destination, writes the payload and
header, then commits the marker last. The firmware loads the newest valid bank
and falls back to the previous bank if a save was interrupted.

The payload stores little-endian `[field ID u16, type u8, length u8, value]`
records. Keep IDs stable when renaming or moving fields and never reuse retired
IDs. Unknown records are skipped, and incompatible type, width, or range
changes use the new field default. Saving a change stores values for the
current schema.
Group names, help, ranges, and editor metadata are compiled into the firmware.

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
