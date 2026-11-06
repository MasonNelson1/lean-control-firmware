#!/usr/bin/env bash
# Run from any working directory; dependencies stay beside this script.
set -euo pipefail
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
venv_dir="$script_dir/.venv"
if [[ ! -x "$venv_dir/bin/python" ]]; then
    echo "Creating Python environment in $venv_dir"
    python3 -m venv "$venv_dir"
fi
if ! "$venv_dir/bin/python" -c 'import numpy, pandas, matplotlib' >/dev/null 2>&1; then
    echo "Installing simulation dependencies (internet required on first run)..."
    "$venv_dir/bin/python" -m pip install --disable-pip-version-check --no-cache-dir -r "$script_dir/requirements.txt"
fi
exec "$venv_dir/bin/python" "$script_dir/simulate_pid.py" "$@"
