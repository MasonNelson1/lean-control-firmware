#!/usr/bin/env bash
# Runs every host-side test: native firmware tests (g++), laptop tools tests
# (python3), dashboard JS tests (node) and dashboard Python tests. No hardware needed.
#
#   ./run-tests.sh            # everything
#   ./run-tests.sh native     # just the C++ tests
#   ./run-tests.sh tools      # just the laptop tools (tools/tests)
#   ./run-tests.sh viewer     # just the dashboard tests
#
# Python: set VIEWER_PYTHON to a venv's python with debugViewer/requirements.txt
# installed. Defaults to ~/.venvs/lean-viewer, then debugViewer/.venv.
set -uo pipefail
cd "$(dirname "$0")"

OUT=.pio/native-tests
mkdir -p "$OUT"
CXX=(g++ -std=c++17 -Wall -Wextra -Werror)
CONFIG_SRC=(src/config/Registry.cpp src/config/Persistence.cpp src/config/Protocol.cpp)
SETTINGS_SRC=(src/config/DriveSettings.cpp "${CONFIG_SRC[@]}")
# lean_stubs supplies Arduino.h (pins, analogRead); config_stubs supplies EEPROM.h.
LEAN_INC=(-I tests/native/lean_stubs -I tests/native/config_stubs -I src)
LEAN_SRC=(src/driveModes/LeanDriveMode.cpp src/driveModes/lean_layout.cpp)

failures=()

native() { # name, then compiler args
    local name=$1; shift
    printf '%-22s ' "$name"
    if ! "${CXX[@]}" "$@" -o "$OUT/$name" 2>"$OUT/$name.compile.log"; then
        echo "COMPILE FAILED"; sed 's/^/    /' "$OUT/$name.compile.log" | head -20
        failures+=("$name"); return
    fi
    if "$OUT/$name" >"$OUT/$name.log" 2>&1; then
        echo "ok  $(tail -n1 "$OUT/$name.log")"
    else
        echo "FAILED"; sed 's/^/    /' "$OUT/$name.log" | tail -20
        failures+=("$name")
    fi
}

run_native() {
    echo "== Native firmware tests"
    native config_exhaustive   -I src tests/native/config_exhaustive_test.cpp "${CONFIG_SRC[@]}"
    native config              -I tests/native/config_stubs -I src tests/native/config_test.cpp "${CONFIG_SRC[@]}"
    native drive_settings      "${LEAN_INC[@]}" tests/native/drive_settings_test.cpp "${SETTINGS_SRC[@]}"
    native drive_knobs         -I src tests/native/drive_knobs_test.cpp
    native rpm_setpoint_pid    -I src tests/native/rpm_setpoint_pid_test.cpp
    native mode_state_machine  -I src tests/native/mode_state_machine_test.cpp src/ModeStateMachine.cpp
    native telemetry           -I tests/native/telemetry_stubs -I src tests/native/telemetry_test.cpp \
                               src/telemetry/TelemetryFrame.cpp src/telemetry/Telemetry.cpp
    native lean_normalization  "${LEAN_INC[@]}" tests/native/lean_normalization_test.cpp \
                               "${LEAN_SRC[@]}" "${SETTINGS_SRC[@]}"
    native dynamic_calibration "${LEAN_INC[@]}" tests/native/dynamic_calibration_test.cpp \
                               "${LEAN_SRC[@]}" "${SETTINGS_SRC[@]}"
    native corner_layout       "${LEAN_INC[@]}" tests/native/corner_layout_test.cpp \
                               "${LEAN_SRC[@]}" "${SETTINGS_SRC[@]}"
    native corner_calibration  "${LEAN_INC[@]}" tests/native/corner_calibration_test.cpp \
                               "${LEAN_SRC[@]}" "${SETTINGS_SRC[@]}"
    native plus_golden         "${LEAN_INC[@]}" tests/native/plus_golden_test.cpp \
                               "${LEAN_SRC[@]}" "${SETTINGS_SRC[@]}"
    native vesc_decode         -I src tests/native/vesc_decode_test.cpp
    # vesc_stubs supplies Arduino.h (millis, Serial1) and FlexCAN_T4.h (records frames).
    native vesc_monitor        -I tests/native/vesc_stubs -I tests/native -I src tests/native/vesc_monitor_test.cpp \
                               src/telemetry/Telemetry.cpp src/telemetry/TelemetryFrame.cpp
}

run_viewer() {
    echo "== Dashboard JS tests"
    if [ ! -d debugViewer/node_modules ]; then (cd debugViewer && npm ci --silent) || failures+=(npm-ci); fi
    if (cd debugViewer && node --test tests/*.test.cjs >"../$OUT/viewer-js.log" 2>&1); then
        echo "ok  $(grep -E '^# (pass|fail)' "$OUT/viewer-js.log" | tr '\n' ' ')"
    else
        echo "FAILED"; tail -30 "$OUT/viewer-js.log"; failures+=(viewer-js)
    fi

    echo "== Dashboard Python tests"
    local py=${VIEWER_PYTHON:-}
    for candidate in "$HOME/.venvs/lean-viewer/bin/python" debugViewer/.venv/bin/python; do
        [ -z "$py" ] && [ -x "$candidate" ] && py=$candidate
    done
    if [ -z "$py" ] || ! "$py" -c 'import fastapi' 2>/dev/null; then
        echo "SKIPPED (no Python env with debugViewer/requirements.txt; set VIEWER_PYTHON)"
        failures+=(viewer-python-skipped); return
    fi
    if "$py" -B -m unittest discover -s debugViewer/tests >"$OUT/viewer-py.log" 2>&1; then
        echo "ok  $(grep -E '^Ran ' "$OUT/viewer-py.log")"
    else
        echo "FAILED"; tail -30 "$OUT/viewer-py.log"; failures+=(viewer-python)
    fi
}

run_tools() {
    echo "== Laptop tools tests (chair_sql, chair_summary)"
    if python3 -B -m unittest discover -s tools/tests >"$OUT/tools-py.log" 2>&1; then
        echo "ok  $(grep -E '^Ran ' "$OUT/tools-py.log")"
    else
        echo "FAILED"; tail -30 "$OUT/tools-py.log"; failures+=(tools-python)
    fi
}

case "${1:-all}" in
    native) run_native ;;
    viewer) run_viewer ;;
    tools)  run_tools ;;
    all)    run_native; run_tools; run_viewer ;;
    *)      echo "usage: $0 [native|tools|viewer|all]"; exit 2 ;;
esac

echo
if [ ${#failures[@]} -eq 0 ]; then echo "All tests passed."; else echo "Problems: ${failures[*]}"; exit 1; fi
