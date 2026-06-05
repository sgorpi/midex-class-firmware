#!/usr/bin/env python3
"""Drive the MIDEX8 r1 bus-probe firmware to confirm the static UART map.

Validates src/midex-class-firmware/doc/hardware_register_map.md on real
hardware. Assumes the bus-probe firmware (VID 0x0A4E, PID 0x10C0) is running
(upload it first with midex-fw-upload), and that the target port's MIDI OUT is
looped back to its MIDI IN with a cable.

  read  XDATA byte: bmRequestType 0xC0, bRequest 0xB0, wValue=addr
  write XDATA byte: bmRequestType 0x40, bRequest 0xB1, wValue=addr, wIndex=byte

UART[port] base = 0x4040 + (port-1)*8, standard 16550 register offsets.

Run as root:
  sudo python3 probe_uart.py            # port 8 (default)
  sudo python3 probe_uart.py --port 1
  sudo python3 probe_uart.py --fifo     # also enable the RX/TX FIFOs
"""
import argparse
import sys
import time
from typing import Any, cast

import usb.core

PROBE_VID = 0x0A4E
PROBE_PID = 0x10C0

VR_READ = 0xB0
VR_WRITE = 0xB1

UART_BASE = 0x4040
UART_STRIDE = 0x08
# 16550 register offsets
RHR = 0
THR = 0
DLL = 0
DLM = 1
FCR = 2
LCR = 3
MCR = 4
LSR = 5
# LSR bits
LSR_DR = 0x01
LSR_THRE = 0x20


def port_base(port):  # port is 1-based (matches the front-panel label)
    return UART_BASE + (port - 1) * UART_STRIDE


class Probe:
    def __init__(self, dev):
        self.dev = dev

    def xread(self, addr):
        r = self.dev.ctrl_transfer(0xC0, VR_READ, wValue=addr, wIndex=0,
                                   data_or_wLength=1)
        return r[0]

    def xwrite(self, addr, val):
        self.dev.ctrl_transfer(0x40, VR_WRITE, wValue=addr, wIndex=val & 0xff,
                               data_or_wLength=0)


def uart_init(p, base, fifo):
    p.xwrite(base + LCR, 0x83)   # DLAB=1, 8N1
    p.xwrite(base + DLL, 0x01)   # divisor low  = 1  (-> 31250 baud @ 500kHz)
    p.xwrite(base + DLM, 0x00)   # divisor high = 0
    p.xwrite(base + LCR, 0x03)   # DLAB=0, 8N1
    p.xwrite(base + MCR, 0x00)
    if fifo:
        p.xwrite(base + FCR, 0x07)  # enable + reset RX/TX FIFOs


def verify_divisor(p, base):
    """Read back the divisor latch (DLAB=1) to prove writes landed."""
    p.xwrite(base + LCR, 0x83)          # DLAB=1
    dll = p.xread(base + DLL)
    dlm = p.xread(base + DLM)
    p.xwrite(base + LCR, 0x03)          # DLAB=0
    lcr = p.xread(base + LCR)
    ok = (dll == 0x01 and dlm == 0x00 and lcr == 0x03)
    print(f"Readback: DLL=0x{dll:02X} DLM=0x{dlm:02X} LCR=0x{lcr:02X} "
          f"-> {'OK (writes land, divisor=1)' if ok else 'WRITES NOT LANDING'}")
    return ok


def tx_byte(p, base, b, timeout=0.5):
    deadline = time.time() + timeout
    while not (p.xread(base + LSR) & LSR_THRE):
        if time.time() > deadline:
            raise TimeoutError("THRE never set (transmitter not ready)")
    p.xwrite(base + THR, b)


def rx_drain(p, base, timeout=0.5):
    out = []
    deadline = time.time() + timeout
    while time.time() < deadline:
        if p.xread(base + LSR) & LSR_DR:
            out.append(p.xread(base + RHR))
            deadline = time.time() + 0.05  # extend slightly after each byte
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8, help="1-based port (default 8)")
    ap.add_argument("--fifo", action="store_true", help="enable UART FIFOs")
    args = ap.parse_args()

    dev = cast(Any, usb.core.find(idVendor=PROBE_VID, idProduct=PROBE_PID))
    if dev is None:
        print(f"[ERROR] bus-probe not found ({PROBE_VID:04x}:{PROBE_PID:04x}). "
              "Upload midex-probe-r1.ihx first.")
        sys.exit(1)
    try:
        if dev.is_kernel_driver_active(0):
            dev.detach_kernel_driver(0)
    except (usb.core.USBError, NotImplementedError):
        pass

    p = Probe(dev)
    base = port_base(args.port)
    print(f"Port {args.port}: UART base 0x{base:04X}")

    # Smoke test: scratch register (offset 7) should hold what we write.
    p.xwrite(base + 7, 0x5A)
    scr = p.xread(base + 7)
    print(f"SCR readback: wrote 0x5A, read 0x{scr:02X} "
          f"-> {'OK' if scr == 0x5A else 'MISMATCH (bus/timing?)'}")

    print("Initialising UART (8N1, divisor 1)...")
    uart_init(p, base, args.fifo)
    verify_divisor(p, base)
    print(f"LSR after init: 0x{p.xread(base + LSR):02X} "
          "(expect THRE=0x20 set, i.e. >= 0x20)")

    msg = [0x90, 0x3C, 0x40]  # Note On, note 60, velocity 64
    print(f"Transmitting MIDI {['0x%02X' % b for b in msg]} on port {args.port} OUT...")
    for b in msg:
        tx_byte(p, base, b)

    got = rx_drain(p, base)
    print(f"Received on port {args.port} IN: {['0x%02X' % b for b in got]}")
    if got == msg:
        print("[PASS] Loopback matches -> UART base, offsets, baud and the "
              "opto/MIDI path are all confirmed.")
    elif got:
        print("[PARTIAL] Got bytes but not an exact match -- check the cable, "
              "baud divisor (500 kHz clock assumption) or FIFO setting.")
    else:
        print("[FAIL] Nothing received. TX side may work (check a MIDI monitor "
              "on the OUT jack); RX path / base address / clock needs review.")


if __name__ == "__main__":
    main()
