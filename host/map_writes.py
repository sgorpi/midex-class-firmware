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

"""Map WHERE external-bus writes land: internal XRAM (control), external SRAM
(CY62256, ~0x8000-0xFFFF), and the UART window (0x40xx).

Pivotal question: the stock firmware demonstrably writes external SRAM (it reads
back 0xFE02 in ep4out_process), so external writes are NOT globally dead. If our
probe can write SRAM but not the UART, the fault is SPECIFIC to the 0x40xx UART
decode (138/PAL), not the bus in general -> a much narrower, likely-fixable target.

Run as root after uploading midex-probe-r1.ihx.
"""
import sys
from typing import Any, cast

import usb.core

VID, PID = 0x0A4E, 0x10C0
VR_READ, VR_WRITE = 0xB0, 0xB1
PATTERNS = (0x00, 0x55, 0xAA, 0xFF, 0x3C)

# (label, addr, expect_writable)
TARGETS = [
    ("internal XRAM (control)", 0x2300, True),
    ("ext SRAM 0x8000",         0x8000, None),
    ("ext SRAM 0xC000",         0xC000, None),
    ("ext SRAM 0xFF40",         0xFF40, None),   # above fw scratch 0xFF00-2F
    ("ext SRAM 0xFE10",         0xFE10, None),
    ("UART p1 RHR/THR 0x4040",  0x4040, None),
    ("UART p4 SCR    0x405F",   0x405F, None),
    ("UART p8 SCR    0x407F",   0x407F, None),
]


def xread(dev, addr):
    return dev.ctrl_transfer(0xC0, VR_READ, wValue=addr, wIndex=0,
                             data_or_wLength=1)[0]


def xwrite(dev, addr, val):
    dev.ctrl_transfer(0x40, VR_WRITE, wValue=addr, wIndex=val & 0xff,
                      data_or_wLength=0)


def rw_test(dev, addr):
    res = [(p, (xwrite(dev, addr, p), xread(dev, addr))[1]) for p in PATTERNS]
    ok = all(w == r for w, r in res)
    return ok, " ".join(f"{w:02X}->{r:02X}" for w, r in res)


def main():
    dev = cast(Any, usb.core.find(idVendor=VID, idProduct=PID))
    if dev is None:
        print("bus-probe not found; upload midex-probe-r1.ihx first.")
        sys.exit(1)
    try:
        if dev.is_kernel_driver_active(0):
            dev.detach_kernel_driver(0)
    except Exception:
        pass

    sram_ok = uart_ok = False
    for label, addr, _ in TARGETS:
        ok, detail = rw_test(dev, addr)
        print(f"  {label:26s} @0x{addr:04X}: {detail}  {'OK' if ok else 'FAIL'}")
        if "SRAM" in label and ok:
            sram_ok = True
        if "UART" in label and ok:
            uart_ok = True

    print()
    if sram_ok and not uart_ok:
        print("=> External SRAM writes LAND, UART writes do NOT. The fault is "
              "SPECIFIC to the 0x40xx UART decode (138/PAL chip-select for writes), "
              "not the external bus in general.")
    elif sram_ok and uart_ok:
        print("=> Both SRAM and UART writes land now (something changed!).")
    elif not sram_ok:
        print("=> External SRAM writes ALSO fail -> the problem is the external "
              "WRITE path in general, not UART-specific. (If SRAM exists at a "
              "different window, adjust the test addresses.)")


if __name__ == "__main__":
    main()
