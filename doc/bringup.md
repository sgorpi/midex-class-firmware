# MIDEX8 r1 bring-up notes

Bring-up findings for the r1 firmware, kept as reference alongside
[hardware_register_map.md](hardware_register_map.md) (the validated r1 register
map), [bus_write_debug.md](bus_write_debug.md) (the bus/UART bring-up) and
[firmware_build.md](firmware_build.md) (the full 8-port build & validation).

**Confidence legend:** ✅ confirmed on hardware · ⚠️ inferred / partial · ❓ open.

## Enumeration & path validation ✅

Firmware `firmware/midex-class-r1.ihx` (`make class`) enumerates as a USB Audio
Class 1.0 / MIDIStreaming device, **VID `0x0A4E` / PID `0x10C1`**, over one bulk
endpoint pair (EP2-IN `0x82` + EP2-OUT `0x02`). The UART backend uses the
validated r1 map (`uart.c`: `0x4040 + port*8`, 8N1, divisor 1, FIFOs on;
`board_init` does the PORTCCFG / Timer2→PB7 / PB4-RESET bring-up).

On a real MIDEX8 r1:

- **Enumeration:** `snd-usb-audio` binds with no custom driver; `amidi -l` lists
  the `MIDEX8 UAC MIDI N` ports. ✅
- **Full MIDI path:** channel-voice messages round-trip byte-exact end-to-end
  (USB-OUT → bridge → UART TX → DIN OUT → cable → DIN IN → UART RX → bridge →
  USB-IN), and the `0x4040 + port*8` per-port stride scales across all ports. ✅

## Test-harness caveat ⚠️

An `amidi -d` receiver started/stopped per message (as
`host/class_loopback.py --diag` does) can **race** when fired rapidly and produce
a spurious "corruption" (`90 3D 7F` → `90 7F 7F`) that does **not** reproduce
with a single long-lived receiver. When characterising round-trip behaviour,
**use one continuous `amidi -p PORT -d` receiver** and stream messages through it.

## Where system bytes are dropped (usbmon localisation) ✅

A usbmon capture of EP2 (`host/usbmon_localize.sh` — sends `F0 7D 10 F7`, then
`F8`, then `FE` through one continuous receiver while capturing both directions)
pins down two **distinct** drops, neither a firmware bridge defect. The host
**output** path is correct in every case — the bytes leave the host with proper
CINs:

| Sent | EP2-OUT (`Bo`, host→dev) | EP2-IN (`Bi`, dev→host) | `amidi` saw | Lost at |
|------|--------------------------|--------------------------|-------------|---------|
| SysEx `F0 7D 10 F7` | `04 f0 7d 10` + `05 f7 00 00` ✅ | data CINs (**no `F0` echo**) | `7D 10 F7` | **device** (F0 TX'd, never echoed on IN) |
| `F8` clock | `0f f8 00 00` ✅ | `0f f8 00 00` ✅ | — (dropped) | **host input** |
| `FE` sensing | `0f fe 00 00` ✅ | `0f fe 00 00` ✅ | — (dropped) | **host input** |

1. **`F8`/`FE` (system real-time): host-input-side, not a firmware bug.** The
   device *does* echo them back on EP2-IN — they make the full loop. But
   `snd-usbmidi`/`amidi` does not surface a system real-time byte (≥`0xF8`) to the
   rawmidi stream, whereas data bytes pass. The firmware delivered the byte; the
   host filtered it.
2. **`F0` (SysEx start): device-side, value-specific.** The host sent it
   correctly (`04 f0 7d 10`) and the bridge forwarded all three bytes of that
   packet to the UART THR (`7D` and `10` from the *same* packet looped back fine)
   — yet `F0` never reappears on EP2-IN. So it is lost inside the device's own
   TX→DIN→IN loopback, specific to the value `0xF0`. Most likely a **loopback
   artifact** — a real external `F0` arriving on DIN IN is read like any other
   byte.
