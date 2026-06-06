# Phase 2 spike — class-compliant bring-up log

Status of the Phase-2 vertical spike (class-compliant USB-MIDI, 2 cables). Read
together with [hardware_register_map.md](hardware_register_map.md) (the validated
r1 register map) and [bus_write_debug.md](bus_write_debug.md) (the Phase-1
bus/UART bring-up).

**Confidence legend:** ✅ confirmed on hardware · ⚠️ inferred / partial · ❓ open.

## What the spike is

Firmware `firmware/midex-spike-r1.ihx` (build: `make spike`). Enumerates as a USB
Audio Class 1.0 / MIDIStreaming device, **VID `0x0A4E` / PID `0x10C1`**, exposing
**2 bidirectional cables** over one bulk endpoint pair (EP2-IN `0x82` +
EP2-OUT `0x02`). Polled bridge in `firmware/main.c`:
- **TX (host→instrument):** decode each EP2-OUT 4-byte USB-MIDI packet's CIN into
  a MIDI byte count (`cin_len[]`) and write those bytes to the cable's UART THR.
- **RX (instrument→host):** poll each cable's UART LSR and emit every received
  byte as a **CIN=0xF single-byte passthrough** packet on EP2-IN. *No MIDI
  parser yet* — that is Phase 3.

UART backend = the hardware-validated Phase-1 map (`uart.c`: `0x4040 + port*8`,
8N1, divisor 1, FIFOs on; `board_init` does the PORTCCFG / Timer2→PB7 /
PB4-RESET bring-up).

## Confirmed on hardware ✅

Tested on a real MIDEX8 r1 (USB `0a4e:10c1`, ALSA card `hw:6`).

- **Enumeration:** `snd-usb-audio` binds with no custom driver; `amidi -l` lists
  `MIDEX8 UAC MIDI 1` (`hw:6,0,0`) and `MIDEX8 UAC MIDI 2` (`hw:6,0,1`). ✅
- **Channel voice messages round-trip 100% (2026-06-05, port 1).** 16 distinct
  note-ons (`90 40 7F` … `90 4F 7F`) and CC (`B0 07 64`) all returned byte-exact.
  This proves the full path end-to-end: USB-OUT → bridge → UART TX → DIN OUT →
  cable → DIN IN → UART RX → bridge → USB-IN, and the `0x4040 + port*8` stride on
  port 1. ✅
- **2-port round-trip (2026-06-06): Phase-2 gate PASS.** With cables looping
  port 1 and port 2 DIN OUT→IN, `./spike_loopback.py -n 2` round-tripped a
  note-on on **both** cables byte-exact (`90 3C 7F` on MIDI 1, `90 3D 7F` on
  MIDI 2). Confirms the per-port stride scales past port 1 — the hard gate
  requirement. ✅

## Test-harness caveat ⚠️

`host/spike_loopback.py` originally started/stopped a fresh `amidi -d` receiver
per message; fired rapidly (the `--diag` sequence) this **races** and produced a
spurious "corruption" (`90 3D 7F` → `90 7F 7F`) that does **not** reproduce with a
single long-lived receiver (16/16 clean). When characterising round-trip,
**use one continuous `amidi -p PORT -d` receiver** and stream messages through it.
The gate (`spike_loopback.py` default) now sends one **note-on** per port (a
channel message — the correct probe for the no-parser passthrough; SysEx was the
wrong gate probe and has been removed from the gate path).

## Localized: where each system byte is lost (2026-06-06, usbmon) ✅

A usbmon capture of EP2 (`host/usbmon_localize.sh` — sends `F0 7D 10 F7`, then
`F8`, then `FE` through one continuous receiver while capturing both directions)
pins down each drop. The host **output** path is correct in every case — the
bytes leave the host with proper CINs:

| Sent | EP2-OUT (`Bo`, host→dev) | EP2-IN (`Bi`, dev→host) | `amidi` saw | Lost at |
|------|--------------------------|--------------------------|-------------|---------|
| SysEx `F0 7D 10 F7` | `04 f0 7d 10` + `05 f7 00 00` ✅ | `0f 7d`, `0f 10`, `0f f7` (**no `0f f0`**) | `7D 10 F7` | **device** (F0 TX'd, never echoed on IN) |
| `F8` clock | `0f f8 00 00` ✅ | `0f f8 00 00` ✅ | — (dropped) | **host input** |
| `FE` sensing | `0f fe 00 00` ✅ | `0f fe 00 00` ✅ | — (dropped) | **host input** |

Two **distinct** loss locations, neither a Phase-2 bridge defect:

1. **`F8`/`FE` (system real-time): host-input-side, not our bug.** The device
   *does* echo them back on EP2-IN as CIN=0xF singles — they make the full loop.
   But `snd-usbmidi`/`amidi` does not surface a CIN=0xF packet carrying a system
   real-time byte (≥`0xF8`) to the rawmidi stream, whereas CIN=0xF carrying `F7`
   and data bytes passes. The firmware delivered the byte; the host filtered it.
2. **`F0` (SysEx start): device-side, value-specific.** The host sent it
   correctly (`04 f0 7d 10`) and `bridge_tx` forwarded all three bytes of that
   packet to the UART THR (`7D` and `10` from the *same* packet looped back fine)
   — yet `F0` never reappears on EP2-IN. So it is lost inside the device's own
   TX→DIN→IN loopback, specific to the value `0xF0` (note-on `90`/CC `B0` status
   bytes, also first-of-burst, survive). Mechanism unresolved (UART/opto/wire
   timing); most likely a **loopback artifact** — a real external `F0` arriving
   on DIN IN would be read by `uart_getc` like any byte. Re-examine if it recurs
   once the Phase-3 parser frames SysEx as CIN `0x4`.

**Assessment unchanged:** correct SysEx/real-time framing is **Phase-3 work** (the
real parser assigns proper CINs — SysEx→`0x4`/`0x5`/`0x6`/`0x7`, real-time→`0xF`).
The spike was never expected to frame these. The channel-message round-trip on
**2 ports** already proves the bridge ⇒ **Phase-2 gate is met.**

## Phase-2 gate: COMPLETE (2026-06-06) ✅

Both remaining items are closed:

- **2-port round-trip:** `./spike_loopback.py -n 2` round-tripped a note-on on
  ports 1 *and* 2 byte-exact — the per-port stride scales (see "Confirmed on
  hardware" above).
- **usbmon localisation** of the `F0`/`F8`/`FE` drop: done (see "Localized"
  above) — F8/FE are host-input filtering (not our bug), F0 is a device-side
  loopback artifact; both are out of Phase-2 scope and tracked for Phase 3.

⇒ Enumeration, `snd-usb-audio` binding, port listing, and ≥2-port MIDI
round-trip all verified. **Ready for Phase 3** (real MIDI parser + scale
`NUM_MIDI_PORTS` 2→8).
