# Phase 3 — full r1 build (8 ports + real MIDI parser)

Builds on the [Phase 2 spike](spike_bringup.md). Same firmware image name
(`firmware/midex-spike-r1.ihx`, `make spike`), same PID `0x0A4E:0x10C1`, same
single EP2-IN/EP2-OUT bulk pair — but now **8 bidirectional cables** and a
**real USB-MIDI 1.0 RX stream parser** in place of the spike's CIN=0xF
single-byte passthrough.

**Confidence legend:** ✅ confirmed on hardware · ⚠️ inferred / built-not-tested · ❓ open.

## What changed from the spike

1. **`NUM_MIDI_PORTS` 2 → 8** (`firmware/midi_config.h`). The TX bridge and the
   RX pump already loop `0..NUM_MIDI_PORTS-1`, so the per-port `0x4040 + port*8`
   UART stride scales unchanged.
2. **Descriptors scaled to 8 ports** (`firmware/usb_descriptors.c`). The jack
   list is hand-unrolled via a `MIDI_PORT_JACKS(p)` macro (ids `4p+1..4p+4`,
   p = 0..7 → jack ids 1..32). Length constants (`CONFIG_TOTAL_LEN`,
   `MS_TOTAL_LEN`) now derive from `NUM_MIDI_PORTS`; the `config_block_len_check`
   static assert verifies the packed array matches. The two class-specific
   endpoint association arrays map cable p → Embedded-IN jack `4p+1`
   (`1,5,…,29`) on EP2-OUT and cable p → Embedded-OUT jack `4p+3` (`3,7,…,31`)
   on EP2-IN. Final `wTotalLength` = **325 bytes** (the EZ-USB SUDPTR auto-length
   engine streams it across EP0 in 64-byte chunks, exactly as the 133-byte spike
   descriptor was streamed).
3. **Real RX parser** (`firmware/midi_parser.{c,h}`, called from `main.c`'s loop
   as `midi_rx_pump()`). Per-cable state in XDATA reassembles each UART byte
   stream into self-contained 4-byte USB-MIDI event packets:
   - **Channel voice** (`0x80–0xEF`): CIN = status high nibble; note off/on,
     poly-AT, CC, pitch bend = 2 data bytes; program change, channel-AT = 1.
   - **Running status**: a data byte with no fresh status reuses the last
     channel-voice status; the parser **expands** it back into a full
     status-carrying packet (USB-MIDI packets are always self-contained).
   - **System common**: MTC quarter frame (F1) / song select (F3) → CIN 0x2;
     song position (F2) → CIN 0x3; tune request (F6) + undefined F4/F5 → CIN 0x5
     single. These cancel running status.
   - **SysEx**: F0…F7 framed as CIN 0x4 (3-byte start/continue) chunks plus a
     CIN 0x5/0x6/0x7 end packet carrying the trailing 1/2/3 bytes incl. F7.
   - **System real-time** (`0xF8–0xFF`): each emitted as a CIN 0xF single;
     interleaves anywhere without disturbing running status or an open SysEx.

   **Key fix during bring-up:** every completed-message packet places the MIDI
   **status byte in MIDI_0** (`emit(cn, cin, status, d0, d1)`). The spike's
   CIN=0xF passthrough emitted each raw byte separately, so it never had to carry
   an explicit status byte; a proper CIN-framed packet must.

## Build status ✅ (hardware-validated, all 8 ports)

`make spike` is clean with SDCC 4.2.0:

- **Code** ~3.6 KB / 6912 bytes (well below the `0x1B00` USB-jump-table ceiling).
- **XDATA** 80 bytes at `0x2000` = the 8 × 10-byte per-port parser structs (of
  2 KB free).
- The assembled `config_block` was parsed back out of the `.ihx` and validated:
  `wTotalLength` 325 walks exactly to length; 16 MIDI-IN + 16 MIDI-OUT jacks
  (8 embedded + 8 external each direction); endpoint assoc arrays
  `1,5,…,29` and `3,7,…,31`.
- **On hardware (2026-06-06):** every port round-trips MIDI OUT→IN
  (`spike_loopback.py`, cables moved across all 8 ports), and an on-device LCR
  readback shows all 8 channels at `0x03` (8N1), latching on the first init pass.

## Bug found + fixed during bring-up: deferred UART init ✅

The first 8-port build round-tripped most ports but **exactly one channel came
up dead** (originally port 2). Root-caused over a long debug session:

- The channel's **LCR was stuck at `0x00`** (the 5-bit reset default) instead of
  `0x03` (8N1) — confirmed by an on-device register readback. 5-bit mode
  truncates every byte to its low 5 bits (`0x90`→`0x10`), so the bridge's parser
  correctly dropped the garbage and emitted nothing. The parser was never at
  fault.
- The Phase-1 **bus-probe round-tripped that same channel perfectly** (including
  with FIFOs on), so the UART hardware/socket/opto were fine. The fault was the
  class firmware's **`uart_init`**.
- The victim was **deterministic per build but moved between builds** (port 2 →
  port 3 → port 1 as the code/timing changed), and **read-back retry could not
  fix it** (re-running the same write sequence re-hit the same bad window). That
  signature = a **marginal analog timing fault in the `0x40xx` write glue
  (PAL16V8 + 74HC138 + 74HC123)**, not a logic bug. Matching stock's exact
  `TR2`-stop init sequence did **not** help.
- **The fix:** the common trait of every *working* reference (stock fw, bus-probe)
  is that they configure the UARTs **well after power-on**, never at boot. The
  `0x40xx` write glue is simply marginal right at power-on. So the bring-up was
  **deferred**: `board_init` leaves the ST16C454s **in reset** (PB4 high-Z)
  through power-on and USB enumeration; `uart_bringup()` then waits
  `BOARD_UART_BRINGUP_DELAY_MS` (100 ms), drives PB4 low (releases reset), and
  runs `uart_init`. With that, all 8 channels latch on the first pass. (A
  read-back retry on each LCR write is kept as cheap insurance against
  unit-to-unit variation.)

This is recorded in [bus_write_debug.md](bus_write_debug.md) and the comments in
`firmware/main.c` (`board_init`/`uart_bringup`) and `firmware/uart.c`.

## Hardware gate (run these — needs the physical MIDEX8 r1)

Each test needs a power-cycle (→ loader PID `0x1000`) + re-upload, with
`snd_usb_midex` unloaded so `snd-usb-audio` wins binding. Upload
`firmware/midex-spike-r1.ihx` via `host/midex-fw-upload` (or `fxload`), then:

1. **Enumerate / list:** `lsusb -d 0a4e:10c1 -v` shows one AudioControl +
   one MIDIStreaming interface with **8** embedded jacks each way; `amidi -l`
   lists `MIDEX8 UAC MIDI 1`…`MIDI 8`.
2. **All-8-port loopback:** patch a MIDI cable OUT→IN on each port, then
   `host/spike_loopback.py -n 8` — expect a byte-exact note-on round-trip on
   every cable (proves cable↔port order and the parser's channel-voice path).
3. **Parser robustness** (Verification step 4 in the plan): with one continuous
   `amidi -p PORT -d` receiver, stream:
   - running status (`90 3C 7F 3E 7F 40 7F`) → three note-ons surface;
   - a SysEx split across the 64-byte boundary;
   - a real-time byte (`F8`) mid-message → note completes, clock passes through;
   - sustained all-port throughput.
   Re-check the spike's deferred `F0`/`F8`/`FE` observations now that SysEx is
   framed as CIN 0x4 (see spike_bringup.md "Localized"): `F8`/`FE` were
   **host-input filtering** (not ours); `F0` was a device-side loopback artifact
   — confirm whether it persists with proper SysEx framing.

## Open / deferred (unchanged from the plan)

- **TX path** still busy-waits per byte on THRE (`bridge_tx` in `main.c`). FIFOs
  are enabled (FCR=0x07) so writes mostly don't stall, but a large host→device
  burst can briefly starve the RX pump; revisit only if RX FIFO overflow shows
  up under load.
- **PINSA per-port RX IRQ bitmap** — still the optional perf lever (1 MOVX vs 8
  LSR reads). Not needed for correctness; gate on a probe check of IER→PINSA
  polarity and re-trace per revision.
- **LEDs** left dark (no XDATA latch found).

## End-to-end validation (host/e2e_test.py, 2026-06-07)

A ctypes/ALSA-rawmidi harness (`host/e2e_test.py`, subcommands
functional/sysex/timing/jitter/throughput/soak/isolation/sysexsweep/selftest;
matches on *canonicalized* messages so running-status expansion isn't a false
fail) drove the device through a loopback cable. Results:

- **All channel-voice + system-common messages round-trip** byte-exact: note
  on/off (both forms), poly-AT, CC, program change, channel-AT, pitch bend, MTC
  quarter frame, song position/select, tune request. ✅
- **Running status** (`90 3C 7F 3E 7F 40 7F`) and **chords** round-trip. ✅
- **Short/medium SysEx** round-trips. ✅
- **Round-trip latency** (host→USB→fw→UART→DIN→cable→DIN→UART→fw→USB→host):
  mean ~2.0–2.4 ms, p99 ~2.7 ms, sd ~0.1 ms; **20 000-message soak: 0 drops,
  0 corrupt**. ✅
- **Throughput** below the UART ceiling (≤~1040 three-byte msg/s/port): 0 loss;
  loss only appears when pushed past that genuine 31250-baud limit. ✅

Bugs found and fixed during this validation:
1. **Multi-packet OUT bursts collapsed to their last packet** — `bridge_tx`
   busy-waited on the slow UART, starving the RX pump. Fixed by per-port TX ring
   buffers + a non-blocking `uart_tx_pump` (decouples UART from EP servicing).
2. **Spurious TX on random ports (stuck OUT LEDs) + corrupt first run** — the
   ring/parser state was reset *before* `usb_init`, so enumeration's iso-buffer
   activity (the 0x2000 XDATA region freed by ISODISAB) clobbered the indices.
   Fixed by resetting after `usb_init`.
3. **EP2-IN back-pressure starved RX** — added a device→host ring so
   `midi_rx_pump` always drains the UARTs and ships when EP2-IN is free.

### Resolved: long/sustained SysEx RX overrun ✅

A **sustained** stream (a long SysEx, hundreds of bytes) used to intermittently
drop a byte. Root cause (datasheet-confirmed): the **ST16C454 is a 16C450-class
part with NO FIFO** — a single-byte RHR — so at 31250 baud each byte must be read
within ~320 µs or it overruns, and the old main-loop RX poll was occasionally
delayed past that by other work.

**Fix (implemented):** a **high-priority Timer0 ISR** (`uart_rx_isr`, mode 2,
`__using 1`, `PT0=1`) now captures every received byte into per-port software
FIFOs; the main-loop parser drains them via `uart_rx_available`/`uart_rx_dequeue`.
The chip RHR is serviced on a hard timer cadence regardless of main-loop or
low-priority-USB-ISR delay, so sustained RX can no longer overrun. This mirrors
the stock firmware's Timer-driven RX poller. Full design + stock-firmware
comparison: [timer_isr_rx_capture_design.md](timer_isr_rx_capture_design.md).

**Performance tuning (prescaler).** The 8-port LSR sweep costs ~100 µs (SDCC
recomputes each channel's XDATA address per port); at a flat 100 µs tick it
nearly saturated the CPU — round-trip latency tripled (2 → 6 ms) and throughput
halved. A **/3 software prescaler** (`BOARD_T0_PRESCALE`) runs the sweep every
~279 µs (93 µs hardware tick), still well inside the ~640 µs RHR+shift overrun
window, which restored latency/throughput to baseline while keeping RX
overrun-proof. A cheaper future option (the **PINSA RX-bitmap pre-filter**, one
MOVX per tick) is documented in `firmware/board_r1.h` but needs the Port A
polarity/wiring verified on hardware first.

**Diagnostic counter limitation.** A vendor control request exposes a saturating
RX-overflow counter (`uart_rx_overflows`), but while the device is bound to the
in-kernel UAC (`snd-usb-audio`) driver the vendor control transfer does not get
through (pyusb sees a pipe error / timeout). So the counter is not readable in
normal operation; validation relies on byte-exact MIDI instead.

**Hardware-validated** (MIDEX8 r1, 3 loopback cables on ports 1–3):
- `sysexsoak --ports 1,2,3 --reps 500 --size 256`: **0 drops / 0 corrupt**.
- soak 50 000 round-trips: 0 drops / 0 corrupt, mean ~3.1 ms.
- timing 1 000: mean ~2.6 ms (baseline ~2.0–2.4 ms).
- throughput: 0 loss at 500 msg/s/port (loss only as the ~1040-baud ceiling is
  approached), matching the pre-fix baseline.
- functional 14/14, sysex 5/5 (incl. 200 B multi-packet).
