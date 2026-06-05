#!/usr/bin/env python3
"""Find what enables external-bus WRITES, by trying candidate register pokes
from the host (the probe can write any internal EZ-USB register) and re-testing
an external UART write after each.

Stock firmware's MIDI-out works on this unit, so the hardware write path is
good -- some EZ-USB register state differs. This sweeps the candidates.

Run as root after uploading midex-probe-r1.ihx.
"""
import sys
from typing import Any, cast

import usb.core

VID, PID = 0x0A4E, 0x10C0
VR_READ, VR_WRITE = 0xB0, 0xB1
SCR = 0x407F          # port-8 UART scratch register (external, read/write)


def xr(dev, a):
    return dev.ctrl_transfer(0xC0, VR_READ, wValue=a, wIndex=0,
                             data_or_wLength=1)[0]


def xw(dev, a, v):
    dev.ctrl_transfer(0x40, VR_WRITE, wValue=a, wIndex=v & 0xff,
                      data_or_wLength=0)


def ext_write_works(dev):
    """Write two patterns to the external SCR; return True if they read back."""
    ok = True
    for pat in (0x55, 0xAA):
        xw(dev, SCR, pat)
        if xr(dev, SCR) != pat:
            ok = False
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

    print("Full port/control register dump:")
    regs = {
        "CPUCS": 0x7F92, "PORTACFG": 0x7F93, "PORTBCFG": 0x7F94,
        "PORTCCFG": 0x7F95, "OUTA": 0x7F96, "OUTB": 0x7F97, "OUTC": 0x7F98,
        "PINSA": 0x7F99, "PINSB": 0x7F9A, "PINSC": 0x7F9B,
        "OEA": 0x7F9C, "OEB": 0x7F9D, "OEC": 0x7F9E, "ISOCTL": 0x7FA1,
    }
    for name, addr in regs.items():
        print(f"   {name:9s} 0x{addr:04X} = 0x{xr(dev, addr):02X}")

    print(f"\nBaseline external write: {'WORKS' if ext_write_works(dev) else 'FAILS'}")

    # Ordered candidate pokes. Each is (label, fn) applied then retested.
    def poke(addr, val):
        return lambda: xw(dev, addr, val)

    def rmw(addr, orbits):
        def f():
            xw(dev, addr, xr(dev, addr) | orbits)
        return f

    candidates = [
        ("CPUCS=0 (clear CLK24OE)",         poke(0x7F92, 0x00)),
        ("PORTACFG |= 0x0C (CS#/OE#)",       rmw(0x7F93, 0x0C)),
        ("PORTACFG |= 0x3C (CS/OE/FWR/FRD)", rmw(0x7F93, 0x3C)),
        ("OEA = 0xFF",                       poke(0x7F9C, 0xFF)),
        ("OEC = 0xFF",                       poke(0x7F9E, 0xFF)),
        ("glue 0xFE00 = 0xC9",               poke(0xFE00, 0xC9)),
        ("glue 0xFE00 = 0xFF",               poke(0xFE00, 0xFF)),
        ("glue 0xFE00 = 0x00",               poke(0xFE00, 0x00)),
    ]

    print("\nSweeping candidate enablers (cumulative):")
    for label, fn in candidates:
        try:
            fn()
        except usb.core.USBError as e:
            print(f"   after [{label}]: poke errored ({e})")
            continue
        works = ext_write_works(dev)
        print(f"   after [{label:32s}]: external write {'WORKS *** ' if works else 'still fails'}")
        if works:
            print(f"\n=> '{label}' enabled external writes. Bake this into board_init.")
            return

    print("\n=> none of the pokes enabled external writes. "
          "Next: multimeter the 74HC245 DIR/OE pins vs stock.")


if __name__ == "__main__":
    main()
