# lean-control-firmware

See [RPM setpoint PID documentation](docs/RPM_SETPOINT_PID.md) for the controller API, equations, tuning, limits and current firmware integration.



TODO:
consider making calibration non blocking. (chat thinks it can cause issues if everything else is suspended for a while)

issues from astra
1. High — Link loss does not bypass motor-command smoothing.
   Remote timeout sets the requested targets to zero, but setMotorPower() still runs those targets through the PID smoothers. The controllers retain their RPM, acceleration, and integral history, so nonzero commands can continue after the link has timed out—even while telemetry says “Stopped.” Soft stop and mode changes also retain smoother state. This conflicts with the PID helper’s own instruction to reset and bypass smoothing on a stop/fault.
   Recommendation: distinguish ordinary zero input from fault/stop conditions and explicitly define the motor-output behavior for each.
   References: RemoteDriveMode.cpp:54 (/mnt/c/Users/Caleb/Documents/School/Boise State University/Semesters/Senior Design/lean-control-firmware/wheelchair-firmware/src/driveModes/RemoteDriveMode.cpp:54), main.cpp:53 (/mnt/c/Users/Caleb/Documents/School/Boise State University/Semesters/Senior Design/lean-control-firmware/wheelchair-firmware/src/main.cpp:53).
2. High — Calibration can suspend motor updates and input handling for over one second.
   Calibration allows 500 samples and delays 2 ms per sample, in addition to ADC work. The physical calibration button can invoke this while already driving in LEAN; there is no motion check or explicit zero command before entering the blocking loop. During calibration, the main loop cannot process mode buttons, Bluetooth packets, or motor updates. What the motors do during that gap depends on their separately configured timeout behavior.
   Recommendation: require a stopped state and implement calibration without blocking the control loop.
   References: LeanDriveMode.cpp:82 (/mnt/c/Users/Caleb/Documents/School/Boise State University/Semesters/Senior Design/lean-control-firmware/wheelchair-firmware/src/driveModes/LeanDriveMode.cpp:82), main.cpp:239 (/mnt/c/Users/Caleb/Documents/School/Boise State University/Semesters/Senior Design/lean-control-firmware/wheelchair-firmware/src/main.cpp:239).
3. High — A lost configuration acknowledgment can make “Revert” falsely report success.
   Consider a clean configuration: the device accepts a Set request, but its reply is lost. The browser times out with the old field value and link.dirty === false. Clicking Revert then skips the device Revert command, clears the draft, and displays “Reverted to saved settings.” The device still has the changed value in RAM. The summary can also incorrectly say its values match saved settings.
   Recommendation: treat timed-out mutations as having an unknown outcome; reconcile with the device before claiming success or allowing a local-only revert.
   References: config-protocol.js:266 (config-protocol.js:266), configuration.js:691 (configuration.js:691).
4. Medium — The documented configuration limits do not match accepted values.
   The guide promises a watchdog no longer than 100 ms, a 4–10 ms control period, and jerk capped at 400 RPM/s². The declarations actually accept 500 ms, 2–15 ms, and 3,000 RPM/s², respectively. These larger values reach the runtime consumers; they are not merely display limits.
   Recommendation: settle the intended limits and align the declarations, comments, and guide. The current documentation overstates the restrictions.
   References: DriveSettings.cpp:84 (/mnt/c/Users/Caleb/Documents/School/Boise State University/Semesters/Senior Design/lean-control-firmware/wheelchair-firmware/src/config/DriveSettings.cpp:84), DriveSettings.cpp:115 (/mnt/c/Users/Caleb/Documents/School/Boise State University/Semesters/Senior Design/lean-control-firmware/wheelchair-firmware/src/config/DriveSettings.cpp:115), CONFIGURATION.md:114 (/mnt/c/Users/Caleb/Documents/School/Boise State University/Semesters/Senior Design/lean-control-firmware/wheelchair-firmware/docs/CONFIGURATION.md:114).
5. Medium — “Exact-value” inputs conceal the values actually being applied.
   Floating-point inputs display only two decimal places while retaining a different precise value in dataset.precise. For example, 0.035 displays as 0.04, and a valid response-curve coordinate of 0.001 displays as 0. Applying the form uses the hidden precise value. Distinct configurations therefore look identical, and retyping the displayed number can change the setting.
   Recommendation: show round-trippable precision in the editable input; reserve rounding for summaries.
   Reference: configuration.js:267 (configuration.js:267).
6. Medium — Valid multi-field changes can be impossible to apply directly.
   The browser sends each draft as a separate Set, and firmware validates the VESC-ID inequality after every field. Swapping the default IDs from (26, 2) to (2, 26) fails whichever field is sent first, although the final pair is valid. Other earlier edits may already have taken effect when this failure occurs.
   Recommendation: support an atomic update for related fields, or explicitly guide the user through a temporary unused ID.
   References: configuration.js:206 (configuration.js:206), DriveSettings.cpp:135 (/mnt/c/Users/Caleb/Documents/School/Boise State University/Semesters/Senior Design/lean-control-firmware/wheelchair-firmware/src/config/DriveSettings.cpp:135).
7. Medium — Calibration accepts zero normalization denominators.
   Saturation values are integer conversions of factor × userWeightEstimate, and calibration unconditionally marks itself successful. Zero readings—or sufficiently small readings with the permitted 0.2 factor—produce zero saturation. Subsequent normalization can evaluate 0 / 0, producing NaN. The PID’s finite-value check offers a downstream safeguard, but calibration and telemetry still represent this invalid state as calibrated/enabled.
   Recommendation: validate the weight reference and all saturation values before accepting calibration.
   Reference: LeanDriveMode.cpp:98 (/mnt/c/Users/Caleb/Documents/School/Boise State University/Semesters/Senior Design/lean-control-firmware/wheelchair-firmware/src/driveModes/LeanDriveMode.cpp:98).
8. Medium — Motor commands lose fractional wheel RPM before conversion to electrical RPM.
   The smoother produces floats, but vescSetWheelRPM() takes an int32_t wheel RPM argument. Consequently, 0.9 RPM becomes zero and 1.9 RPM becomes one before conversion. Telemetry and previews show the untruncated values, obscuring the actual staircase in motor commands.
   Recommendation: retain floating-point wheel RPM until the final electrical-RPM rounding.
   Reference: vesc_can.h:69 (/mnt/c/Users/Caleb/Documents/School/Boise State University/Semesters/Senior Design/lean-control-firmware/wheelchair-firmware/src/vesc_can.h:69).
9. Medium — PID timing assumes the configured interval rather than elapsed time.
   updateDrive() checks the clock but always supplies period / 1000 as dt. After a delayed loop, including calibration or synchronous persistence work, the smoother still receives one nominal interval. This distorts its derivative and time-dependent behavior and weakens agreement with the preview.
   Recommendation: measure elapsed time and define explicit handling for excessive delays.
   Reference: main.cpp:200 (/mnt/c/Users/Caleb/Documents/School/Boise State University/Semesters/Senior Design/lean-control-firmware/wheelchair-firmware/src/main.cpp:200).
10. Medium — A missing Bluetooth delimiter can prevent framing recovery.
    BluetoothReceiver toggles between text and binary mode on every zero byte. Losing one delimiter shifts that alternation: subsequent CSV can be consumed as binary, while valid replies are consumed as text. The frame-length limit bounds memory but does not reset the framing state. Reconnecting resets it.
    Recommendation: add a recovery strategy for invalid/oversized frames or incomplete-frame timeouts.
    Reference: bluetooth.js:28 (bluetooth.js:28).
11. Medium — Migration can leave “saved” settings absent from EEPROM.
    Loading establishes the dirty baseline after applying defaults and migration repairs. If an older valid bank lacks a newly added field, the new default is considered clean, and save() returns early because a valid bank exists. The dashboard also disables Save without edits. That new value is therefore not committed; a later firmware default change can silently change it again.
    Recommendation: track whether storage needs a migration write separately from whether the user changed RAM values.
    Reference: Persistence.cpp:95 (/mnt/c/Users/Caleb/Documents/School/Boise State University/Semesters/Senior Design/lean-control-firmware/wheelchair-firmware/src/config/Persistence.cpp:95).
12. Medium — One browser can disconnect USB monitoring for every browser.
    USB connection state is one global server Boolean. Every client sends connect/disconnect commands based on its own selected transport. A second browser choosing Bluetooth therefore stops USB acquisition for an existing USB viewer; reconnecting another USB client reverses it.
    Recommendation: define shared acquisition ownership or track each client’s demand for USB monitoring.
    References: app.py:120 (app.py:120), bluetooth.js:51 (bluetooth.js:51).

I would prioritize stop/fault handling, blocking calibration, and uncertain configuration-write outcomes. The most valuable follow-up tests are end-to-end cases spanning those boundaries; isolated registry and protocol tests cannot establish the resulting motor behavior or dashboard truthfulness