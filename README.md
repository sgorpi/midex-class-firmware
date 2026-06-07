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
- **Phase 2 (class-compliant spike): complete, hardware-validated.** The
  firmware enumerates as a standard USB Audio Class / MIDIStreaming device
  (VID `0x0A4E`, PID `0x10C1`); `snd-usb-audio` binds with no custom driver and
  `amidi -l` lists the ports.
- **Phase 3 (full r1: all 8 ports + real RX MIDI parser): complete,
  hardware-validated.** A per-port USB-MIDI 1.0 parser (`midi_parser.c`) and a
  non-blocking TX/RX ring bridge feed the eight external 16550 UARTs; all 8 ports
  round-trip MIDI byte-exact. The earlier **sustained-SysEx RX overrun** (the
  FIFO-less ST16C454 has a 1-byte RHR) is **resolved** by a high-priority Timer0
  RX-capture ISR that drains the RHR into per-port software FIFOs; see
  [`doc/timer_isr_rx_capture_design.md`](doc/timer_isr_rx_capture_design.md).
  [`firmware/midi_config.h`](firmware/midi_config.h) `NUM_MIDI_PORTS` is the
  single knob that scales the descriptors + bridge.

The bus-probe is retained as a diagnostic (`make probe`); the spike is the
default build (`make spike`).

## Performance

Measured on a physical MIDEX8 **r1** running `firmware/midex-spike-r1.ihx`, via
the ALSA-rawmidi harness ([`host/e2e_test.py`](host/e2e_test.py)) with MIDI
loopback cables (DIN OUT→IN) on the tested ports. The UART line rate is the hard
limit: 31250 baud ÷ 10 bits ÷ 3 bytes ≈ **~1040 three-byte messages/s per port**.

**Round-trip latency** (host→device→DIN→loopback→device→host), single port:

| test | min | mean | median | p95 | p99 | max | sd |
|------|-----|------|--------|-----|-----|-----|----|
| `timing` (1 000 msgs) | 2.07 | 2.57 | 2.51 | 2.97 | 3.27 | 3.79 | 0.27 |
| `soak` (50 000 msgs)  | 1.91 | 3.09 | 2.99 | 4.00 | 4.42 | 5.45 | 0.72 |

(milliseconds; `soak` reported **0 drops / 0 corrupt** over 50 000 round-trips.)

**Throughput**, single port driven full-duplex (self-loopback) for 5 s:

| target rate | delivered | loss |
|-------------|-----------|------|
| 500 msg/s   | 500 msg/s | **0 %** |
| 800 msg/s   | 800 msg/s | 8.5 % |
| ~1000 msg/s | ~966 msg/s | 24.3 % |

Loss stays at zero up to roughly half the line rate and rises only as the
~1040 msg/s/port UART ceiling is approached (above the ceiling some reported
"loss" is host-side input buffering, not device drops).

**Sustained SysEx** (the RX-overrun regression test), three ports driven at
once, 500 × 259-byte SysEx each (`e2e_test.py sysexsoak --ports 1,2,3`):
**0 drops / 0 corrupt**.

> The RX-capture ISR runs a software /3 prescaler (~279 µs effective sweep) so
> servicing the eight UARTs costs little CPU; without it a flat 100 µs tick
> roughly tripled latency and halved throughput. Details:
> [`doc/timer_isr_rx_capture_design.md`](doc/timer_isr_rx_capture_design.md).

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
