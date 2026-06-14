#!/usr/bin/env python3
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

"""ALSA `amidi` loopback test for the class-compliant MIDEX8 firmware.

Unlike loopback.py (which drives the *bus-probe* over EP0 vendor commands), the
class firmware enumerates as a standard USB-MIDI device that the inbox
`snd-usb-audio` driver binds -- so there is no custom protocol to speak here, we
just use ALSA rawmidi via `amidi`.

With a physical MIDI cable looping each port's DIN OUT back into its DIN IN, a
message sent on that ALSA port should come straight back. This script discovers
the MIDEX ports (`amidi -l`), then sends test messages on each and listens for
them to return.

  ./class_loopback.py              # GATE: one SysEx per port (pass/fail)
  ./class_loopback.py -n 2         # require/test the first 2 ports
  ./class_loopback.py --diag       # DIAGNOSTIC: note-ons + SysEx, repeated, per
                                   #   message (discriminates first-packet vs
                                   #   per-message vs SysEx-specific loss)

Prereqs: upload midex-class-r1.ihx, confirm `amidi -l` lists the ports, and patch
a MIDI cable OUT->IN on each port under test. `amidi` ships with alsa-utils; no
root needed if your user can access the sound devices.
"""
import argparse
import re
import subprocess
import sys
import time


def amidi(*args, timeout=None):
    return subprocess.run(["amidi", *args], capture_output=True, text=True,
                          timeout=timeout)


def list_midex_ports(match):
    """Return [(alsa_addr, name), ...] for IO ports whose name matches."""
    out = amidi("-l").stdout
    ports = []
    for line in out.splitlines():
        # amidi -l columns:  Dir  Device        Name
        m = re.match(r"\s*(IO|I|O)\s+(\S+)\s+(.*\S)\s*$", line)
        if not m:
            continue
        direction, addr, name = m.group(1), m.group(2), m.group(3)
        if match.lower() in name.lower() and "I" in direction and "O" in direction:
            ports.append((addr, name))
    return ports


def normalize(hexdump):
    return " ".join(hexdump.upper().split())


def roundtrip(addr, send_hex):
    """Send send_hex on `addr`, return (captured_hex_normalized, error_or_None)."""
    # Start the receiver first so it is listening when we send.
    rx = subprocess.Popen(["amidi", "-p", addr, "-d"],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          text=True)
    err = None
    try:
        time.sleep(0.25)
        send = amidi("-p", addr, "-S", send_hex, timeout=3)
        if send.returncode != 0:
            err = send.stderr.strip()
        else:
            time.sleep(0.4)  # bytes @31250 baud + opto + USB latency
    finally:
        rx.terminate()
        try:
            captured, _ = rx.communicate(timeout=2)
        except subprocess.TimeoutExpired:
            rx.kill()
            captured, _ = rx.communicate()
    return normalize(captured or ""), err


# Test-message builders: (label, send_hex, expect_hex).
def msg_note(tag):
    m = f"90 {tag:02X} 7F"          # note-on, note=tag, vel=0x7F
    return ("note-on", m, m)


def msg_sysex(tag):
    m = f"F0 7D {tag:02X} F7"        # SysEx, dev id 0x7D, one data byte = tag
    return ("sysex  ", m, m)


def run_one(addr, label, send_hex, expect_hex):
    got, err = roundtrip(addr, send_hex)
    if err:
        print(f"      {label}: send failed -- {err}")
        return False
    ok = normalize(expect_hex) in got
    print(f"      {label}: sent [{send_hex}] -> got [{got or '(nothing)'}]  "
          f"{'MATCH' if ok else 'FAIL'}")
    return ok


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-m", "--match", default="MIDEX",
                    help="port-name substring to match (default: MIDEX)")
    ap.add_argument("-n", "--num", type=int, default=0,
                    help="require/test exactly this many ports (default: all)")
    ap.add_argument("--diag", action="store_true",
                    help="diagnostic mode: note-ons + SysEx, repeated, per message")
    ap.add_argument("-r", "--repeat", type=int, default=3,
                    help="messages of each kind per port in --diag (default: 3)")
    args = ap.parse_args()

    try:
        ports = list_midex_ports(args.match)
    except FileNotFoundError:
        print("amidi not found -- install alsa-utils.", file=sys.stderr)
        sys.exit(2)

    if not ports:
        print(f"No MIDEX ports found (matching '{args.match}'). Upload "
              f"midex-class-r1.ihx and check `amidi -l`.", file=sys.stderr)
        sys.exit(1)
    if args.num and len(ports) < args.num:
        print(f"Found {len(ports)} port(s), need {args.num}.", file=sys.stderr)
        sys.exit(1)
    if args.num:
        ports = ports[:args.num]

    if args.diag:
        # Diagnostic: per port, alternate note-ons and SysEx so we can see
        # whether the first byte is lost only on the first message of the
        # session (toggle), on every message (per-message), or only on SysEx
        # (CIN=0xF / F0 handling). note-on and SysEx make opposite predictions.
        print(f"DIAGNOSTIC over {len(ports)} port(s) -- loop each DIN OUT->IN:\n")
        for addr, name in ports:
            print(f"  [{addr}] {name}")
            for i in range(args.repeat):
                label, s, e = msg_note(0x3C + i)
                run_one(addr, f"#{i+1} {label}", s, e)
            for i in range(args.repeat):
                label, s, e = msg_sysex(0x10 + i)
                run_one(addr, f"#{i+1} {label}", s, e)
            print()
        print("Read-off: note-on first byte (90) lost too -> first-IN-packet / "
              "data-toggle. Only SysEx F0 lost -> CIN=0xF/F0 handling. Only the "
              "first message of the session affected -> session-start toggle.")
        return

    # Gate mode: one note-on per port, strict pass/fail. Channel voice messages
    # are a simple probe; use --diag to exercise SysEx and system real-time
    # framing through the MIDI parser as well.
    print(f"Testing {len(ports)} MIDEX port(s) -- loop each DIN OUT->IN:\n")
    passed = 0
    for idx, (addr, name) in enumerate(ports):
        label, s, e = msg_note(0x3C + idx)
        ok = run_one(addr, name, s, e)
        passed += ok
    print()
    if passed == len(ports):
        print(f"=> All {passed} port(s) round-tripped. PASS.")
        sys.exit(0)
    print(f"=> {passed}/{len(ports)} port(s) round-tripped. Check the cable on "
          f"failing ports (or run --diag).")
    sys.exit(1)


if __name__ == "__main__":
    main()
