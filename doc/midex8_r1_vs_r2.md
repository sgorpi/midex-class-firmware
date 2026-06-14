# MIDEX8 r1 vs r2 — firmware reverse-engineering comparison

An RE comparison of the stock proprietary firmware of the two MIDEX8 hardware
revisions, used to derive a `board_r2.h` for the class-compliant firmware
project (`src/midex-class-firmware/`).

- **r1** = MIDEX8 rev 1, **EZ-USB AN2131** + 2× ST16C454 (8 external UART channels).
  Firmware: [`doc/firmware/midex8_firmware_combined.bin`](../../../doc/firmware/midex8_firmware_combined.bin) (6007 B).
- **r2** = MIDEX8 rev 2, **EZ-USB FX CY7C646** + 1× ST16C454 + 1× ST16C452
  (6 external UART channels). Firmware:
  [`doc/firmware/midex8r2_combined.bin`](../../../doc/firmware/midex8r2_combined.bin) (6906 B).

Both imported into the `Steinberg MIDEX` Ghidra project as `8051:BE:16:default`
(Ghidra's 8051 is big-endian). All addresses are CODE-space unless prefixed.
Companion: [`hardware_register_map.md`](hardware_register_map.md)
(the r1 single-source-of-truth this builds on).

**Confidence legend:** ✅ confirmed in disassembly (and cross-checked against the
CY7C646 TRM / ST16C45x datasheets) · ⚠️ inferred, needs hardware confirmation ·
❓ open question.

---

## TL;DR — the headline

**r2 is a hybrid MIDI backend, exactly as hypothesised.** Its 8 ports are driven
by **two different UART subsystems**:

| MIDI port | backend | how |
|-----------|---------|-----|
| **0–5** (6 ports) | external 16550 (ST16C454 ×1 + ST16C452 ×1) | MOVX to `0x4040 + port*8`, same window as r1 ✅ |
| **6** | EZ-USB FX **on-chip UART0** (`SCON`/`SBUF`, SFR 0x98/0x99) | `SBUF` TX, `RI`/`TI` IRQ ✅ |
| **7** | EZ-USB FX **on-chip UART1** (`SCON1`/`SBUF1`, SFR 0xC0/0xC1) | `SBUF1` TX, `RI`/`TI` IRQ ✅ |

So r2 is **not** a near-mechanical repeat of r1. The on-chip-serial half is the
**same mechanism MIDEX3 uses** (the doc's MIDEX3 scoping predicted this). For the
class firmware this means r2 needs the *second* UART backend behind the `uart.c`
op-set — r2, not MIDEX3, is its first real consumer.

Two further r2-specific findings:
- **External UART divisor = 24** (not r1's 1) ⇒ a **fixed 12 MHz** external UART
  clock, *not* Timer2→PB7 like r1. ✅ **Confirmed:** both PCBs carry a **12 MHz
  crystal (marking `FS120`)** — the standard EZ-USB core crystal, which on r2 also
  clocks the external ST16C45x (12 MHz / (16×24) = 31250, independent of the
  24/48 MHz core strap; it can't be CLKOUT, which is 24/48 MHz).
- The firmware is **core-clock-agnostic (24 or 48 MHz)**: it reads the read-only
  **CPUCS bit 3 (“24/48”)** boot strap and re-tunes SMOD + the Timer2 reload to
  hold 31250 baud and the RX tick at either speed. ✅

---

## 1. Interrupt vector table ✅

| Vector | addr | r1 handler | r2 handler | r2 role |
|--------|------|-----------|-----------|---------|
| reset  | `0x0000` | →`0x01E3` | →**`0x0066`** `fw_entry` | init, tail-calls `fw_main` |
| Timer0 | `0x000B` | `0x0A5A` | `0x0DDB` `timestamp_timer0_isr` | 16-bit timestamp + frac accumulator |
| **INT1** | `0x0013` | *(none)* | **`0x0DA5`** `timestamp_capture_int1_isr` | latches timer snapshot on INT1 edge ❓ |
| Timer1 | `0x001B` | `0x0AAA` (RX poll) | `0x0EED` `timer1_isr` (**empty**) | Timer1 is only the UART baud gen |
| **Serial0** | `0x0023` | *(none)* | **`0x0EEE`** `serial0_isr` | on-chip UART0 → port 6 |
| **Timer2** | `0x002B` | *(none)* | **`0x0E24`** `uart_rx_poll_timer2_isr` | **unified RX-capture poller (all 8 ports)** |
| Serial1/Resume | `0x0033` | `0x0B17` (stub) | `0x0EE1` `resume_isr` (stub) | FX Resume/Wakeup, clears a flag |
| **Serial1/UART1** | `0x003B` | *(none)* | **`0x0F10`** `serial1_isr` | on-chip UART1 → port 7 |
| USB    | `0x0043` | `0x0100` | `0x0100` (autovector thunk) | EZ-USB SETUP/EP service (shared) |
| INT4/I²C etc. | `0x004B`+ | `0x137F` | (EP/I²C helpers) | — |
| ext (0x5B) | `0x005B` | *(none)* | `0x0EE6` | clears `EXIF` (SFR 0x91) bit7, stub |

**What changed:** r1 used **Timer0** (timestamp) + **Timer1** (RX poll) and no
serial ports. r2 frees Timer1 to be the **UART baud generator**, moves the
**RX poll to Timer2**, lights up **both on-chip serial vectors** (0x23 + 0x3B),
and adds an **INT1** timestamp-capture handler. The USB autovector (0x43→0x0100)
is unchanged between revisions.

> ⚠️ **Ghidra SFR mislabel.** The `8051:BE:16:default` SFR map is *not* the FX
> variant, so it renders UART1's registers wrong: **`FIFLG` is really `SCON1`
> (SFR 0xC0)** and **`FIFLG1` is really `SBUF1` (SFR 0xC1)** (CY7C646 TRM §18,
> Table 18-15). Read every `FIFLG*` in the decompiler as the second UART.

---

## 2. UART backend — the load-bearing comparison ✅

### 2.1 External 16550 channels (ports 0–5) — same window, different clock

Function `uart_ext_init_6ports` @ **`0x169A`** (r2) vs `uart_init_all_ports`
@ `0x1177` (r1). r2 loops **6** ports (`R7 = 6`), r1 loops 8. Per channel at
`0x4040 + port*8` (identical address window and standard 16550 offsets):

| Step | r1 (`0x1177`) | r2 (`0x169A`) |
|------|---------------|---------------|
| LCR (off3) | `0x83` (DLAB=1, 8N1) | `0x83` ✅ same |
| **DLL (off0)** | **`0x01`** | **`0x18` = 24** ⚠️ different |
| DLM (off1) | `0x00` | `0x00` |
| LCR (off3) | `0x03` (DLAB=0, 8N1) | `0x03` ✅ same |
| MCR (off4) | `0x00` | `0x00` |
| FIFO (FCR) | not written (16450 mode) | not written (16450 mode) |

- **Divisor 24 ⇒ external XIN = 31250 × 16 × 24 = 12 MHz** (`set` @`0x16B9`).
  r1's divisor 1 implied a 500 kHz XIN generated on-chip by Timer2→PB7.
- r2's init does **not** route Timer2 to PB7 (`PORTBCFG 0x7F94 = 0x0C`, bit7
  clear), and Timer2 is repurposed as the RX poller — so the 12 MHz is a board
  clock, not MCU-generated. ✅ **PCB confirms a 12 MHz crystal (marking `FS120`)**
  on both revisions (the EZ-USB core crystal; on r2 the same 12 MHz also feeds the
  ST16C45x XIN). ❓ remaining: confirm the XIN *routing* (direct crystal tap vs a
  buffer) on the r2 board.
- The divisor write is **unconditional** (no CPUCS.3 branch), consistent with the
  fixed 12 MHz external clock that doesn't scale with the 24/48 MHz core.
- TX/RX/LSR access helpers: `uart_ext_write_thr` @`0x1719`, `uart_ext_read_rhr`
  @`0x170D`, `uart_ext_read_lsr` @`0x16FC` — all `0x4040 + port*8` + offset.
- `uart_ext_init_6ports` opens with a `CLR P1.4` / `SETB P2.4` GPIO strobe ❓
  (purpose TBD — possibly an external-UART reset/enable; r2's reset wiring differs
  from r1's PB4/OEB.4 and must be re-traced).

### 2.2 On-chip UARTs (ports 6 & 7) — identical to MIDEX3 ✅

Configured inline in `fw_main` init:

```
SCON  (0x98)        = 0x56     ; UART0 mode1, REN=1            -> port 6
SCON1 (0xC0,"FIFLG")= 0x56     ; UART1 mode1, REN=1            -> port 7
PCON.SMOD (0x87.7)  = 1        ; double baud (24 MHz core)
TH1 = TL1           = 0xF4     ; Timer1 mode2 reload = 12
CKCON.T1M (0x8E.4)  = 1        ; Timer1 clock = CLKOUT/4
TR1                 = 1        ; run Timer1 (shared baud gen for both UARTs)
ES = 1, EC = 1                 ; enable UART0 + UART1 interrupts
```

Baud check (mode 1, Timer1): `(2^SMOD / 32) × Fcore / (4 × (256−0xF4))`
= `(2/32) × 24e6 / (4×12)` = **31250 baud** ✅. The same Timer1/SMOD/T1M baud setup
appears in **MIDEX3**, whose re-analysis this pass **confirmed is also an FX-class
CY7C646** (`hardware_register_map.md`): it drives port 0 on UART0 and port 1 on
this same UART1 at **vec 0x3B** (but leaves `REN_1` clear → TX-only, `SCON1=0x46`
vs r2's `0x56`), plus a Timer2 **bit-banged** 3rd port on PC3 — so r2's on-chip half
is two-thirds of the MIDEX3 backend.

> Per CY7C646 TRM: UART1 cannot use Timer2 as its baud generator, so both UARTs
> share **Timer1**. UART1's pins are alternate-function (TRM: **PB2 = RxD1**);
> UART0 uses the standard P3.0/P3.1 (RxD0/TxD0). ❓ confirm port 6/7 RxD/TxD pin
> routing on the r2 PCB.

### 2.3 RX architecture ✅ — one Timer2 ISR drains everything

`uart_rx_poll_timer2_isr` @ **`0x0E24`** is r2's single RX-capture ISR (replaces
r1's Timer1 poller `0x0AAA`). It runs in **register bank 2** with a **/3 software
prescaler** (IDATA `0x50`):

1. **Ports 0–5:** poll external LSR `0x4045 + p*8` bit0 (DR), read RHR
   `0x4040 + p*8`, push into per-port **256-byte XDATA ring at page `0x90+p`**
   (head idx IDATA `0xA8+p`, tail IDATA `0x90+p`) — **same ring layout as r1 stock.**
2. **Port 6:** if `_d_7` (set by `serial0_isr` on `RI`), read `SBUF` → ring page `0x96`.
3. **Port 7:** if `_e_0` (set by `serial1_isr` on `RI`), read `SBUF1` → ring page `0x97`.

The two on-chip serial ISRs only **flag** RX (`serial0_isr`/`serial1_isr` clear
`RI` and set a pending bit); the actual `SBUF`/`SBUF1` read happens in the Timer2
ISR. ⚠️ This is a **single-byte buffer** — Timer2 must read within ~320 µs
@31250 or the on-chip UART byte is lost (no FIFO on the FX UARTs). The external
ST16C45x also have no FIFO enabled, same ~640 µs RHR+shift window as r1.

Timer2 reload: `RCAP2 = 0xFF46` (24 MHz) or `0xFE8C` (48 MHz) — both give a
**93 µs** base tick; ×3 prescaler ⇒ **279 µs** effective sweep, matching r1's
Timer1 poller (TH1=0x46 ÷3). *(This is the stock design the class firmware's own
Timer0 RX-capture ISR was modelled on.)*

### 2.4 TX architecture ✅ — polled in `fw_main`, split by backend

The main loop scans ports 0–7 and dispatches each port's next byte:

| Port(s) | TX path |
|---------|---------|
| 0–5 | poll external LSR bit5 (THRE) → write THR `0x4040+p*8` |
| 6 | stage byte in IDATA `0x73`, set UART0 `TI` (ISR reloads `SBUF`) |
| 7 | stage byte in IDATA `0x74`, set UART1 `TI` (ISR reloads `SBUF1`) |

---

## 3. Clock-agnostic 24/48 MHz operation ✅

The FX boots at 24 or 48 MHz depending on an EEPROM strap, exposed read-only as
**CPUCS (`0x7F92`) bit 3, “24/48”** (CY7C646 TRM, EEPROM config byte bit 2
“48MHZ”; default 24 MHz). `fw_main` tests `CPUCS.3` and adapts:

| When CPUCS.3 = 1 (48 MHz) | why |
|---------------------------|-----|
| `PCON.SMOD` cleared, `CF` cleared | CLKOUT doubled → drop the ×2 baud multiplier to keep 31250 |
| Timer2 reload `0xFE8C` instead of `0xFF46` | 372 counts @ (48/12) MHz = same 93 µs RX tick |
| extra settle-delay iterations | longer loops to match wall-clock at 2× core |

The external 16550 divisor stays **24** in both cases (its 12 MHz XIN is
independent of the core). Net: one firmware image runs correctly on either
strap. r1 (AN2131) has no such branch.

---

## 4. Proprietary timestamp engine — present in r2, not needed by class fw ✅

Same as r1: `timestamp_timer0_isr` @`0x0DDB` keeps a 16-bit counter + fractional
accumulator (Timer0 mode2, TH0=0x38, /2 prescaler via `_c_7`) feeding the
EP2-IN timestamped MIDI-in report. **New in r2:** `timestamp_capture_int1_isr`
@`0x0DA5` latches `{counter, TL0, prescaler phase}` into XDATA `0x9A00–0x9A03`
on each **INT1** (P3.3) edge — a hardware-triggered timestamp snapshot. ❓ What
drives INT1 is unknown (likely a USB-SOF or external sync pulse for jitter
correction). The class firmware **discards this whole layer** (no per-event
timestamps in class-compliant USB-MIDI), so the engine is documentation-only.

---

## 5. r2 Ghidra function map (this pass)

| Addr | Name | Role |
|------|------|------|
| `0x0066` | `fw_entry` | reset, tail-calls `fw_main` |
| `0x0204` | `fw_main` | init + main TX/USB polling loop |
| `0x0100` | `thunk_FUN_CODE_0f32` | USB autovector entry |
| `0x0DA5` | `timestamp_capture_int1_isr` | INT1 timestamp latch (new) |
| `0x0DDB` | `timestamp_timer0_isr` | timestamp counter + frac |
| `0x0E24` | `uart_rx_poll_timer2_isr` | **unified 8-port RX capture** |
| `0x0EE1` | `resume_isr` | FX Resume/Wakeup stub |
| `0x0EE6` | `FUN_CODE_0ee6` | EXIF.7 clear stub |
| `0x0EED` | `timer1_isr` | empty (baud gen only) |
| `0x0EEE` | `serial0_isr` | on-chip UART0 → port 6 |
| `0x0F10` | `serial1_isr` | on-chip UART1 → port 7 |
| `0x169A` | `uart_ext_init_6ports` | external 16550 init (6 ch, divisor 24) |
| `0x16FC` / `0x170D` / `0x1719` | `uart_ext_read_lsr` / `_read_rhr` / `_write_thr` | external UART access helpers |

Plate/EOL comments capturing the above were written to the program and saved.

---

## 6. Open questions — what needs PCB trace or DMM 📐

Items 1–3 were the build-blockers; all three are now **RESOLVED** (PCB trace +
datasheet + decompile, June 2026) and encoded in `firmware/board_r2.h`. Items 4–6
remain but don't block the build.

1. **External UART clock routing.** ✅ **RESOLVED.** Divisor 24 ⇒ 12 MHz, from the
   board `FS120` 12 MHz crystal. The two external chips **share one clock node**:
   **ST16C454 pin35 XTAL1 → ST16C452 pin4 CLK** (PCB trace). Firmware only needs
   the divisor (fixed 24, unconditional), so the clock tree is fully documented.

2. **External-UART reset / enable wiring.** ✅ **RESOLVED.** Two chips, **opposite
   reset polarity**, each on a distinct FX SFR-port pin (FX maps IOA/IOB/IOC =
   PORTA/B/C, so the stock `P1.4`/`P2.4` are **PB4**/**PC4**), driven as a static
   level (not pulsed):
   - **ST16C454 RESET pin37 = PB4 (IOB.4), active-high** → driven **low** to run
     (`OEB.4=1`, `OUTB.4=0`; stock also redundantly `CLR P1.4`).
   - **ST16C452 Master Reset pin39 = PC4 (IOC.4), active-low** (datasheet) → driven
     **high** to run (`OEC.4=1`, `P2.4=1`).
   The class fw keeps both asserted through enumeration and releases them in
   `uart_bringup` (the same late-bring-up the r1 glue needs).

3. **Port↔channel physical mapping.** ✅ **RESOLVED — identity** (HW loopback,
   2026-06-13). Self-loop round-trips on jacks 1,2,3,6,7,8 and a cross cable
   (jack 7 OUT → jack 8 IN lighting up ALSA OUT 7 → IN 8) confirm **firmware port
   index N = panel jack N+1** for both TX and RX, including the on-chip pair (ALSA
   7 = jack 7 = UART0, ALSA 8 = jack 8 = UART1). The on-chip pair is **not**
   reversed — the tentative "RxD0 → jack 8" read is not borne out (a self-loop on
   jack 7 alone round-trips on ALSA 7). `usb_descriptors.c`'s provisional order is
   correct as shipped; no change needed.

Also settled during the firmware pass: **`0x7FE5` is `AUTODATA`** (the EZ-USB
autopointer data register), not an FX-mandatory init — `BOARD_HAS_FX_INIT` was a
red herring, dropped. **`PC2` (IOC.2)** is a stock output held low whose purpose is
untraceable on the PCB; `board_r2.h` mirrors it verbatim (`BOARD_MIRROR_PC2_LOW`)
as insurance against it being an external-bus buffer-OE, to be dropped once
bring-up proves ports 0–5 work without it.

4. **INT1 (P3.3) source.** What hardware signal triggers
   `timestamp_capture_int1_isr`? → **Trace P3.3 / INT1#** (USB SOF tap? sync
   input?). Only matters for understanding the stock timing scheme; irrelevant to
   the class build.

5. **24 vs 48 MHz strap on this board.** CPUCS.3 is set by the EEPROM (or default
   24 MHz with no EEPROM). → **Read the r2 EEPROM / confirm the CLKOUT frequency**
   to know which branch the real unit takes (affects only timing margins; the
   class firmware can pin its own clock).

6. **PID / descriptors.** Not analysed this pass — r2's USB IDs and the
   external-vs-on-chip jack assignment in the stock descriptors. (Class firmware
   supplies its own descriptors, so low priority.)

---

## 7. Implications for the r2 class firmware

- **`board_r2.h` needs a hybrid UART backend**, not a parameter tweak of
  `board_r1.h`:
  - external 16550 ops for ports 0–5 (reuse r1's MOVX backend at `0x4040+p*8`,
    but **divisor 24**, and the external clock is a board oscillator);
  - an **on-chip-UART backend** for ports 6–7 (`SCON`/`SBUF` + `SCON1`/`SBUF1`,
    Timer1 baud gen, the same code path MIDEX3 will need) behind the `uart.c`
    op-set seam.
- The class firmware can pin its **own clock assumptions** (e.g. force 24 or
  48 MHz, or just honour CPUCS.3 like stock)
- The 16-bit timestamp engine, INT1 capture, and EP2-IN timestamp format are all
  **dropped** (class-compliant USB-MIDI has no per-event timestamps).
- **Gate before building:** resolve open questions 1–3 (external clock, reset
  wiring, port mapping) — those are the only items that block finalising r2
  descriptors + bring-up.
