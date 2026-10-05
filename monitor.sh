#!/usr/bin/env bash
# Opens the Cardputer's serial console. Usage: ./monitor.sh [port]   (default: first /dev/ttyACM*)
# Quit with Ctrl+C.
set -euo pipefail
cd "$(dirname "$0")"

port="${1:-}"
if [ -z "$port" ]; then
    # The port disappears briefly while the device resets; wait for it.
    for _ in $(seq 20); do
        port=$(ls /dev/ttyACM* 2>/dev/null | head -n1 || true)
        [ -n "$port" ] && break
        sleep 0.5
    done
fi
if [ -z "$port" ]; then
    echo "No /dev/ttyACM* found. Is the Cardputer plugged in and on?" >&2
    exit 1
fi

echo "Connecting to $port - type 'help' and press Enter. Ctrl+C to quit."
exec nix shell nixpkgs#platformio -c pio device monitor -p "$port" -b 115200 -f direct
