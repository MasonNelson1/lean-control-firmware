# RPM setpoint PID controller

This document describes the C++ controller in [`src/rpm_setpoint_pid.h`](../src/rpm_setpoint_pid.h), its settings in [`src/drive_constants.h`](../src/drive_constants.h), and its current use in [`src/simple_drive.cpp`](../src/simple_drive.cpp). The source code is the authority if its settings change after this document is updated.

## Purpose and scope

`RPMSetpointPID` converts a desired **wheel RPM** into the next smoothed wheel RPM command. It limits how quickly the command changes and how quickly its acceleration changes, while allowing different tracking behavior at low and high commanded speeds.

It does **not** read actual motor RPM. The feedback value is the controller's previous output command. Consequently, this is a PID-based **setpoint shaper**, not a closed-loop motor-speed controller. It cannot determine actual vehicle speed, wheel slip, motor response, physical acceleration, or whether the chair followed the requested direction. VESC status messages are printed separately and are not fed into this controller.

Each wheel has its own instance and independent state. Changes to the two commands may temporarily alter the ratio between translation and turning. The helper does not coordinate the wheels to preserve a particular curvature.

## Files and responsibilities

| File | Responsibility |
| --- | --- |
| `src/drive_constants.h` | Shared geometry, hard design limits, PID tuning, response-curve settings and update period, in namespace `DriveConstants`. |
| `src/rpm_setpoint_pid.h` | Controller, immutable per-instance tuning, state, validation, PID calculation and limits. |
| `src/simple_drive.cpp` | Sensor processing, deadband and response curve, wheel target mixing, scheduling, stop handling, controller calls and VESC communication. |
| `src/vesc_can.h` | Converts wheel RPM commands to VESC electrical RPM and sends CAN frames. |

The controller is currently called from `loop()`. The old source comment saying “review only; not used by loop yet” is stale. Likewise, the comment showing a “future integration” call does not describe the active calls below it.

## Public interface

### Construction

```cpp
RPMSetpointPID::Tuning tuning = {
    lowSpeedTauS,
    highSpeedTauS,
    ki,
    kd,
    derivativeFilterS,
    lowSpeedAccelMS2,
    highSpeedAccelMS2,
    maxJerkRPMPerS2
};
RPMSetpointPID controller(tuning);
```

The constructor copies the tuning and validates it. The stored tuning is `const`: changing the source tuning object later does not change an existing controller. A new instance is needed to replace its tuning. Speed and acceleration selections are different: they are arguments to every update and may vary while the controller runs.

All state starts at zero. Construction does not read a clock or communicate with the motors.

### `smoothRPM`

```cpp
float smoothRPM(float requestedRPM,
                float dt,
                float maxSpeedMS,
                float maxAccelMS2);
```

**Call exactly once per update to that wheel's VESC motor setpoint.** Do not call it on every unrestricted pass through `loop()`. Every valid call advances the state by `dt`; the helper does not schedule calls, measure elapsed time, or detect skipped or repeated calls.

| Argument | Units and meaning |
| --- | --- |
| `requestedRPM` | Desired signed wheel RPM. Used directly in the PID error, even if outside the output speed limit. It is not normalized power, duty, motor current, or electrical RPM. |
| `dt` | Seconds represented by this update. Use `0.005f` for a 5 ms interval, not `5`. Must be finite and strictly positive. |
| `maxSpeedMS` | Positive user-selected limit on the magnitude of wheel linear speed, in m/s. Capped internally at the design maximum. |
| `maxAccelMS2` | Positive user-selected limit on the magnitude of wheel command acceleration, in m/s². Capped internally at the design maximum. Applies in either direction and during ordinary acceleration/deceleration. |
| Return value | Next signed, smoothed wheel RPM command. The first valid call advances from zero immediately. |

The method has no upper bound on an otherwise valid `dt`. A caller supplying an erroneously large positive value causes a large integration step; this is not rejected as a timing error.

### `reset`

```cpp
void reset();
```

Sets commanded RPM, acceleration, integral, filtered derivative, previous RPM and previous interval to zero; clears the flag that indicates derivative history is available. It preserves the immutable tuning and its validity result.

`reset()` does not send any CAN command. The caller must independently send zero for an immediate stop. Passing `requestedRPM = 0` is an ordinary smoothed request and is **not** equivalent to `reset()` plus sending zero.

### `isValid`

```cpp
bool isValid() const;
```

Returns whether the constructor's tuning passed validation. It does not report the validity of the most recent update arguments. An invalid controller continues returning zero on updates; `reset()` cannot make its tuning valid.

## Current settings and units

These are the shared constants at the time of writing:

| Setting | Current value | Meaning |
| --- | ---: | --- |
| `WHEEL_CIRCUMFERENCE_M` | 1.89865 m | 74.75-inch wheel circumference. |
| `LINEAR_TO_RPM` | approximately 31.6014 | `60 / circumference`. Multiplies m/s into RPM, or m/s² into RPM/s. |
| `MAX_SPEED` | 4.91744 m/s | Hard design speed, 11 mph; approximately 155.398 wheel RPM. |
| `MAX_ACCEL` | 2.85 m/s² | Hard design wheel command-acceleration cap; approximately 90.064 RPM/s. |
| `RPM_PID_LOW_SPEED_TAU_S` | 0.035 s | Low-speed proportional tracking parameter. |
| `RPM_PID_HIGH_SPEED_TAU_S` | 0.1 s | High-speed proportional tracking parameter. |
| `RPM_PID_KI` | 0 | Integral gain, units 1/s²; zero disables integral accumulation. |
| `RPM_PID_KD` | 2 | Derivative gain; dimensionless in this acceleration-output formulation. |
| `RPM_PID_DERIVATIVE_FILTER_S` | 0.02 s | Time constant of the derivative low-pass filter. |
| `RPM_PID_LOW_SPEED_ACCEL_MS2` | 2.85 m/s² | Low-speed requested-acceleration bound before runtime capping. |
| `RPM_PID_HIGH_SPEED_ACCEL_MS2` | 2.85 m/s² | High-speed requested-acceleration bound before runtime capping. |
| `RPM_PID_MAX_JERK_RPM_PER_S2` | 400 RPM/s² | Maximum ordinary rate of change of command acceleration. Approximately 12.658 m/s³ at the wheel. |
| `LOOP_RATE` | 5 ms | Minimum scheduled interval in `simple_drive.cpp`. |
| `dt` | 0.005 s | Fixed interval currently passed by `simple_drive.cpp`. |

The two acceleration tuning bounds currently match. Therefore their speed-dependent interpolation does not change acceleration limits at the present tuning; tau still varies with speed.

`JERK_RATE = 1 m/s³` belongs to the old `accelToTarget` implementation. It is not the PID's jerk setting. `RPM_PID_MAX_RPM` is a derived design-speed reference; the PID computes its active RPM cap from the per-call speed selection.

All speed/acceleration limits here constrain commands, not measurements of physical wheelchair motion.

## Controller state

For one instance, immediately before an update:

| Source field | Symbol below | Meaning |
| --- | --- | --- |
| `rpm_` | `r` | Previous output wheel RPM. |
| `acceleration_` | `a` | Stored command acceleration, RPM/s. |
| `integral_` | `I` | Accumulated RPM error multiplied by time, RPM·s. |
| `filteredRate_` | `d` | Filtered rate of change of commanded RPM, RPM/s. |
| `previousRPM_` | `r_previous` | Command RPM at the start of the preceding update. |
| `previousDt_` | `dt_previous` | Interval used for the preceding integration step. |
| `havePreviousRPM_` | — | Whether derivative history is available. |

The state never comes from measured wheel speed.

## Exact update sequence

In the equations, `clip(x, lo, hi)` means restricting `x` to that interval. Let `C = LINEAR_TO_RPM`.

### 1. Validate the call

The method calls `reset()` and returns zero if:

- constructor tuning is invalid;
- `requestedRPM` is NaN or infinite;
- `dt` is NaN, infinite, zero or negative;
- either runtime limit is NaN, infinite, zero or negative.

A zero speed **or acceleration** selection thus means zero output and cleared state. It does not mean “keep the previous speed with no further acceleration.” Negative selections are not converted to their absolute values.

Finite selections above design limits are accepted, then capped in the next step.

### 2. Calculate active hard limits

```text
speed_limit_ms = min(maxSpeedMS, MAX_SPEED)
max_rpm        = speed_limit_ms × C
user_accel_ms2 = min(maxAccelMS2, MAX_ACCEL)
```

The input target remains unclipped. Output clipping happens at the end.

### 3. Update the derivative measurement

If history exists:

```text
raw_rate = (r − r_previous) / dt_previous
alpha    = dt / (derivativeFilterS + dt)
d        = d + alpha × (raw_rate − d)
```

The numerator measures the RPM change produced by the preceding update, so it is divided by that update's interval. The filter coefficient uses the current interval. This distinction matters when `dt` varies.

Then the method saves the current `r` and current `dt` as history for the following call.

On the first call, or immediately after history is cleared, no rate estimate is added; `d` starts from zero. With filter time zero, `alpha = 1` and there is no filtering. With the current 20 ms filter and 5 ms update, `alpha = 0.2`.

The derivative term uses command motion, not the derivative of the requested target. A step in the requested RPM therefore does not directly create a derivative kick.

### 4. Blend low- and high-speed tuning

```text
u        = clip(abs(r) / max_rpm, 0, 1)
fraction = u² × (3 − 2u)
```

`fraction` is a smoothstep blend weight. It is zero at rest, 0.5 at half the active speed cap, and one at or above that cap. It is based on the magnitude of this wheel's current command, not its requested target, and not the other wheel.

```text
tau = lowSpeedTauS + fraction × (highSpeedTauS − lowSpeedTauS)

low_accel  = min(lowSpeedAccelMS2,  user_accel_ms2) × C
high_accel = min(highSpeedAccelMS2, user_accel_ms2) × C
accel_limit = low_accel + fraction × (high_accel − low_accel)
```

Changing the speed selection also changes this blend: the same RPM represents a larger fraction of a smaller selected maximum speed.

### 5. Calculate PID-requested acceleration

```text
error = requestedRPM − r
requested_accel = error / tau + ki × I − kd × d
```

This PID's output is acceleration in RPM/s, not an absolute RPM setpoint.

- **P:** `error / tau`, equivalent to `Kp = 1 / tau`. Present Kp varies from approximately 28.571/s near rest to 10/s near the active speed cap. Smaller tau means more aggressive correction.
- **I:** `ki × I`. It compensates persistent error in the command-state calculation when enabled. It cannot compensate actual motor load without measured motor feedback. The current Ki is zero.
- **D:** `−kd × d`. Opposes motion of the commanded RPM. It does not read acceleration or angular velocity from an IMU.

Tau is not an exact rise time or settling time. Even an ideal proportional-only unconstrained integrator reaches approximately 63% of a step in one tau, not 100%; D, filtering, speed scheduling and limits change the actual response further.

### 6. Apply requested-acceleration and jerk limits

First restrict what acceleration the PID requests:

```text
bounded_accel = clip(requested_accel, −accel_limit, +accel_limit)
jerk_step     = maxJerkRPMPerS2 × dt
a             = a + clip(bounded_accel − a, −jerk_step, +jerk_step)
```

The stored acceleration moves toward the bounded request. It does not normally jump to that request.

Then enforce the user/design acceleration cap on the stored state itself:

```text
user_accel_limit = user_accel_ms2 × C
a = clip(a, −user_accel_limit, +user_accel_limit)
```

**A lower user-selected acceleration cap takes effect in this call, bypassing jerk limiting where necessary.** An increased cap does not force acceleration upward; normal PID and jerk calculations determine subsequent acceleration.

The final hard clamp is to the user/design cap, not to the interpolated `accel_limit`. If the speed blend reduces a tuned acceleration bound, stored acceleration can approach that smaller bound gradually through the jerk limiter, while remaining within the user/design cap.

At the current 400 RPM/s² jerk and 5 ms interval, ordinary acceleration changes by at most **2 RPM/s per update**, equivalent to approximately **0.0633 m/s² per update**.

### 7. Conditionally update the integral

If Ki is positive, update the integral only when either:

```text
abs(requested_accel − a) < 0.00001
```

or:

```text
error × (requested_accel − a) <= 0
```

The first condition means the requested acceleration was effectively delivered. The second permits integration when its direction opposes the unapplied correction. Otherwise the integral is frozen, preventing accumulation in the direction blocked by acceleration/jerk constraints.

When an update is permitted:

```text
integral_limit = low_accel / ki
I = clip(I + error × dt, −integral_limit, +integral_limit)
```

The PID request for this call was already calculated using the previous integral. The updated integral first affects the following call. The integral bound is applied when integration occurs, not unconditionally every update. With Ki zero, this branch is skipped entirely.

### 8. Integrate command RPM

```text
candidate_rpm = r + a × dt
```

The method resets and returns zero if the candidate RPM, filtered derivative or integral is nonfinite.

For example, from a fresh state, a sufficiently large positive request with the current default settings at 5 ms is acceleration-limited and jerk-limited:

```text
stored acceleration after first call = 400 × 0.005 = 2 RPM/s
first output RPM                     = 0 + 2 × 0.005 = 0.01 RPM
```

This is a calculation of the command, not a prediction of the motor's physical movement.

### 9. Clip the output and return it

```text
r = clip(candidate_rpm, −max_rpm, +max_rpm)
```

If clipping actually changes the candidate RPM, the method clears acceleration, integral, filtered derivative and the derivative-history flag. It retains the clipped output RPM. The next call therefore starts its dynamics from that boundary without stale acceleration pushing further outside it.

When a lower speed selection is below the existing output speed, this final clip may produce an immediate output change larger than the normal acceleration/jerk limits. The speed cap has priority. Likewise, resets and invalid-input returns bypass the smoothing limits. Normal requests can overshoot their target within the absolute output speed bounds because the stored acceleration takes time to reverse.

## Sensor-to-motor pipeline in `simple_drive.cpp`

The controller receives a wheel target after the following processing:

1. Read F/B/L/R sensors, subtract calibrated floors and clamp negatives to zero.
2. Apply sensor gains, then sensor saturation. Current rear gain is 4; the other gains are 1. Current code caps all four using `frontSaturation`.
3. Normalize `F − B` to speed and `R − L` to turn, each in `[-1, 1]`, using the corresponding directional saturation limits.
4. Set a component to zero when its magnitude is strictly less than `DEADBAND = 0.05`.
5. On the scheduled update, apply the power response separately to speed and turn.
6. Mix shaped components into left/right normalized demands, clip each demand and convert to wheel RPM.
7. Update each wheel's independent PID and send the resulting command to its VESC.

### Input response curve

The configurable knee is currently `(0.5, 0.25)`:

```text
p = log(INPUT_RESPONSE_KNEE_OUTPUT) / log(INPUT_RESPONSE_KNEE_INPUT)
shaped(x) = sign(x) × abs(x)^p
```

The exponent is calculated once outside the loop. The current exponent is 2. The mapping is odd-symmetric, keeps zero and full scale, and gives ±0.25 output at ±0.5 input. Both speed and turn use the same knee.

Compile-time assertions require `0 < kneeInput < 1` and `0 < kneeOutput <= kneeInput`. Equal coordinates give a linear response; output below input reduces small-input sensitivity.

The deadband is not remapped to a new zero-to-one interval. For example, an input just below 0.05 becomes zero; an input exactly 0.05 survives the strict deadband check and maps to 0.0025 with the default curve. Changing the response curve changes the target shape, not the PID dynamics.

### Wheel mixing and current runtime selections

```cpp
leftTargetRPM  = constrain(speed + turn, -1.0f, 1.0f) * speedToRPM(MAX_SPEED);
rightTargetRPM = constrain(speed - turn, -1.0f, 1.0f) * speedToRPM(MAX_SPEED);

leftRpms  = leftRPMSetpointPID.smoothRPM(leftTargetRPM, dt, MAX_SPEED, MAX_ACCEL);
rightRpms = rightRPMSetpointPID.smoothRPM(rightTargetRPM, dt, MAX_SPEED, MAX_ACCEL);
```

Positive speed requests both wheels forward. Positive turn increases the left target relative to the right. The components are normalized translation/turn commands, not physical velocity and yaw-rate measurements.

`userMaxSpeedMS` and `userMaxAccelMS2` exist and are initialized to the design limits, but **the active calls currently pass `MAX_SPEED` and `MAX_ACCEL` directly**. Changing those runtime variables alone will not change operation until the caller uses them. Knob reading is not implemented here. Targets are currently scaled by design speed as well; using a smaller output cap alone would clip high demand instead of rescaling the complete input range to the selected maximum.

The controller itself leaves `requestedRPM` unclipped. The caller's normalized `speed ± turn` clipping is a separate wheel-mixing operation.

### Timing

The caller uses `millis()` and a 5 ms minimum interval before the PID and CAN section. It then passes the fixed shared `dt = 0.005` seconds. The gate does not guarantee exactly 5 ms between calls: blocking work or scheduling delays can make an interval longer. The PID does not detect that discrepancy. If a future caller supplies measured intervals, they must be in seconds; the derivative history supports varying intervals.

### Stop behavior

When `stopped` is true, `simple_drive.cpp` sets both output RPMs to zero, resets both controller instances and sends zero to both VESCs. It bypasses the normal smoothing calculation. On restart, the next valid PID update starts from zero.

The current condition is `if (stopped /*|| !seated*/)`: the calculated `seated` value is not part of the active stop condition. The PID helper does not implement occupancy, CAN freshness or other application-level interlocks.

### VESC units and logging

`vescSetWheelRPM` currently accepts an `int32_t` wheel RPM argument. Passing the controller's float output therefore truncates fractional wheel RPM before conversion to electrical RPM. The helper then multiplies by `14.5454 × 4` and rounds the electrical RPM for the CAN payload. This conversion belongs to the VESC helper, not the PID.

The current serial printout reports wheel RPM commands rounded to whole RPM and separately prints VESC status ERPM. Those values use different units; neither serial rounding nor the CAN helper's integer input should be mistaken for extra PID filtering. The printout occurs every 20 scheduled passes, approximately every 100 ms at the nominal update rate. The input speed/turn values printed are the values after the response curve.

## Tuning interpretation

| Desired change | Relevant setting and tradeoff |
| --- | --- |
| Lower sensitivity for small torso inputs | Lower the response knee output, or otherwise select a larger response exponent. This changes input mapping before PID. |
| Steeper large RPM ramps | Raise the permitted acceleration, within the 2.85 m/s² design cap. A large-error ramp is often acceleration-limited rather than tau-limited. |
| More rounded ramp corners | Lower `maxJerkRPMPerS2`. This delays acceleration changes and can increase target-crossing overshoot. |
| Faster tracking near the target | Lower the relevant tau, increasing Kp. This may increase overshoot or oscillation; it may barely affect acceleration-limited portions. |
| More opposition to changes in command RPM | Increase Kd. This changes dynamics and can add lag; its effect interacts with filtering and the discrete update period. |
| More smoothing of the derivative signal | Increase `derivativeFilterS`. This filters D's measurement, not the input target directly, and adds phase delay. |
| Enable accumulated-error correction | Increase Ki from zero. This introduces state and potential overshoot; it does not add real motor-speed feedback. |

A trapezoidal RPM profile is expected when acceleration reaches a constant bound. Rounded acceleration transitions produce more of an S-shaped profile. Smaller tau cannot bypass the configured acceleration and jerk bounds. There is no general guarantee of monotonic or critically damped response for arbitrary tuning or inputs.

## Validation and limits of the guarantees

Constructor tuning must satisfy:

- finite, positive low/high tau;
- finite, nonnegative Ki, Kd and derivative filter time;
- finite, positive low/high acceleration tuning;
- high-speed acceleration tuning no greater than low-speed acceleration tuning;
- finite, positive jerk setting.

These checks validate parameter ranges, not closed-loop stability or comfort. The constructor does not validate combinations against a particular `dt`, motor or vehicle model.

For ordinary finite calculations, the returned RPM magnitude stays within the active user/design speed cap. The internal command acceleration stays within the active user/design acceleration cap. However, **returned-command differences do not obey that acceleration bound during hard output clipping or reset-to-zero events**. The jerk bound applies to ordinary acceleration updates; user-cap reductions and state resets can bypass it. The method does not guarantee stopping distance, actual motor acceleration, actual maximum vehicle speed, traction or wheel-speed tracking.

The Python replay and its historical response numbers are not a specification of the current firmware. The C++ version uses per-wheel speed scheduling, per-call speed/acceleration caps and a hard output clamp, and the current tuning differs from earlier Python experiments. Re-evaluate responses when changing the settings or update interval.
