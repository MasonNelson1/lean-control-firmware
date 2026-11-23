#!/usr/bin/env bash
# Wait for the Teensy to be plugged in over USB, then upload the current build.
# Usage: tools/upload_on_plug.sh   (run from wheelchair-firmware/, under WSL)
# Exits after one upload (or after 2 h), so whoever started it gets notified.
# Uploads only if the built firmware differs from the last one uploaded here.
set -u
cd "$(dirname "$0")/.." || exit 1
marker=.pio/last-uploaded.sha256
pio run >/dev/null 2>&1 || { echo "build failed; fix it before uploading"; exit 1; }
hex=.pio/build/teensy41/firmware.hex
want=$(sha256sum "$hex" | cut -d' ' -f1)
if [ -f "$marker" ] && [ "$(cat "$marker")" = "$want" ]; then
    echo "already uploaded this build ($want); nothing to do"; exit 0
fi
echo "waiting for the Teensy on USB (build $want)"
plugged() {
    powershell.exe -NoProfile -Command \
      "if (Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | ? { \$_.InstanceId -match 'VID_16C0' }) { 'yes' }" \
      2>/dev/null | grep -q yes
}
for _ in $(seq 1 1440); do   # 2 h at 5 s
    if plugged; then
        echo "Teensy detected at $(date +%T); uploading"
        # The first try sometimes lands while Windows is still enumerating the Teensy.
        for attempt in 1 2 3; do
            sleep 2
            if timeout 180 pio run -t upload 2>&1 | tail -3 | tee /dev/stderr | grep -q SUCCESS; then
                sha256sum .pio/build/teensy41/firmware.hex | cut -d' ' -f1 > "$marker"; echo "upload OK at $(date +%T)"; exit 0
            fi
            echo "upload attempt $attempt failed"
        done
        echo "upload failed"; exit 2
    fi
    sleep 5
done
echo "gave up waiting after 2 h"; exit 3
