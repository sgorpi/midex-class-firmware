# MIDEX class-compliant firmware

An independent, clean-room effort to build an **alternative USB-MIDI
class-compliant firmware** for the Steinberg MIDEX8 (and family), loaded into the
EZ-USB's RAM at runtime (no flashing, unbrickable). The goal is to make the
hardware usable as a standard USB-MIDI device without the proprietary firmware.

This is a **separate repository** from the Linux kernel driver it accompanies; it
is included there as a git submodule so the two can be maintained — and, if
necessary, taken down — independently.

## What it does

The firmware enumerates the MIDEX8 as a standard USB Audio Class / MIDIStreaming
device (VID `0x0A4E`, PID `0x10C1`), so the OS's generic USB-MIDI driver
(`snd-usb-audio` on Linux) binds with no custom driver and `amidi -l` lists the
ports. All 8 ports round-trip MIDI byte-exact, and the firmware-port→front-panel
jack mapping is identity. The proprietary timestamp engine and host-timing tick
are not implemented, as class-compliant USB-MIDI carries no per-event timestamps.

A per-port USB-MIDI 1.0 parser ([`firmware/midi_parser.c`](firmware/midi_parser.c))
and a non-blocking TX/RX ring bridge connect the single USB bulk endpoint pair to
the UARTs. The FIFO-less ST16C454 has only a 1-byte RHR, so a high-priority
Timer0 RX-capture ISR drains each port into a per-port software FIFO, keeping
sustained streams (long SysEx) overrun-proof; see
[`doc/timer_isr_rx_capture_design.md`](doc/timer_isr_rx_capture_design.md).
[`firmware/midi_config.h`](firmware/midi_config.h) `NUM_MIDI_PORTS` is the single
knob that scales the descriptors and bridge.

Both MIDEX8 hardware revisions are supported:

- **r1** (EZ-USB AN2131): all 8 ports are external 16550 channels (two ST16C454).
  Bring-up de-asserts the ST16C454 RESET by driving AN2131 **PB4 low**; see
  [`doc/bus_write_debug.md`](doc/bus_write_debug.md).
- **r2** (EZ-USB FX CY7C646): a **hybrid backend** — 6 external 16550 channels
  (ST16C454 + ST16C452, divisor 24 off a fixed 12 MHz crystal) drive ports 1–6,
  and the FX's **two on-chip UARTs** drive ports 7–8. A board seam
  ([`board.h`](firmware/board.h), `make BOARD=r2`) selects
  [`board_r2.h`](firmware/board_r2.h) and a split UART backend
  ([`uart_ext.c`](firmware/uart_ext.c) + [`uart_onchip.c`](firmware/uart_onchip.c))
  behind the shared op-set; the on-chip ports are polled, and the single
  high-priority Timer0 RX-capture ISR services both backends. Full RE comparison:
  [`doc/midex8_r1_vs_r2.md`](doc/midex8_r1_vs_r2.md).

`make class` builds the r1 image, `make BOARD=r2 class` the r2 image, and
`make both` produces both. A small EZ-USB bus-probe firmware is retained as a
diagnostic (`make probe`). The host installer auto-uploads the image matching the
appearing device's loader PID (`0x1000`→r1, `0x1010`→r2), so plugging in either
revision just works; see [`host/README.md`](host/README.md).

## Performance

Measured on a physical MIDEX8 **r1** running `firmware/midex-class-r1.ihx`, via
the ALSA-rawmidi harness ([`host/e2e_test.py`](host/e2e_test.py)) with MIDI
loopback cables (DIN OUT→IN) on the tested ports. The UART line rate is the hard
limit: 31250 baud ÷ 10 bits ÷ 3 bytes ≈ **~1040 three-byte messages/s per port**.

### MIDEX8 r1 (8× external 16550)

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

### MIDEX8 r2 (hybrid: external 16550 + on-chip FX UARTs)

Measured on a physical MIDEX8 **r2** running `firmware/midex-class-r2.ihx`, same
harness, with loopback cables on an external port (1) and both on-chip ports
(7, 8). The on-chip ports are the FX's two internal UARTs — a single-byte `SBUF`
captured on the shared ~279 µs Timer0 sweep — while the external ports are the
ST16C45x as on r1 but at divisor 24.

**Round-trip latency** (ms), external vs on-chip backend:

| test | port (backend) | min | mean | median | p95 | p99 | max | sd |
|------|----------------|-----|------|--------|-----|-----|-----|----|
| `timing` (2 000) | 1 (external) | 1.64 | 1.94 | 1.94 | 1.95 | 2.12 | 2.66 | 0.05 |
| `timing` (2 000) | 7 (on-chip)  | 1.68 | 2.04 | 1.94 | 2.52 | 2.56 | 7.33 | 0.24 |
| `soak` (20 000)  | 1 (external) | 1.40 | 2.30 | 2.00 | 4.00 | 4.01 | 4.28 | 0.81 |
| `soak` (20 000)  | 7 (on-chip)  | 1.38 | 2.42 | 2.00 | 4.00 | 4.01 | 4.34 | 0.82 |

Both backends reported **0 drops / 0 corrupt** over 20 000 round-trips. The
on-chip ports show slightly higher tail jitter (occasional 7–9 ms vs the external
path's ~2.7 ms max) — the cost of polling the single-byte `SBUF` on the capture
sweep — but no drops.

- **`functional`** (14 cases) and **`sysex`** (including a 200-byte
  multi-USB-packet message) pass on both an external and an on-chip port.
- **`jitter`** (on-chip port, 1 000 msgs @ 10 ms interval): inter-arrival mean
  10.00 ms; absolute jitter vs target p99 1.02 / max 1.20 ms; 1000/1000 received.
- **`throughput`** (5 s full-duplex): ~1150 msg/s external, ~960 msg/s on-chip,
  both near the ~1040 msg/s/port UART ceiling (loss above it is largely host-side
  input buffering, as on r1).
- **`isolation`**: sending on any port produces no echo on the others — no
  crosstalk between the external and on-chip backends.
- **port→jack mapping**: identity (ALSA port *N* = front-panel jack *N*),
  confirmed with a cross-cable scan ([`host/map_scan.py`](host/map_scan.py)).

### vs. the stock firmware

The same harness drives the **stock** Steinberg firmware (PID `0x1001`, custom
`snd-usb-midex` kernel driver) — pass `-m "MIDEX Port"`, the name its ports
enumerate under. Both firmwares were measured on the **same** r1 unit with the
same loopback cables on ports 1–3, so the numbers are directly comparable.

**Round-trip latency** (ms), stock vs class-compliant:

| test | fw | min | mean | median | p95 | p99 | max | sd |
|------|----|-----|------|--------|-----|-----|-----|----|
| `timing` (1 000)  | stock | 2.40 | 2.91 | 2.92 | 2.96 | 3.10 | 20.16 | 0.56 |
| `timing` (1 000)  | class | 2.07 | 2.57 | 2.51 | 2.97 | 3.27 | **3.79** | 0.27 |
| `soak` (50 000)   | stock | 2.42 | 3.01 | 2.99 | 3.03 | 3.95 | 23.42 | 0.14 |
| `soak` (50 000)   | class | 1.91 | 3.09 | 2.99 | 4.00 | 4.42 | **5.45** | 0.72 |

Central tendency is close (medians within ~0.4 ms), but the stock firmware shows
rare **~20 ms latency outliers** (its `max`), consistent with the proprietary
firmware's ~25.6 ms host-timing tick occasionally gating a delivery; the
class-compliant firmware bounds its worst case under 6 ms. Both reported
**0 drops / 0 corrupt** over 50 000 round-trips.

**Throughput**, single port full-duplex for 5 s, % loss:

| target rate | stock | class |
|-------------|-------|-------|
| 500 msg/s   | 0 %   | 0 %   |
| 800 msg/s   | 0 %   | 8.5 % |
| 1000 msg/s  | 0 %   | 24.3 % |

The stock `snd-usb-midex` path holds 0 % loss right up to the ~1040 msg/s/port
UART ceiling, where the class firmware begins shedding above ~800 msg/s. Most of
that gap is **host-side input buffering**, not device drops (the stock driver's
interrupt-endpoint URB pool absorbs bursts the class path overflows), so it
reflects driver/buffering depth rather than a raw device-capacity difference —
both share the same UART line-rate limit.

**Jitter** (stock, 1 000 msgs at a 10 ms interval): inter-arrival mean 10.00 ms,
sd 0.58; absolute jitter vs target mean 0.33 / p99 1.25 / max 2.31 ms; 1000/1000
received.

**Sustained SysEx** (`sysexsoak --ports 1,2,3`, 500 × 259-byte): stock
**0 drops / 0 corrupt** (PASS), same as the class firmware. The stock firmware's
RX-overflow counter is a class-firmware-only vendor request, so that telemetry
reads "unavailable" on stock.

**Correctness** (`functional`, 14 cases): the class firmware passes all 14; the
stock firmware passes **13/14**, reproducibly dropping the **Song Position
Pointer** `[F2 12 34]` (the only 2-data-byte system-common message — MTC `F1`,
song-select `F3` and tune-request `F6` all echo fine). The stock driver also
passes MIDI **real-time bytes** (`0xF8`/`0xFE`) up unfiltered, whereas the class
path (`snd-usbmidi`) filters them on input.

## Layout

- [`firmware/`](firmware/) — EZ-USB (8051) firmware. Build with SDCC: `cd firmware && make`.
- [`host/`](host/) — host-side tools (libusb uploader + Python bring-up/test
  scripts). See [`host/README.md`](host/README.md).
- [`doc/`](doc/) — reverse-engineering notes:
  [hardware register map](doc/hardware_register_map.md), the
  [external-write debug log](doc/bus_write_debug.md), and the
  [r1 vs r2 firmware comparison](doc/midex8_r1_vs_r2.md) (the hybrid-backend
  derivation behind `board_r2.h`).

## License

Released under the **GNU General Public License v2.0 or later** — see
[`LICENSE`](LICENSE). The USB enumeration scaffolding under `firmware/`
(`usb.c`, `usb.h`, `common.h`, `delay.*`, `io.h`, `reg_ezusb.h`, `USBJmpTb.a51`,
`usb_probe.c`) is vendored from the [OpenULINK](https://github.com/OpenULINK)
fork and retains its original copyright notices; it is GPLv2-compatible.

## Legal

Independent interoperability work based on black-box analysis of the device's own
behaviour and publicly available component datasheets. No Steinberg firmware,
source, or other proprietary material is included or redistributed here.
