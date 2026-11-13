# lean-control-firmware

See [RPM setpoint PID documentation](docs/RPM_SETPOINT_PID.md) for the controller API, equations, tuning, limits and current firmware integration.



TODO:
consider making calibration non blocking. (chat thinks it can cause issues if everything else is suspended for a while)

outstanding possible issues from astra. I don't want to play with the code any more but need to test it on the wheelchair before messing with stuff. 

1. High — A lost configuration acknowledgment can make “Revert” falsely report success.
   Consider a clean configuration: the device accepts a Set request, but its reply is lost. The browser times out with the old field value and link.dirty === false. Clicking Revert then skips the device Revert command, clears the draft, and displays “Reverted to saved settings.” The device still has the changed value in RAM. The summary can also incorrectly say its values match saved settings.
   Recommendation: treat timed-out mutations as having an unknown outcome; reconcile with the device before claiming success or allowing a local-only revert.
   References: config-protocol.js:266 (config-protocol.js:266), configuration.js:691 (configuration.js:691).
2. Medium — The documented configuration limits do not match accepted values.
   The guide promises a watchdog no longer than 100 ms, a 4–10 ms control period, and jerk capped at 400 RPM/s². The declarations actually accept 500 ms, 2–15 ms, and 3,000 RPM/s², respectively. These larger values reach the runtime consumers; they are not merely display limits.
 How to fix: modify the comments such that they are agnostic of the hardcoded values. 
   References: DriveSettings.cpp:84 (/mnt/c/Users/Caleb/Documents/School/Boise State University/Semesters/Senior Design/lean-control-firmware/wheelchair-firmware/src/config/DriveSettings.cpp:84), DriveSettings.cpp:115 (/mnt/c/Users/Caleb/Documents/School/Boise State University/Semesters/Senior Design/lean-control-firmware/wheelchair-firmware/src/config/DriveSettings.cpp:115), CONFIGURATION.md:114 (/mnt/c/Users/Caleb/Documents/School/Boise State University/Semesters/Senior Design/lean-control-firmware/wheelchair-firmware/docs/CONFIGURATION.md:114).
3. Medium — “Exact-value” inputs conceal the values actually being applied.
   Floating-point inputs display only two decimal places while retaining a different precise value in dataset.precise. For example, 0.035 displays as 0.04, and a valid response-curve coordinate of 0.001 displays as 0. Applying the form uses the hidden precise value. Distinct configurations therefore look identical, and retyping the displayed number can change the setting.
   how to fis: display 3 digits instead of 2. 
   Reference: configuration.js:267 (configuration.js:267).
4. Medium — Valid multi-field changes can be impossible to apply directly.
   The browser sends each draft as a separate Set, and firmware validates the VESC-ID inequality after every field. Swapping the default IDs from (26, 2) to (2, 26) fails whichever field is sent first, although the final pair is valid. Other earlier edits may already have taken effect when this failure occurs.
   Recommendation: support an atomic update for related fields, or explicitly guide the user through a temporary unused ID.
   References: configuration.js:206 (configuration.js:206), DriveSettings.cpp:135 (/mnt/c/Users/Caleb/Documents/School/Boise State University/Semesters/Senior Design/lean-control-firmware/wheelchair-firmware/src/config/DriveSettings.cpp:135).
5. Medium — Calibration accepts zero normalization denominators.
   Saturation values are integer conversions of factor × userWeightEstimate, and calibration unconditionally marks itself successful. Zero readings—or sufficiently small readings with the permitted 0.2 factor—produce zero saturation. Subsequent normalization can evaluate 0 / 0, producing NaN. The PID’s finite-value check offers a downstream safeguard, but calibration and telemetry still represent this invalid state as calibrated/enabled.
   Recommendation: validate the weight reference and all saturation values before accepting calibration.
   Reference: LeanDriveMode.cpp:98 (/mnt/c/Users/Caleb/Documents/School/Boise State University/Semesters/Senior Design/lean-control-firmware/wheelchair-firmware/src/driveModes/LeanDriveMode.cpp:98).
6. Medium — PID timing assumes the configured interval rather than elapsed time.
   updateDrive() checks the clock but always supplies period / 1000 as dt. After a delayed loop, including calibration or synchronous persistence work, the smoother still receives one nominal interval. This distorts its derivative and time-dependent behavior and weakens agreement with the preview.
   Recommendation: measure elapsed time and define explicit handling for excessive delays.
   Reference: main.cpp:200 (/mnt/c/Users/Caleb/Documents/School/Boise State University/Semesters/Senior Design/lean-control-firmware/wheelchair-firmware/src/main.cpp:200).
7.  Medium — A missing Bluetooth delimiter can prevent framing recovery.
    BluetoothReceiver toggles between text and binary mode on every zero byte. Losing one delimiter shifts that alternation: subsequent CSV can be consumed as binary, while valid replies are consumed as text. The frame-length limit bounds memory but does not reset the framing state. Reconnecting resets it.
    Recommendation: add a recovery strategy for invalid/oversized frames or incomplete-frame timeouts.
    Reference: bluetooth.js:28 (bluetooth.js:28).
