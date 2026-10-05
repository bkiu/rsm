#!/usr/bin/env bash
# Back up or restore the Cardputer over USB serial.
#   ./backup.sh export [file]     save identity key, settings, channels, nodes + keys, quick messages
#                                 (default file: backups/rsm-<date>.txt)
#   ./backup.sh restore <file>    send a backup's commands back to the device
# PORT=/dev/ttyACM1 ./backup.sh ...  to pick the port. Close monitor.sh first: only one program can hold the port.
set -euo pipefail
cd "$(dirname "$0")"

mode="${1:-}"
file="${2:-}"
case "$mode" in
export) [ -n "$file" ] || { mkdir -p backups; file="backups/rsm-$(date +%Y%m%d-%H%M%S).txt"; } ;;
restore) [ -f "$file" ] || { echo "usage: $0 restore <backup file>" >&2; exit 1; } ;;
*) sed -n '2,7p' "$0" | sed 's/^# \{0,1\}//'; exit 1 ;;
esac

port="${PORT:-$(ls /dev/ttyACM* 2>/dev/null | head -n1 || true)}"
[ -n "$port" ] || { echo "No /dev/ttyACM* found. Is the Cardputer plugged in and on?" >&2; exit 1; }

exec nix-shell -p 'python3.withPackages (p: [ p.pyserial ])' --run "python3 - '$mode' '$port' '$file'" <<'PY'
import os, sys, time, serial

mode, port, path = sys.argv[1:4]
ser = serial.Serial(port, 115200, timeout=0.2)

def read_until(marker, timeout):
    buf, end = b"", time.time() + timeout
    while time.time() < end:
        buf += ser.read(4096)
        if marker in buf:
            return buf
    raise SystemExit(f"timed out waiting for the device on {port} (is monitor.sh still open?)")

def command(line, timeout=10):
    ser.write(line.encode() + b"\r")
    return read_until(b"\n> ", timeout)

ser.write(b"\r")  # get a fresh prompt and flush anything pending
time.sleep(0.5)
ser.reset_input_buffer()

if mode == "export":
    out = command("export", 30).decode("utf-8", "replace")
    start, end = out.find("--- BEGIN RSM EXPORT ---"), out.find("--- END RSM EXPORT ---")
    if start < 0 or end < 0:
        raise SystemExit("didn't get an export back; is the firmware up to date?")
    body = out[out.index("\n", start) + 1:end].replace("\r", "")
    # Keep only command and comment lines (drops any 'log on' packet lines that interleaved).
    keep = [l for l in body.split("\n") if l and not l.startswith("rx !")]
    old = os.umask(0o077)  # it holds private keys
    with open(path, "w") as f:
        f.write("\n".join(keep) + "\n")
    os.umask(old)
    nodes = sum(l.startswith("nodeadd ") for l in keep)
    print(f"saved {path}: {nodes} nodes, {sum(l.startswith('quick ') for l in keep)} quick messages")
else:
    lines = [l.rstrip("\n") for l in open(path, encoding="utf-8")]
    lines = [l for l in lines if l.strip() and not l.startswith("#")]
    problems = 0
    for i, l in enumerate(lines, 1):
        reply = command(l).decode("utf-8", "replace")
        text = reply.split("\n", 1)[1] if "\n" in reply else ""  # drop the echoed command
        text = text.rsplit("\n> ", 1)[0].strip()
        if any(w in text for w in ("unknown", "usage", "bad ", "already exists", "needs", "in use", "at most")):
            problems += 1
            print(f"line {i}: {l}\n    -> {text}")
        print(f"\r{i}/{len(lines)}", end="", file=sys.stderr, flush=True)
    print(file=sys.stderr)
    print("restored" + (f", {problems} lines need a look (above)" if problems else ""))
    print("Reboot the device (type 'reboot' in monitor.sh) so the radio picks up everything cleanly.")
PY
