# VESC firmware reference (excerpts)

Unmodified source files from the VESC firmware (https://github.com/vedderb/bldc,
GPL-3.0, copyright Benjamin Vedder; licence header in each file). They're kept
here because the Teensy talks to the VESCs over CAN and writes their settings:

- `comm_can.c`: CAN frame IDs, status messages 1–5, how forwarded COMM commands are handled.
- `mcpwm_foc.c`, `foc_math.c`: the FOC current/speed control behind `s_pid_min_erpm`, braking and timeouts.

The chair's controllers are TRAMPA VESC 6 MkVI on firmware 6.06. The hardware
config and `appconf` parameter definitions are in `../../pcb/vesc-reference/`.
