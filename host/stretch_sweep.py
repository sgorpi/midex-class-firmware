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

"""Sweep the AN2131 MOVX cycle-stretch (CKCON bits 2:0 = 0..7) and, for each
value, re-run the external-bus write test on the port-8 UART scratch register.

Why: the DMM proved the write strobe (-IOW), the chip-select, and the write data
all reach the UART, yet writes never latch -> a -IOW-edge timing problem. Widening
the write strobe via the MOVX stretch is the AN2131's only bus-timing knob. If any
stretch value makes 0x407F read back what we wrote, we have a working config (and a
strong hint the board needs a longer/later write strobe than stock's stretch=0).

Requires the updated midex-probe-r1.ihx (adds VR_SET_CKCON=0xB4). Run as root.
"""
import sys
from typing import Any, cast

import usb.core

VID, PID = 0x0A4E, 0x10C0
VR_READ, VR_WRITE, VR_SET_CKCON = 0xB0, 0xB1, 0xB4
UART_SCR = 0x407F          # port-8 scratch register (R/W, no side effects)
XRAM_CTRL = 0x2300         # internal XRAM control (must always pass)
PATTERNS = (0x00, 0x55, 0xAA, 0xFF, 0x3C)


def xread(dev, addr):
    return dev.ctrl_transfer(0xC0, VR_READ, wValue=addr, wIndex=0,
                             data_or_wLength=1)[0]


def xwrite(dev, addr, val):
    dev.ctrl_transfer(0x40, VR_WRITE, wValue=addr, wIndex=val & 0xff,
                      data_or_wLength=0)


def set_ckcon(dev, val):
    dev.ctrl_transfer(0x40, VR_SET_CKCON, wValue=val & 0xff, wIndex=0,
                      data_or_wLength=0)


def rw_test(dev, addr):
    results = [(p, (xwrite(dev, addr, p), xread(dev, addr))[1]) for p in PATTERNS]
    ok = all(w == r for w, r in results)
    detail = " ".join(f"{w:02X}->{r:02X}" for w, r in results)
    return ok, detail


def main():
    dev = cast(Any, usb.core.find(idVendor=VID, idProduct=PID))
    if dev is None:
        print("bus-probe not found; upload the updated midex-probe-r1.ihx first.")
        sys.exit(1)
    try:
        if dev.is_kernel_driver_active(0):
            dev.detach_kernel_driver(0)
    except Exception:
        pass

    # Sanity: internal write path must work regardless of stretch.
    ok, detail = rw_test(dev, XRAM_CTRL)
    print(f"control  XRAM   @0x{XRAM_CTRL:04X}: {detail}  {'OK' if ok else 'FAIL'}")
    print()
    print("stretch  UART SCR @0x%04X (write-then-readback)" % UART_SCR)
    any_pass = False
    for s in range(8):
        set_ckcon(dev, s)
        ok, detail = rw_test(dev, UART_SCR)
        print(f"  MD={s} (CKCON={s:#04x}): {detail}  {'<== WRITES LAND!' if ok else 'fail'}")
        any_pass = any_pass or ok
    print()
    if any_pass:
        print("=> At least one stretch value makes external writes latch. "
              "The board needs a wider/later write strobe than stock used.")
    else:
        print("=> No stretch value helps. Strobe WIDTH is not the issue; the "
              "fault is structural timing (CS/-IOW/data overlap or the 74HC123 "
              "one-shot) -> needs a scope/logic analyzer to resolve.")


if __name__ == "__main__":
    main()
