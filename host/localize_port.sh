#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Copyright (C) 2026 Hedde Bosman (sgorpi@gmail.com)
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, see <https://www.gnu.org/licenses/>.

#
# localize_port.sh - pinpoint WHERE a per-port round-trip fails on the
# class-compliant MIDEX firmware by capturing the EP2 bulk traffic while sending
# a note-on on a known-good port (MIDI 1) and a failing port (MIDI 2).
#
# For each send it shows, for that device's EP2:
#   Bo (host->device, EP 0x02): the 4-byte USB-MIDI packet(s) the host emitted.
#       Byte 0 high nibble = CABLE number. So a send on "MIDI 2" must show a
#       packet whose first byte is 0x1X (cable 1). If it shows 0x0X/0x2X the
#       host mapped the ALSA port to the wrong cable (descriptor issue).
#   Bi (device->host, EP 0x82): what the firmware sent back. Empty here = the
#       device never produced an IN packet for that cable (firmware RX / TX /
#       UART-channel issue), independent of any host-side filtering.
#
# Plus what each continuous receiver actually captured.
#
# Run with sudo (usbmon debugfs + modprobe need root):
#   sudo ./localize_port.sh
#
set -u

PID=0a4e:10c1
CAP=/tmp/midex_lp_usbmon.txt
RX1=/tmp/midex_lp_rx1.txt
RX2=/tmp/midex_lp_rx2.txt
DUR=8

LSUSB_LINE="$(lsusb -d "$PID" | head -n1)"
if [ -z "$LSUSB_LINE" ]; then
	echo "No $PID firmware found. Upload midex-class-r1.ihx first." >&2
	exit 1
fi
BUS="$(printf '%s\n' "$LSUSB_LINE" | sed -E 's/^Bus ([0-9]+) Device ([0-9]+):.*/\1/')"
DEV="$(printf '%s\n' "$LSUSB_LINE" | sed -E 's/^Bus ([0-9]+) Device ([0-9]+):.*/\2/')"
BUSN="$((10#$BUS))"
echo "MIDEX at bus $BUS device $DEV  ($LSUSB_LINE)"

PORT1="$(amidi -l | sed -nE 's/^\s*IO\s+(\S+)\s+MIDEX8 UAC MIDI 1\s*$/\1/p' | head -n1)"
PORT2="$(amidi -l | sed -nE 's/^\s*IO\s+(\S+)\s+MIDEX8 UAC MIDI 2\s*$/\1/p' | head -n1)"
if [ -z "$PORT1" ] || [ -z "$PORT2" ]; then
	echo "Could not find ALSA ports for MIDI 1 / MIDI 2. 'amidi -l' shows:" >&2
	amidi -l >&2
	exit 1
fi
echo "MIDI 1 = $PORT1 (loop a cable OUT->IN here)"
echo "MIDI 2 = $PORT2 (loop a cable OUT->IN here)"

if [ ! -d /sys/kernel/debug/usb/usbmon ]; then
	modprobe usbmon || { echo "modprobe usbmon failed" >&2; exit 1; }
fi
MONNODE="/sys/kernel/debug/usb/usbmon/${BUSN}u"
[ -r "$MONNODE" ] || { echo "usbmon node $MONNODE not readable" >&2; exit 1; }

: > "$CAP"; : > "$RX1"; : > "$RX2"
timeout "$DUR" cat "$MONNODE" > "$CAP" &
CAP_PID=$!
amidi -p "$PORT1" -d > "$RX1" 2>/dev/null &
RX1_PID=$!
amidi -p "$PORT2" -d > "$RX2" 2>/dev/null &
RX2_PID=$!
sleep 0.8

echo ">>> MIDI 1: sending 90 3C 7F"
amidi -p "$PORT1" -S "90 3C 7F"; sleep 1.0
echo ">>> MIDI 2: sending 90 3D 7F"
amidi -p "$PORT2" -S "90 3D 7F"; sleep 1.0

kill "$RX1_PID" "$RX2_PID" 2>/dev/null
wait "$CAP_PID" 2>/dev/null

echo
echo "================ EP2 bulk packets (dev $DEV bus $BUSN) ================"
echo "(Bo = host->device OUT/0x02, Bi = device->host IN/0x82; '=' is the data)"
grep -E "B[io]:${BUSN}:${DEV}:0?2 " "$CAP" | sed -E 's/^[0-9a-f]+ +[0-9]+ +//'
echo
echo "================ MIDI 1 receiver ================"; cat "$RX1"
echo "================ MIDI 2 receiver ================"; cat "$RX2"
echo
echo "(full capture: $CAP)"
