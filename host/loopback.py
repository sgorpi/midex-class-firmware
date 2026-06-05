#!/usr/bin/env python3
"""External MIDI loopback test for the MIDEX8 r1 bus-probe.

Each MIDI port is one ST16C454 channel: TX drives MIDI OUT, RX reads MIDI IN.
With a physical MIDI cable from "port N OUT" to "port N IN", a byte written to
that channel's THR should reappear on its RHR. This is the end-to-end proof
that external UART writes work (not just register read/write-back).

Because the channel<->physical-port address mapping is not yet confirmed, this
sends on the port you name but SCANS all 8 channels' RHR for the returned byte,
so a mapping mismatch still detects the loopback and reveals the true mapping.

  sudo python3 loopback.py 8     # send on port 8, scan all RX
  sudo python3 loopback.py 1     # send on port 1, scan all RX

Run as root after uploading midex-probe-r1.ihx (PID 0x10C0).
"""
import sys
import time
from typing import Any, cast

import usb.core

VID, PID = 0x0A4E, 0x10C0
VR_READ, VR_WRITE = 0xB0, 0xB1

UART_BASE, UART_STRIDE = 0x4040, 0x08

# Register offsets (added to a channel base)
RHR = THR = DLL = 0
DLM = IER = 1
FCR = 2
LCR = 3
MCR = 4
LSR = 5
SCR = 7

LSR_DR = 0x01     # receive data ready
LSR_THRE = 0x20   # transmit holding register empty

TEST_BYTES = (0x55, 0xAA, 0x3C, 0x7E, 0x01)


def base(port):          # 1-based MIDI port -> channel base address
    return UART_BASE + (port - 1) * UART_STRIDE


def xread(dev, addr):
    return dev.ctrl_transfer(0xC0, VR_READ, wValue=addr, wIndex=0,
                             data_or_wLength=1)[0]


def xwrite(dev, addr, val):
    dev.ctrl_transfer(0x40, VR_WRITE, wValue=addr, wIndex=val & 0xff,
                      data_or_wLength=0)


def uart_init(dev, port):
    """8N1, divisor 1 (500 kHz XIN / 16 = 31250 baud = MIDI). No internal
    loopback (MCR=0), interrupts off (IER=0), FIFOs off (16450 mode)."""
    b = base(port)
    xwrite(dev, b + LCR, 0x80)      # DLAB=1 to reach divisor latches
    xwrite(dev, b + DLL, 0x01)
    xwrite(dev, b + DLM, 0x00)
    xwrite(dev, b + LCR, 0x03)      # DLAB=0, 8 data bits, no parity, 1 stop
    xwrite(dev, b + IER, 0x00)
    xwrite(dev, b + FCR, 0x00)
    xwrite(dev, b + MCR, 0x00)      # bit4=0 -> NOT internal loopback


def drain_rx(dev, port):
    b = base(port)
    for _ in range(16):
        if not (xread(dev, b + LSR) & LSR_DR):
            break
        xread(dev, b + RHR)


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8
    if not 1 <= port <= 8:
        print("port must be 1..8")
        sys.exit(1)

    dev = cast(Any, usb.core.find(idVendor=VID, idProduct=PID))
    if dev is None:
        print("bus-probe not found; upload midex-probe-r1.ihx first.")
        sys.exit(1)
    try:
        if dev.is_kernel_driver_active(0):
            dev.detach_kernel_driver(0)
    except Exception:
        pass

    print(f"Init all 8 channels (8N1/31250)...")
    for p in range(1, 9):
        uart_init(dev, p)
        drain_rx(dev, p)

    # Confirm the line-control register actually stuck (a real RW register,
    # unlike THR/RHR). If this reads back 0x03 the UART config writes landed.
    b = base(port)
    lcr_rb = xread(dev, b + LCR)
    print(f"port {port} @0x{b:04X}: LCR readback = 0x{lcr_rb:02X} "
          f"({'OK' if lcr_rb == 0x03 else 'unexpected'})")

    print(f"\nSending on port {port} (0x{b:04X}), scanning all RX channels...\n")
    any_hit = False
    for tb in TEST_BYTES:
        # ensure transmitter empty, then send
        for _ in range(100):
            if xread(dev, b + LSR) & LSR_THRE:
                break
        xwrite(dev, b + THR, tb)

        # one MIDI byte = 320 us; allow for opto + USB latency
        time.sleep(0.01)

        hits = []
        for p in range(1, 9):
            rb = base(p)
            if xread(dev, rb + LSR) & LSR_DR:
                got = xread(dev, rb + RHR)
                hits.append((p, got))
        if hits:
            any_hit = True
        desc = ", ".join(f"port{p}=0x{g:02X}" for p, g in hits) or "(nothing)"
        match = any(g == tb for _, g in hits)
        print(f"  TX 0x{tb:02X} -> RX {desc}  {'MATCH' if match else ''}")

    print()
    if any_hit:
        print("=> Loopback RX detected -- external UART TX works end-to-end.")
    else:
        print("=> No byte received on any channel. If the cable is on a "
              "different physical port, try the other port number; otherwise "
              "TX path or port mapping needs another look.")


if __name__ == "__main__":
    main()
