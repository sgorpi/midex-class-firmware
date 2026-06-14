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

"""Port->jack mapping scan for the class-compliant MIDEX8.

With a CROSS MIDI cable (one jack's DIN OUT -> a DIFFERENT jack's DIN IN), this
sends a unique marker on every ALSA OUT port in turn and watches every ALSA IN
port. The single (out_port, in_port) pair that lights up tells you which ALSA
port drives the source jack and which ALSA port reads the destination jack --
i.e. the real firmware-index -> panel-jack mapping (which self-loop tests can't
reveal, since a self-loop passes regardless of order).

Reuses e2e_test's libasound RawPort wrapper (ALSA hw:C,0,S addressing) and
amidi-based port discovery -- no sudo, no pip deps.

Usage:
  ./map_scan.py                  # auto-find "MIDEX8 UAC" ports
  ./map_scan.py -m "MIDEX8 UAC"

Set up ONE cross cable, e.g. panel jack 7 OUT -> jack 8 IN, then run.
"""
import argparse
import sys
import time

import e2e_test as e2e

MARKER_BASE = 0x3C  # note number; OUT port p (1-based) sends note MARKER_BASE+p-1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-m", "--match", default="MIDEX8 UAC")
    args = ap.parse_args()

    pm = e2e.find_ports(args.match)
    if not pm:
        sys.exit(f"no ports matching '{args.match}' (amidi -l)")
    ports = sorted(pm)
    print(f"found {len(ports)} ports: {[pm[p] for p in ports]}")

    rp = {p: e2e.RawPort(pm[p]) for p in ports}

    print("\nALSA OUT port -> ALSA IN port that received its marker:")
    hits = []
    for src in ports:
        for p in ports:
            rp[p].drain_input(0.02)
        note = MARKER_BASE + (src - 1)
        rp[src].write(bytes([0x90, note, 0x7F]))
        time.sleep(0.08)
        for dst in ports:
            data = rp[dst].read_ready()
            if note in data:
                hits.append((src, dst))
                print(f"  OUT port {src}  ->  IN port {dst}"
                      f"   (note 0x{note:02X})")

    if not hits:
        print("  (nothing received -- check the cross cable is OUT->IN between "
              "two DIFFERENT jacks)")
    print("\nThe source jack (cable OUT end) is driven by the OUT port shown;")
    print("the destination jack (cable IN end) is read by the IN port shown.")
    for p in ports:
        rp[p].close()


if __name__ == "__main__":
    main()
