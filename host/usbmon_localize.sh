#!/usr/bin/env bash
#
# usbmon_localize.sh - localize the F0/F8/FE round-trip drop seen in the Phase-2
# spike (see ../doc/spike_bringup.md "system status bytes are dropped").
#
# It captures the EP2 bulk traffic (Bo:...:02 host->device, Bi:...:02
# device->host) on the MIDEX while a continuous receiver is up and we send three
# probe sequences one at a time:
#   1. SysEx   F0 7D 10 F7
#   2. clock   F8
#   3. sensing FE
#
# Reading the result:
#   - byte present on Bo (OUT) but absent on Bi (IN)  -> device RX/TX lost it
#   - byte absent on Bo (OUT)                         -> host never emitted it
#                                                        (ALSA output filtering)
#   - byte present on Bi (IN) but not in the receiver -> host input dropped it
#
# Run with sudo (usbmon debugfs + modprobe need root):
#   sudo ./usbmon_localize.sh [hw:CARD,0,0]
#
set -u

PORT="${1:-}"
CAP=/tmp/midex_usbmon.txt
RX=/tmp/midex_rx.txt
DUR=8

# --- discover the MIDEX: bus, device address, and an ALSA port -----------------
LSUSB_LINE="$(lsusb -d 0a4e:10c1 | head -n1)"
if [ -z "$LSUSB_LINE" ]; then
	echo "No 0a4e:10c1 (spike firmware) found. Upload midex-class-r1.ihx first." >&2
	exit 1
fi
BUS="$(printf '%s\n' "$LSUSB_LINE" | sed -E 's/^Bus ([0-9]+) Device ([0-9]+):.*/\1/')"
DEV="$(printf '%s\n' "$LSUSB_LINE" | sed -E 's/^Bus ([0-9]+) Device ([0-9]+):.*/\2/')"
BUSN="$((10#$BUS))"
echo "MIDEX at bus $BUS device $DEV  ($LSUSB_LINE)"

if [ -z "$PORT" ]; then
	PORT="$(amidi -l | sed -nE 's/^\s*IO\s+(\S+)\s+MIDEX8 UAC MIDI 1.*/\1/p' | head -n1)"
fi
if [ -z "$PORT" ]; then
	echo "Could not find an ALSA port for 'MIDEX8 UAC MIDI 1'. Pass it explicitly." >&2
	exit 1
fi
echo "Using ALSA port $PORT (loop a cable OUT->IN on this port for the IN side)."

# --- load usbmon ---------------------------------------------------------------
if [ ! -d /sys/kernel/debug/usb/usbmon ]; then
	modprobe usbmon || { echo "modprobe usbmon failed" >&2; exit 1; }
fi
MONNODE="/sys/kernel/debug/usb/usbmon/${BUSN}u"
[ -r "$MONNODE" ] || { echo "usbmon node $MONNODE not readable" >&2; exit 1; }

# --- start capture + continuous receiver ---------------------------------------
: > "$CAP"; : > "$RX"
timeout "$DUR" cat "$MONNODE" > "$CAP" &
CAP_PID=$!
amidi -p "$PORT" -d > "$RX" 2>/dev/null &
RX_PID=$!
sleep 0.6

send() { echo ">>> sending: $1"; amidi -p "$PORT" -S "$1"; sleep 1.2; }
send "F0 7D 10 F7"      # SysEx (4 bytes)
send "F8"               # MIDI clock (real-time, single byte)
send "FE"               # active sensing (real-time, single byte)

sleep 0.6
kill "$RX_PID" 2>/dev/null
wait "$CAP_PID" 2>/dev/null

# --- filter to this device's EP2 bulk traffic ----------------------------------
echo
echo "================ EP2 bulk packets for device $DEV (bus $BUSN) ================"
grep -E "B[io]:${BUSN}:${DEV}:0?2 " "$CAP" \
	| sed -E 's/^[0-9a-f]+ +[0-9]+ +//' \
	| tee /tmp/midex_ep2.txt
echo
echo "================ what the receiver captured ================"
cat "$RX"
echo
echo "(full capture: $CAP   filtered EP2: /tmp/midex_ep2.txt   rx: $RX)"
