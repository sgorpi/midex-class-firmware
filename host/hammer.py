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

"""Trigger the probe's continuous read/write hammer so a multimeter can see the
RD#/WR# strobe (and whether it reaches the UART) as an average-voltage drop.

The hammer loops forever on the device -- this call's USB transfer WILL time
out, which is expected. Power-cycle the MIDEX to stop the hammer.

  sudo python3 hammer.py write 0x407F   # hammer writes to port-8 UART
  sudo python3 hammer.py read  0x407F   # hammer reads  (control: known good)
  sudo python3 hammer.py write 0x2300   # hammer writes to INTERNAL RAM (control)
"""
import sys
from typing import Any, cast

import usb.core

VID, PID = 0x0A4E, 0x10C0
VR_HAMMER_WRITE, VR_HAMMER_READ = 0xB2, 0xB3


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ("read", "write"):
        print(__doc__)
        sys.exit(1)
    req = VR_HAMMER_WRITE if sys.argv[1] == "write" else VR_HAMMER_READ
    addr = int(sys.argv[2], 0)

    dev = cast(Any, usb.core.find(idVendor=VID, idProduct=PID))
    if dev is None:
        print("bus-probe not found; upload midex-probe-r1.ihx first.")
        sys.exit(1)
    try:
        if dev.is_kernel_driver_active(0):
            dev.detach_kernel_driver(0)
    except Exception:
        pass

    print(f"Hammering {sys.argv[1]} @0x{addr:04X}. Measure now; "
          "power-cycle to stop. (USB timeout below is expected.)")
    try:
        dev.ctrl_transfer(0x40, req, wValue=addr, wIndex=0,
                          data_or_wLength=0, timeout=1000)
    except usb.core.USBError:
        print("(timed out as expected -- the device is now hammering.)")


if __name__ == "__main__":
    main()
