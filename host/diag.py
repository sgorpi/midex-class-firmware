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

"""Bus-probe diagnostic: isolate where writes fail.

Reads work but writes don't land. This separates the boundaries:
  A) firmware board_init actually ran   (read PORTxCFG back)
  B) the vendor WRITE command + MOVX work at all (write internal XRAM, read back)
  C) external-bus writes (WR# strobe) work (write a UART scratch reg, read back)

If B passes but C fails -> external WR# strobe / write timing.
If B fails           -> the vendor-write command path is broken.

Run as root after uploading midex-probe-r1.ihx.
"""
import sys
from typing import Any, cast

import usb.core

VID, PID = 0x0A4E, 0x10C0
VR_READ, VR_WRITE = 0xB0, 0xB1


def xread(dev, addr):
    return dev.ctrl_transfer(0xC0, VR_READ, wValue=addr, wIndex=0,
                             data_or_wLength=1)[0]


def xwrite(dev, addr, val):
    dev.ctrl_transfer(0x40, VR_WRITE, wValue=addr, wIndex=val & 0xff,
                      data_or_wLength=0)


def rw_test(dev, addr, label):
    results = []
    for pat in (0x00, 0x55, 0xAA, 0xFF, 0x3C):
        xwrite(dev, addr, pat)
        got = xread(dev, addr)
        results.append((pat, got))
    ok = all(w == r for w, r in results)
    detail = " ".join(f"{w:02X}->{r:02X}" for w, r in results)
    print(f"  {label} @0x{addr:04X}: {detail}  {'OK' if ok else 'FAIL'}")
    return ok


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

    print("A) board_init register state (internal EZ-USB regs):")
    for name, addr in (("PORTACFG", 0x7F93), ("PORTBCFG", 0x7F94),
                       ("PORTCCFG", 0x7F95), ("OEA", 0x7F9C),
                       ("OEB", 0x7F9D), ("OEC", 0x7F9E), ("CPUCS", 0x7F92)):
        print(f"   {name:8s} 0x{addr:04X} = 0x{xread(dev, addr):02X}")
    print("   (expect PORTCCFG bits6,7=0xC0; PORTBCFG bit7=0x80)")

    print("B) vendor WRITE path - internal XRAM (no WR# pin):")
    b = rw_test(dev, 0x2300, "XRAM")

    print("C) external bus WRITE - UART port 8 scratch reg (uses WR#):")
    c = rw_test(dev, 0x407F, "SCR ")

    print()
    if b and c:
        print("=> writes land everywhere; the earlier failure is elsewhere.")
    elif b and not c:
        print("=> vendor write works, EXTERNAL writes fail -> WR# strobe / "
              "write timing on the expansion bus.")
    elif not b:
        print("=> even internal vendor writes fail -> the VR_WRITE firmware "
              "path is broken (not a bus issue).")


if __name__ == "__main__":
    main()
