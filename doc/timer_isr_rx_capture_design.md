# Design — timer-ISR RX capture (overrun-proof sustained RX)

Status: **implemented & hardware-validated** · 2026-06-07
Resolves the long/sustained-SysEx RX overrun described in the
[firmware build & validation](firmware_build.md) note.

**Confidence legend:** ✅ confirmed on hardware / in stock RE · ⚠️ inferred / to-be-validated · ❓ open.

## Problem

The ST16C454 is a 16C450-class part with **no RX FIFO** — a single-byte RHR
(datasheet: offset 2 is ISR on read / nothing on write). At 31250 baud each
received byte must be read within ~320 µs (one byte time) or, once the receiver
shift register also fills, ~640 µs before a true overrun. The original RX path
polled each port's LSR from the main loop (`midi_rx_pump`) and that poll was
occasionally delayed past the byte window on a **sustained** stream (a long
SysEx, hundreds of bytes), dropping one byte. Single messages and short streams
are unaffected — only continuous streams hit it.

This was reproduced and tracked in [firmware_build.md](firmware_build.md).

## Fix (summary)

A **high-priority Timer0 ISR** captures every received byte into per-port
software FIFOs every ~100 µs, regardless of what the main loop or a low-priority
USB ISR is doing. The existing MIDI parser keeps running in the main loop,
draining those FIFOs instead of reading the chip directly. The chip's RHR is
therefore serviced on a hard ~100 µs cadence — comfortably inside the overrun
window — so sustained RX can no longer overrun.

This mirrors what the **stock firmware** does (a Timer-driven RX poller), with
three deliberate, justified divergences (see "Divergences from stock").

## Stock-firmware reference (Ghidra RE) ✅

Decoded from `midex8_firmware_combined.bin`, `fw_entry` @0x0311–0x0344 and the
ISRs. Both stock timers are 8051 **mode 2 (8-bit auto-reload)**, clocked
Fosc/12 = 0.5 µs/tick:

| Timer | Purpose | Reload | Period | Priority |
|-------|---------|--------|--------|----------|
| Timer0 `timestamp_timer0_isr` @0x0a5a | MIDI-in `[P3 f4 HH LL]` timestamps | TH0=`0x38` (200 ticks) | 100 µs | **HIGH** (`SETB PT0`) |
| Timer1 `uart_rx_poll_timer1_isr` @0x0aaa | RX drain | TH1=`0x46` (186 ticks) | 93 µs, **÷3 prescaler** | normal |

Stock's RX ISR (`@0x0aaa`) is **pure capture, no MIDI parsing**: it sweeps ports
0–7, tests LSR `0x4045+p*8` bit0 (Data Ready), reads RHR `0x4040+p*8`, and pushes
the raw byte into a **per-port 256-byte XDATA ring** (page `0x90+p`, head/tail
indices in IDATA). On a full ring it **drops the byte and sets an overflow flag**
(`SETB 0x3b`); it sets a "RX pending" flag for the main loop (`SETB 0x18`) and
runs in a **separate register bank** (`SETB RS1` → bank 2). Effective RX sweep
cadence ≈ 279 µs at normal priority — stock relies on the ~640 µs RHR+shift
double-buffer to cover that.

This validates our minimal-capture design point-for-point.

### Divergences from stock (each justified)

1. **RX gets the high-priority slot.** Stock spent its high-priority interrupt on
   the *timestamp* timer and left RX at normal priority. We are class-compliant
   and emit **no timestamps**, so there is no timestamp timer — the high-priority
   slot is free. Putting *RX capture* at high priority is therefore strictly safer
   than stock at no extra cost. It is *defense-in-depth*: the everyday win is just
   "an interrupt preempts the slow main loop"; high priority additionally
   guarantees RX even if a long `sudav_isr` (the only heavy USB ISR) lands
   mid-stream during a control transfer.
2. **16 B/port FIFO, not 256.** Stock can afford 256 B/port because it has the
   external CY62256 SRAM (uses XDATA `0x8000`/`0x9000`). The class firmware lives
   in the 2 KB freed by `ISODISAB` at `0x2000` (tx_ring already uses 1 KB), so
   16 B/port (128 B total) is the scaled-down equivalent — ample because
   high-priority capture keeps the FIFO nearly empty.
3. **100 µs / no prescaler, not 279 µs / ÷3.** The prescaler only existed in stock
   so one timer could serve a faster purpose; we have a dedicated timer with no
   other duty, so we set the period directly. 100 µs is simpler (no prescaler
   counter) and gives more margin than stock's proven 279 µs.
   > **Superseded by HW measurement (adopted the prescaler).** The ~20% CPU
   > estimate below was wrong: on hardware the 8-port LSR sweep is ~100 µs (SDCC
   > recomputes each channel's XDATA address per port), so a flat 100 µs tick
   > nearly saturated the CPU — round-trip latency tripled (2→6 ms) and throughput
   > halved. The ÷3-prescaler fallback was adopted (`TH0 = 0x46` + a counter in the
   > ISR → 279 µs effective, `BOARD_T0_PRESCALE`), restoring latency/throughput to
   > baseline while staying inside the ~640 µs overrun window. Next-level option:
   > the PINSA RX-bitmap pre-filter (one MOVX/tick), pending HW polarity check.

## Architecture

```
   ST16C454 RHR  ──(Timer0 ISR, ~100µs, high prio)──►  per-port raw FIFO (XDATA, uart.c)
                                                              │
                                                              ▼  (main loop)
                              midi_rx_pump: uart_rx_available / uart_rx_dequeue
                                                              │
                                                              ▼
                                       parse_byte ──► rx_ring ──► EP2-IN (host)
```

Single-producer/single-consumer throughout: the ISR is the **sole** reader of the
chip RHR and the **sole** writer of the FIFO `head`; the main loop is the sole
writer of `tail` and the sole owner of the parser state and `rx_ring`. The ISR
touches **no** parser/USB/`rx_ring` state, so it is safe to preempt a USB ISR.

### Components

1. **Timer0 setup** — mode 2 (8-bit auto-reload), reload constant for ~100 µs
   (`TH0 = 0x38` = 256 − 200 ticks @ 0.5 µs/tick), Fosc/12 (CKCON T0M already 0
   after `board_init`'s `CKCON &= ~0x27`). `ET0 = 1` (enable), `PT0 = 1` (high
   priority), `TR0 = 1` (run). Reload constant lives in `board_r1.h` as a
   documented `#define` derived from a µs value, tunable per revision.
   - *Fallback if CPU-bound (~20% ISR CPU at 100 µs):* mirror stock — `TH0 = 0x46`
     (93 µs) + a ÷3 software prescaler counter in the ISR → 279 µs effective,
     ~8% CPU. Documented here so it can be adopted without redesign.

2. **`uart_rx_isr(void)`** — Timer0 ISR body in `uart.c`; vector declared
   `extern void uart_rx_isr(void) __interrupt TF0_VECTOR __using 1;` in `main.c`
   (matching the existing USB-ISR pattern), defined `__interrupt TF0_VECTOR
   __using 1` in `uart.c`. `__using 1` gives it a private register bank (bank 0 is
   main/USB) so nesting over a USB ISR can't clobber registers; SDCC preserves
   ACC/B/DPTR/PSW automatically. Mode-2 hardware clears TF0 on vector entry — no
   manual ack.
   - **Fully self-contained:** sweeps ports `0..NUM_MIDI_PORTS-1`; for each, reads
     LSR via `UART_REG(p, UART_LSR)`, and if `UART_LSR_DR` set, reads
     `UART_REG(p, UART_RHR)` and pushes to that port's FIFO — all inline, calling
     **no** shared function. This avoids the SDCC non-reentrant-overlay hazard
     (the ISR must not call any function the main loop also calls).
   - **Overflow:** if the FIFO is full, drop the byte and increment the global
     `uart_rx_overflows` (saturating at `0xFF`).

3. **Per-port raw RX FIFO** (`uart.c`, XDATA) — `MIDEX_RX_FIFO_SIZE = 16`
   bytes/port (power of two; one slot reserved to distinguish full/empty → 15
   usable), `NUM_MIDI_PORTS` ports = 128 B at the `0x2000` region. Per-port
   `head` (ISR-written) and `tail` (consumer-written), each a single byte (atomic
   on the 8051). A comment records that the size can be bumped if
   `uart_rx_overflows` ever trips.

4. **RX consume op-set** (`uart.h`) — replaces the old chip-reading
   `uart_rx_ready`/`uart_getc`:
   ```c
   bool    uart_rx_available(uint8_t port);   /* FIFO non-empty            */
   uint8_t uart_rx_dequeue(uint8_t port);     /* pop one byte (gate first) */
   ```
   These read the software FIFO, not the chip. `midi_rx_pump` keeps its current
   shape, with the `rx_ring`-room check **before** the dequeue so a full ring
   leaves bytes safely in the FIFO:
   ```c
   while (uart_rx_available(p)) {
       if (rx_ring_full_for_packet()) break;   /* leave byte in FIFO */
       parse_byte(p, uart_rx_dequeue(p));
   }
   ```
   TX ops (`uart_tx_ready`/`uart_putc`) are **unchanged** — TX has no hard
   deadline and stays the non-blocking main-loop `uart_tx_pump`.

5. **Lifecycle** (`uart.c`, called from `main()`):
   - `uart_rx_reset(void)` — zero every port's FIFO `head`/`tail` and
     `uart_rx_overflows`. Called **after** `usb_init` (XDATA at `0x2000` is not
     auto-zeroed and is clobbered by enumeration's iso-buffer activity), alongside
     `tx_reset()` / `midi_parser_reset()`.
   - `uart_rx_start(void)` — program + enable Timer0. Called **after**
     `uart_bringup()` so the UART is configured (RHR/LSR read sane) and the FIFOs
     are already reset before the first capture.

   `main()` order:
   ```
   board_init();
   usb_init();                 // RENUM; frees 0x2000 XDATA
   EA = 1;
   tx_reset();  midi_parser_reset();  uart_rx_reset();   // all after usb_init
   uart_bringup();             // release UART reset + uart_init (line cfg)
   uart_rx_start();            // program + enable Timer0 (TR0/ET0/PT0)
   OUT2BC = 0;
   while (1) { bridge_out(); uart_tx_pump(); midi_rx_pump(); }
   ```
   Stock's "RX pending" flag is **not** replicated — `midi_rx_pump` already
   cheaply checks `uart_rx_available(p)` per port each pass.

6. **Vendor control request** (`usb.c`, `usb_handle_setup_data` `default` case) —
   exposes the overflow counter for the host test. Class-compliance-safe: the OS
   driver (`snd-usb-audio`) ignores vendor requests; only an explicit
   `libusb`/`pyusb` control transfer sees them, and EP0 control transfers work
   even while the kernel driver has the interface claimed.
   - vendor IN, `bRequest = 0x01` → returns 1 byte = `uart_rx_overflows`.
   - vendor OUT, `bRequest = 0x02` → clears `uart_rx_overflows` (host resets per
     run). ⚠️ a tiny read-modify race vs the ISR increment is acceptable for a
     diagnostic counter.

## Concurrency / correctness invariants

- **FIFO is SPSC.** ISR writes `head` only; main writes `tail` only. Single-byte
  indices are atomically read/written on the 8051 → no locking, no interrupt
  masking needed.
- **No chip-access race** between `uart_tx_pump` (main) and the ISR: the ISR only
  *reads* LSR (non-destructive for the THRE bit main polls; DR is cleared by the
  RHR read, not the LSR read) and *reads* RHR (offset 0 read = RHR, a different
  internal register from the THR write `uart_putc` performs). The 8051 completes
  any MOVX before taking the interrupt, so no mid-instruction preemption.
- **ISR shares no state with USB ISRs or the parser** — only the SPSC FIFO and the
  saturating counter — so high-priority preemption of a USB ISR is safe.
- **Timer starts after enumeration**, so USB enumeration timing is unaffected.

## Acceptance / validation

**Build-time:**
1. `make class` clean on SDCC 4.2.0; code size < `0x1B00`; XDATA < 2 KB (new
   128 B FIFO + counter fit the slack).

**Host e2e (`host/e2e_test.py`, loopback cable(s), physical MIDEX8 r1 after a
power-cycle → loader PID + re-upload):**

2. **Regression** — all existing subcommands pass byte-exact
   (functional / sysex / running-status / chords / timing / jitter / throughput /
   isolation / 20 000-msg soak).
3. **Fix target (headline)** — new **`sysexsoak -n 3`**: a long SysEx (several
   hundred bytes) repeated many times on **3 ports simultaneously** (the available
   cable count; scales with `-n` when more cables are present) → **0 drops /
   0 corrupt**. The ISR sweeps all 8 ports every fire regardless of cable count,
   so worst-case ISR duration / CPU is exercised even with 3 cables; 3 concurrent
   sustained streams stress the concurrent-RX path.
4. **RX-overflow assertion** — read the vendor counter (IN `0x01`) after the
   sustained run; it must be **0**. Non-zero-but-byte-exact ⇒ the FIFO absorbed but
   we ran close ⇒ bump `MIDEX_RX_FIFO_SIZE`. Reset (OUT `0x02`) per run.
5. **Throughput sanity** — loss still only appears *above* the genuine ~1040
   three-byte-msg/s/port UART ceiling (the high-priority ISR didn't lower the
   achievable rate).

## Out of scope

- TX path stays the main-loop non-blocking pump.
- PINSA per-port RX-IRQ bitmap (1 MOVX vs 8 LSR reads) is an optional ISR
  micro-optimisation; not needed for correctness.
- LEDs remain dark.
