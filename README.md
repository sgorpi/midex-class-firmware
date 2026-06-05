# MIDEX class-compliant firmware

An independent, clean-room effort to build an **alternative USB-MIDI
class-compliant firmware** for the Steinberg MIDEX8 (and family), loaded into the
EZ-USB's RAM at runtime (no flashing, unbrickable). The goal is to make the
hardware usable as a standard USB-MIDI device without the proprietary firmware.

This is a **separate repository** from the Linux kernel driver it accompanies; it
is included there as a git submodule so the two can be maintained — and, if
necessary, taken down — independently.

## Status

- **Phase 1 (hardware bring-up): complete.** A small EZ-USB bus-probe firmware
  validated the reverse-engineered register map on real hardware, culminating in a
  working external-UART **MIDI loopback** (a byte written to a port's THR returns
  on the same port's RHR over a physical MIDI cable). The key unlock was driving
  AN2131 **PB4 low** to de-assert the ST16C454 RESET (active-high); see
  [`doc/bus_write_debug.md`](doc/bus_write_debug.md).
- Next: a class-compliant USB descriptor + enumeration spike, then wiring the
  confirmed UART backend behind it.

## Layout

- [`firmware/`](firmware/) — EZ-USB (8051) firmware. Build with SDCC: `cd firmware && make`.
- [`host/`](host/) — host-side tools (libusb uploader + Python bring-up/test
  scripts). See [`host/README.md`](host/README.md).
- [`doc/`](doc/) — reverse-engineering notes:
  [hardware register map](doc/hardware_register_map.md) and the
  [external-write debug log](doc/bus_write_debug.md).

## Legal

Independent interoperability work based on black-box analysis of the device's own
behaviour and publicly available component datasheets. No Steinberg firmware,
source, or other proprietary material is included or redistributed here.
