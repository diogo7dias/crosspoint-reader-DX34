#!/usr/bin/env bash
# Spawns a resilient background serial reader for the DX34 device after a
# debug flash. Auto-reconnects on USB drops. Appends to /tmp/dx34_serial.log
# so the hook can fire across multiple flashes without losing prior context.
#
# Idempotent: kills any prior instance reading /dev/cu.usbmodem* before
# starting a new one so the new debug session sees the freshly booted device.

set -u

LOG=/tmp/dx34_serial.log

# Kill any prior reader holding a /dev/cu.usbmodem* port. Match on the python
# invocation so we do not nuke unrelated `cu` usage.
pkill -f "serial_monitor_bg_inner|cu.usbmodem.*serial.Serial" 2>/dev/null || true

# Detach with nohup + & + disown so the reader survives Claude's tool
# boundary and continues running after this hook exits. macOS lacks setsid.
nohup python3 -c '
import serial, sys, time, glob, os
# tag for pkill matching above
os.environ["_DX34_TAG"] = "serial_monitor_bg_inner"
LOG = "/tmp/dx34_serial.log"
with open(LOG, "ab", buffering=0) as fh:
    fh.write(b"\n--- monitor started " + str(int(time.time())).encode() + b" ---\n")
    while True:
        ports = glob.glob("/dev/cu.usbmodem*")
        if not ports:
            time.sleep(0.5); continue
        try:
            s = serial.Serial(ports[0], 115200, timeout=1)
            fh.write(f"\n--- connected {ports[0]} ---\n".encode())
            while True:
                data = s.read(4096)
                if data:
                    fh.write(data)
        except Exception as e:
            fh.write(f"\n--- disconnect: {e} ---\n".encode())
            time.sleep(1)
' >>"$LOG" 2>&1 &
disown

echo "[serial-monitor] background reader spawned, tail -f $LOG"
