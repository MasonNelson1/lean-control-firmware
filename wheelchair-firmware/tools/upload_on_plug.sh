#!/usr/bin/env bash
# Upload the current build whenever the Teensy is plugged in over USB (WSL).
#   tools/upload_on_plug.sh           one upload, then exit (whoever started it gets notified)
#   tools/upload_on_plug.sh --watch   keep going: every plug-in gets the latest build
# Uploads only if the build differs from the last one uploaded here. After an
# upload the chair chases its 4 mode LEDs 3 times on boot (new-firmware signal),
# and its DBG BOOT line (stamp=) matches the stamp printed here.
set -u
cd "$(dirname "$0")/.." || exit 1
marker=.pio/last-uploaded.sha256
watch=0; [ "${1:-}" = "--watch" ] && watch=1
plugged() {
    powershell.exe -NoProfile -Command \
      "if (Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | ? { \$_.InstanceId -match 'VID_16C0' }) { 'yes' }" \
      2>/dev/null | grep -q yes
}
stamp() { sed -n 's/.*BUILD_STAMP 0x\([0-9a-f]*\).*/\1/p' src/build_stamp.h 2>/dev/null; }
deadline=$(( $(date +%s) + 7200 ))
while [ "$(date +%s)" -lt "$deadline" ]; do
    if ! plugged; then sleep 3; continue; fi
    if ! pio run >/dev/null 2>&1; then echo "$(date +%T) build failed; fix it before uploading"; exit 1; fi
    want=$(sha256sum .pio/build/teensy41/firmware.hex | cut -d' ' -f1)
    if [ -f "$marker" ] && [ "$(cat "$marker")" = "$want" ]; then
        echo "$(date +%T) Teensy plugged in; already has this build (stamp $(stamp))"
    else
        echo "$(date +%T) Teensy plugged in; uploading build stamp $(stamp)"
        ok=0
        for attempt in 1 2 3; do  # the first try can land while Windows is still enumerating it
            sleep 2
            if timeout 180 pio run -t upload 2>&1 | tail -3 | grep -q SUCCESS; then ok=1; break; fi
            echo "$(date +%T) upload attempt $attempt failed"
        done
        if [ $ok = 1 ]; then
            echo "$want" > "$marker"
            echo "$(date +%T) UPLOAD OK: stamp $(stamp). The chair's 4 mode LEDs chase 3 times as it boots."
            [ $watch = 0 ] && exit 0
        else
            echo "$(date +%T) UPLOAD FAILED after 3 tries"; [ $watch = 0 ] && exit 2
        fi
    fi
    [ $watch = 0 ] && exit 0
    while plugged; do sleep 3; done   # wait for the unplug before watching again
    echo "$(date +%T) unplugged; watching for the next plug-in"
done
