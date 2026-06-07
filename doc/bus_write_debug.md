# MIDEX8 r1 bus-probe bring-up — status & the external-write puzzle

Status of the Phase-1 hardware confirmation (the EP0 MOVX bus-probe). Read
together with [hardware_register_map.md](hardware_register_map.md).

## ✅ RESOLVED (2026-06-05) — the UART was held in RESET; drive PB4 low

**Root cause: the ST16C454 RESET pin (active-high) was held high because the
probe left AN2131 `PB4` as a high-impedance input (external pull → high).** The
stock firmware drives `PB4` *low* — and that is the one bus-bring-up step the
probe was missing. Fix, added to `board_init` (`main.c`):

```c
PORTBCFG &= ~INT4;   /* 0x7F94: PB4 = GPIO (not INT4 alt-function)         */
OUTB     &= ~OUTB4;  /* 0x7F97: PB4 output latch = 0                       */
OEB      |=  OEB4;   /* 0x7F9D: PB4 = driven output -> RESET de-asserted   */
```

How stock does it: `fw_entry` clears `PORTBCFG.4`/`OUTB.4` at boot, then on the
host **START** command (`0xFD`, handled in `midi_in_aggregate` @ `0x10A8`) sets
`OEB.4` to actually drive `PB4` low — immediately before `uart_init_all_ports`
(`0x1177`). On the **STOP** command (`0xF5`) it does `OEB &= ~0x10`, releasing
`PB4` back to high-Z (the chip re-enters reset). PCB trace that pointed here:
PALLV16 pin 6 → ST16C454 pin 37 (RESET) → AN2131 PB4.

**Validation (scope-free, pure software):**
- `map_writes.py`: UART scratch regs `0x405F` / `0x407F` now **OK** (were FAIL
  every prior session); SRAM/XRAM still OK.
- `loopback.py` with a real MIDI cable OUT→IN: every test byte (0x55/AA/3C/7E/01)
  written to a channel's THR came back on the **same** channel's RHR, on **both**
  port 8 (`0x4078`) and port 1 (`0x4040`). Full path proven:
  `MOVX → THR → ST16C454 TX → MIDI OUT → cable → MIDI IN → opto → RX → RHR`.

**Bonus finding:** the channel↔physical-port mapping is **linear** —
`port N (1-based) = 0x4040 + (N-1)*8` (port 8 = `0x4078`). The earlier DMM
"hammering 0x407F lit the first chip" reading was a mis-probe; no remap needed.

> ⚠️ **Correction to the historical log below:** the §"RESET ruled out" entry
> (2026-06-02) was **wrong**. RESET *was* the cause. PB4 read HIGH under the
> probe AND a non-started stock fw only because stock drives PB4 low **after the
> host START (0xFD) command** — the DMM was sampled before any start, so both
> sat the pulled-up default. The investigation below is kept verbatim for
> history; everything from the "RESET ruled out" entry onward is superseded.

## TL;DR (historical — see RESOLVED above)

The bus-probe **confirmed the read side of the static RE**: the 8 UARTs are at
`0x4040 + port*8`, standard 16550 offsets, and reads return real per-register
values (e.g. `LSR=0x60`, `LCR=0x00`). **External *writes* to the UART do not
land**, even though our firmware now matches the stock firmware on every bus
register, instruction, and (tested) execution context. Stock firmware MIDI-out
is confirmed working on this exact unit, so the write path is *good hardware* —
the cause is something still not captured at the register level. Next step is a
multimeter on the data-bus buffer / write-strobe routing (procedure below).
*(Resolved: the missing register was `OEB.4` → PB4 → UART RESET; see top.)*

## What is confirmed working ✅

- Enumeration as `0x0A4E:0x10C0` ("MIDEX8 bus-probe"), `snd-usb-audio`-free.
- EP0 vendor commands: read XDATA byte (`0xB0`), write XDATA byte (`0xB1`).
- **External bus READS** of the UART window — return correct register values,
  which **confirms `UART[port].base = 0x4040 + port*8` and the 16550 offsets.**
- **Internal XRAM writes** (vendor write to `0x2300`) round-trip perfectly — so
  the vendor-write firmware path and the `MOVX @DPTR,A` it emits are correct.
- The bus bring-up that made reads work: `PORTCCFG |= 0xC0` (RD#/WR# on PC7/PC6).

## The puzzle ❌ — external writes never land

`diag.py` section C: writing any pattern to UART `SCR` (`0x407F`) then reading
it back always returns `0xFF`. The divisor-latch readback after `uart_init`
shows `DLL=00 DLM=00 LCR=00` — i.e. none of the config writes stuck. Reads of
those same registers return real values, so it is **write-specific**.

### Hypotheses tested and eliminated

| # | Hypothesis | Test | Result |
|---|------------|------|--------|
| 1 | EP0 status-stage race dropped writes | moved write handling into SUDAV ISR (synchronous) | still fails |
| 2 | RD#/WR# strobes not enabled | `PORTCCFG=0xC0` confirmed via readback | reads work, writes don't |
| 3 | MOVX cycle stretch wrong | matched stock `CKCON` (T2M=0, stretch=0) | still fails |
| 4 | CPU clock-speed mismatch | AN2131 is fixed 24 MHz (no CLKSPD bits); only `CLK24OE` differs | not a clock-speed knob |
| 5 | WR# pulse too narrow | 24 MHz/stretch-0 ⇒ ~168 ns WR#, ≫ ST16C454 min (~50–90 ns) | width is fine |
| 6 | WR# not generated for `0x4078` | TRM Table 8-2: RD#+WR# active for `0x2800–0x7B40` with ISODISAB=1 | WR# is generated |
| 7 | CS#/OE# pins needed | stock never writes `PORTACFG`; pokes had no effect | not it |
| 8 | A different EZ-USB register state | `experiment.py` swept CPUCS/PORTACFG/OEx/`0xFE00` from host | none enabled writes |
| 9 | Stock uses a special write opcode | disassembled stock: plain `MOVX @DPTR,A` (`F0`), same as ours | identical |
| 10 | Writes only fail from ISR context | writes deferred to main-loop context, re-uploaded + `diag.py` §C | **ELIMINATED** — `SCR@0x407F` still `xx->FF` for every pattern from main-loop context; failure is context-independent |

Register dump under our firmware (matches stock's post-init state):
`CPUCS=0x42` (CLK24OE set — stock clears it, but pokes proved it irrelevant),
`PORTACFG=0x00 PORTBCFG=0x80 PORTCCFG=0xC0 OEA/B/C=0x00`,
`OUTA=0xA6 OUTB=0xDE OUTC=0xEF`, `PINSA=0xE4 PINSB=0x3F PINSC=0xEF`, `ISOCTL=0x01`.

### PCB-traced nets (from `doc/Midex hardware components.md`) — reshape the suspects

The r1 board tracing changes the picture materially:

- **`/IOW` (ST16C454 pin 18) = EZ-USB PC6/WR# = PAL16V8 `/OE`** — all **one net**.
  So WR# both strobes the UART write *and* gates the PAL's output-enable.
- **`/IOR` (ST16C454 pin 52) = EZ-USB PC7/RD#** — one net.
- **UART data bus is DIRECT to the EZ-USB** (D1=pin67↔EZ-USB D1, D2=pin68↔EZ-USB
  D2). The three 74HC245s are therefore almost certainly on the **SRAM** bus, not
  the UART data path → **the "245 direction" hypothesis does not apply to the UART
  write** (no buffer to mis-direct between EZ-USB and UART data pins).
- ST16C454 is in **16 mode** (pin 31 strap): per-channel selects **−CSA=16,
  −CSB=20, −CSC=50, −CSD=54**; A0–A2 (pins 34/33/32) = register offset. The
  74HC138 decodes 8051 A3–A5 → 8 channel selects (Y0–3 = chip-1 A–D, Y4–7 =
  chip-2 A–D). **Port 8 (0x4078, A3–A5=111) → 138 Y7 → chip-2 −CSD (pin 54).**

Note the **PAL16V8 `/OE` is tied to the WR# net**, so the PAL actively drives its
outputs *only during writes*. If a PAL output sits on the data bus, that makes the
**data path** (not the chip-select) the write-specific suspect — see the corrected
DMM-result conclusion below, which (after measuring the non-mux bus + constant 138
enables) rules the chip-select **out** and points downstream to data-bus integrity.

### Leading remaining suspects (hardware-level)

The fault is between the AN2131 pins and the UART, in passive glue the firmware
can't see: the **74HC245 data-bus buffer direction/enable**, the
**74HC138 + PAL16V8 chip-select decode** (does it qualify writes?), or the
**`0xFE00` glue latch** (write-only — our write to it also fails, so its state
under our firmware differs from stock's). Hypothesis #10 (ISR-only failure) is
now **eliminated** (2026-06-02): the deferred-to-main-loop build still fails
`diag.py` §C identically, so context is not a factor. Every firmware variable is
exhausted — the DMM procedure below is the next step.

## DMM RESULT (2026-06-02) — strobe + chip-select OK; fault is DOWNSTREAM ★

Measured on a 3.3 V bus (Vcc ≈ 3.2 V), DMM in DC volts. Probing fine-pitch SOIC
pins of fast (MHz), averaged strobes — **individual pin voltages are noisy and
several readings contradict each other**; only the architecturally-consistent
facts below are trusted.

**ST16C454** (−IOW=18, −IOR=52, −CSD=54), hammer 0x407F:

| pin | signal | hammer READ | hammer WRITE | trusted reading |
|-----|--------|-------------|--------------|-----------------|
| 18  | −IOW   | 3.2 (idle)  | 3.08 (**pulsing ~0.12 V**) | **WR# strobe reaches the UART** ✅ |
| 52  | −IOR   | 3.1 (pulsing ~0.1 V) | 3.2 (idle) | RD# strobe reaches the UART ✅ |
| 54  | −CSD   | 1.5 / 3.1 (chip-dependent) | 2.7 | noisy — see below |

**74HC138** (enables 4=/G2A, 5=/G2B, 6=G1; outputs 7=Y7…15=Y0):

| pin | READ | WRITE | reading |
|-----|------|-------|---------|
| 4 (/G2A) | 0 | 0 | **enabled, both** ✅ |
| 5 (/G2B) | 0 | 0 | **enabled, both** ✅ |
| 6 (G1)   | 3.1 | 3.1 | **enabled, both** ✅ |
| 12 (Y3)  | 1.5 | 2.7 | (noisy) |
| 11 (Y4)  | 3.2 | 1.8 | (noisy) |

**Two hard conclusions:**

1. **The write strobe reaches the UART** — −IOW (pin 18) pulses on writes at the
   same ~0.1 V average dip that −IOR shows on working reads.
2. **The chip-select cannot be the problem.** The AN2131 bus is **non-multiplexed**
   (TRM: separate 16-bit address bus "driven at all times" + 8-bit data bus; the
   board has no octal address latch, only a dual-FF — consistent). So the 138's
   address inputs are *always valid*, and its enables (pins 4/5/6) measured
   **identical for reads and writes**. A purely combinational '138 with identical
   enables and identical address therefore drives **identical −CS for reads and
   writes.** The "−CS asserts on read, not write" (1.5→2.7) and the Y3/Y4 wobble
   are **DMM artifacts**, not a real decode asymmetry. *(This retracts the earlier
   "chip-select suppressed on writes" conclusion.)*

**So: WR# strobe present, −CS present, address valid — yet the write doesn't
latch.** The fault is **downstream of selection**, in one of:

- **(a) Data-bus integrity at the UART.** Only D1/D2 were traced "direct to
  EZ-USB"; the rest of D0–D7 may pass a **74HC245** whose direction is wrong for
  writes (stuck in UART→CPU), or the **PAL16V8 (its `/OE` = WR#, so it actively
  drives only during writes)** may **contend** on the data bus during the write.
  Either way the UART would see invalid data at the −IOW edge → no latch. *This is
  the new prime suspect.*
- **(b) −CS / −IOW / data timing-overlap** the DMM cannot resolve (averaging hides
  whether the strobe falls while −CS and data are simultaneously valid). The
  **74HC123 monostable** could be reshaping the write timing.

**Next measurement — data-bus integrity (high-contrast, DMM-friendly):** the write
hammer drives alternating **0x55 / 0xAA**, so every data bit toggles. During
`hammer.py write 0x407F`, measure the **UART data pins (D1=pin 67, D2=pin 68)**:
- **≈ half-Vcc (toggling)** → clean data reaches the UART ⇒ fault is timing-overlap
  (b) ⇒ scope/LA territory.
- **stuck at a rail, or odd intermediate** → data is **not** arriving / is contended
  (a) ⇒ chase the 74HC245 direction and the PAL outputs on the data net.

If (a)/(b) stay ambiguous on the DMM, a **scope/logic-analyzer** on {−IOW, −CSD,
D0–D7} during one write is the definitive tool (the plan's declined escape hatch);
the DMM has taken us as far as averaged DC can.

### Data-bus result (2026-06-02): data DOES reach the UART

Write-hammer 0x407F (data alternates 0x55/0xAA), DMM on both ST16C454s' data pins:
**D1 (pin67) ≈ 1.9 V, D2 (pin68) ≈ 1.2 V, others similar — mid-rail = swinging.**
So write data physically reaches the UART data pins; option (a) "data not arriving"
is **out**. Strobe + select + data are all present, yet no latch.

### ★ Leading hypothesis now: the UART is HELD IN RESET (pin 37)

ST16C454 **RESET = pin 37, active HIGH in 16 mode** ("logic 1 resets the internal
registers"; datasheet ties register R/W to no clock — XTAL1=pin35 is baud-gen
only, so the clock is not the issue). A chip held in reset explains **every**
symptom at once: writes never stick (reset overrides), reads return the
**reset-default** values we see (`LSR=0x60`, `LCR=0x00`), and strobe/−CS/data
reaching the chip are simply ignored. This reframes the stock startup write
**`0xFE00 = 0xC9`** (role was "❓ reset strobe / mode latch"): if a `0xFE00`-latch
bit **de-asserts the UART RESET** and our `0xFE00` write doesn't take effect, the
UARTs stay in reset.

**Decisive static test (DMM, trivial):** measure **pin 37** under our probe vs under
stock fw (`modprobe snd_usb_midex` auto-uploads stock → 0x1001). Probe HIGH +
stock LOW ⇒ confirmed; trace what drives pin 37 (likely the `0xFE00` latch / a glue
output) and make our firmware de-assert it. Both LOW ⇒ reset isn't it.

### ★★ RESET ruled out + `0xFE00` is SRAM, not a latch — BISECT the bus (2026-06-02)

> ❌ **The "UART RESET ruled out" bullet below is WRONG — see RESOLVED at top.**
> RESET was the cause. The "HIGH under both" DMM reading was taken before any
> host START (`0xFD`) command, and stock only drives PB4 low *after* START
> (`OEB.4`), so both firmwares showed the pulled-up default at sample time. The
> rest of this entry (`0xFE00` = SRAM, the SRAM-vs-UART bisect) remains correct.

- **UART RESET ruled out.** ST16C454 pin 37 (RESET) wires to **AN2131 PB4** and reads
  **HIGH under BOTH our probe and stock fw**. Stock works with it high ⇒ high is the
  normal state ⇒ reset is not the differentiator. (Both firmwares leave PB4 as a
  pulled-high input; neither drives it.)
- **`0xFE00`–`0xFE02` is external SRAM, NOT a glue latch.** `ep4out_process`
  (CODE:0d17) **reads** `DAT_EXTMEM_fe02` (`if (fe02 != 0) … fe01 = fe02`), so the
  region is readable RAM the firmware uses for bookkeeping — the **CY62256 32 KB
  SRAM** (≈0x8000–0xFFFF; doc already had `0xFF00–0xFF2F` as scratch). The earlier
  "glue/control latch" reading is **retracted**. `0xFE00` writes are NOT load-bearing
  for the UART. (`0xFE00`=0xC9 init + ep4out fe01/fe02 are EEPROM-write bookkeeping.)
- **Consequence — stock provably writes external memory** (it reads back `0xFE02`).
  So external writes are **not globally dead**. The sharp question is now:

  > Do **external SRAM** writes land while only **UART (0x40xx)** writes fail?

  `host/map_writes.py` tests internal XRAM (control), SRAM (0x8000/0xC000/0xFE10/
  0xFF40), and UART ports (0x4040/0x405F/0x407F), all via the existing read/write
  vendor commands — **pure software, no probing.**
  - **SRAM OK + UART FAIL** ⇒ fault is **specific to the 0x40xx UART decode** (the
    138/PAL chip-select path for *writes*), not the bus in general → narrow, likely
    addressable target.
  - **SRAM also FAIL** ⇒ the external **write path** is broadly broken for us though
    not for stock → deepest case; revisit identical-cycle assumption / needs scope.

#### RESULT (2026-06-02): SRAM writes LAND, UART writes FAIL — fault is UART-decode-specific ★★★

`map_writes.py`: internal XRAM **OK**; ext SRAM 0x8000/0xC000/0xFE10/0xFF40 **all
OK**; UART 0x405F/0x407F SCR **FAIL** (0x4040 reads 0x00 = RHR/THR are separate
regs, not a valid R/W cell). **So the external bus, WR# strobe, data bus and
address bus are all good — only the 0x40xx UART window fails, and only for writes.**
This kills every "general bus" theory.

**Stock's UART write = plain `MOVX` to 0x40xx** (verified in `uart_init_all_ports`
@0x1177: writes 0x43=0x83, 0x40=1, 0x41=0, 0x43=3, 0x44=0; and TX writes THR at
0x40+port*8). Identical to our probe — no companion access, no special opcode.
Init wraps the writes in `TR2=0 … TR2=1` (stop the 500 kHz XIN while reprogramming
the divisor), but **TX runs with Timer2 ON**, so the clock state does not gate
writes. ⇒ stock and probe issue the *same* `MOVX`, yet stock latches and we don't.

**Mode resolved — 16/Intel (68-mode theory RETRACTED).** pin 31 (16/−68 strap)
measures **3.2 V HIGH on both ST16C454s ⇒ 16/Intel mode** (the noted 2.7 kΩ
pull-down is overridden). So pin 18 = a genuine **−IOW write strobe** (not Motorola
R/‑W), pin 52 = −IOR, per-channel −CS, RESET active-high. pin 35 (XIN) ≈ **1.3 V =
the 500 kHz clock is present/toggling** → no Timer2 bug. (Aside: RESET pin37 reads
HIGH in 16 mode yet stock works with it high, so high is empirically the operational
state here — ruled out as the differentiator regardless of datasheet polarity.)

**THE PARADOX (state at end of 2026-06-02 session).** In 16 mode: −IOW reaches the
UART, −CS is generated, data is valid at the pins, clock present, reset same as
stock. **SRAM writes via the identical `MOVX`/vendor-command path land**, proving
bus + opcode + firmware path are all good. **Only 0x40xx writes fail; 0x40xx reads
work.** The fault is therefore 100% in the **0x40xx write-decode glue
(PAL16V8 + 74HC138 (+74HC123/74))** — which is fixed hardware, identical for stock
and our probe. Stock writes that window with a bare `MOVX` and it latches; ours
doesn't. Every firmware/electrical input we can measure is identical, so the
remaining difference is a **timing/analog detail invisible to a DMM**.

**Two candidate mechanisms for a scope to disambiguate:**
- **(a) −CS contention/qualification:** the PAL's `/OE` = WR#, so the PAL drives its
  outputs *only during writes*. If a PAL output sits on the UART −CS net (or the
  138 enable), it contends with / overrides the 138 during writes → −CS not validly
  low at the −IOW rising edge → no latch. (The DMM −CS readings 1.5 V on reads vs
  2.7 V on writes are *consistent* with this, if not noise.)
- **(b) −CS propagation delay:** −CS through the PAL+138 chain settles too late
  relative to the ~168 ns −IOW pulse for the write-latch edge; reads (data valid
  whenever −CS+−IOR overlap) are more forgiving than the write's edge-latch.

### NEXT SESSION — scope/logic-analyzer plan (the decisive test)

Trigger on the −IOW falling edge at ST16C454 **pin 18**; capture simultaneously:
- **−IOW (pin 18)**, **−CS** of the addressed channel (e.g. pin 54 for port 8),
  **−IOR (pin 52)**, and **D0–D7**.
Do it for **(1) our probe writing 0x407F** (`hammer.py write 0x407F`) and
**(2) stock firmware sending MIDI out** that port (flood MIDI for a steady trace).
**Compare −CS vs −IOW overlap and the data validity at the −IOW rising edge between
the two.** The difference there is the root cause — whether −CS is contended/late
(→ glue rework / a board mod) or a setup/hold the firmware could dodge.

Pin map for the scope session: ST16C454 −IOW=18, −IOR=52, −CSA=16/−CSB=20/−CSC=50/
−CSD=54, A0–A2=34/33/32, RESET=37 (=AN2131 PB4), XTAL1=35; 74HC138 enables pin4/5/6,
outputs 7(Y7)…15(Y0); PAL16V8 `/OE` = the WR#/−IOW net.

### Firmware experiment: MOVX cycle-stretch sweep (no scope)

Rebuilt `midex-probe-r1.ihx` now also: clears **CLK24OE** in `board_init`
(CPUCS=0, exact stock match — rules out a CLKOUT-clocked PAL state diff), and adds
**VR_SET_CKCON (0xB4)** so the host can set the MOVX stretch live. `host/stretch_sweep.py`
walks CKCON bits 2:0 = 0..7 and re-runs the 0x407F write test for each. Any pass ⇒
the board needs a wider/later write strobe than stock's stretch=0 (workaround +
clue). All fail ⇒ strobe width is not the issue.

**RESULT (2026-06-02): all 8 stretch values FAIL** (XRAM control passes). Strobe
**width is ruled out** — the fault is structural (CS/−IOW/data timing-overlap or
the 74HC123 one-shot), or per the bisect above, UART-decode-specific.

## Multimeter procedure (reference — initial bring-up)

A DMM cannot catch a 168 ns pulse, but it **can** read (a) static levels and
(b) the *average* of a continuous strobe. The probe firmware has **hammer
modes** for exactly this — they loop forever (power-cycle to stop):

```
sudo python3 host/hammer.py write 0x407F   # continuous WRITES to port-8 UART
sudo python3 host/hammer.py read  0x407F   # continuous READS  (known-good control)
sudo python3 host/hammer.py write 0x2300   # continuous writes to INTERNAL RAM (control)
```

Average-voltage reading: a 50% square wave reads ~half-rail (≈2.5 V on a 5 V
part). A steady strobe that's mostly high reads a bit under Vcc. "Pulsing" =
reads measurably below Vcc; "idle/dead" = sits at Vcc (or GND).

Key pin references (verify exact pins against the chip + the AN2131 TRM pinout):
- **AN2131**: `PC6 = WR#`, `PC7 = RD#`, `PB7 = T2OUT` (the 500 kHz UART clock).
- **74HC245** (data buffer, 20-pin): pin 1 = `DIR`, pin 19 = `/OE`,
  pins 2–9 = A1–A8, pins 18–11 = B1–B8, pin 10 = GND, pin 20 = Vcc.
- **74HC138** (decoder, 16-pin): pin 6 = `G1`, pin 4 = `/G2A`, pin 5 = `/G2B`
  (enables), pins 1–3 = A/B/C selects, pins 15,14,…7,9 = Y0–Y7.
- **ST16C454**: per its datasheet — note its `/WR`, `/RD`, `/CS`, `D0–D7` pins.

### A) Disconnected (power OFF, continuity / diode mode)

1. Identify the 74HC245, 74HC138, PAL16V8 (label "GAL"/"PAL"), and the data
   latch near the bus (likely 74HC273/373/374 → the `0xFE00` latch).
2. **74HC245 pin 1 (DIR)** — trace what drives it: continuity to AN2131 `RD#`?
   `WR#`? a PAL output? Vcc/GND? a latch output? *This is the prime question —
   what flips the data buffer to "CPU→UART" for writes.*
3. **74HC245 pin 19 (/OE)** — GND (always enabled) or a decode/CS signal?
4. **AN2131 `WR#` (PC6)** — trace to the 74HC138/PAL enables and to the
   ST16C454 `/WR` pins. Confirm WR# actually reaches the write-strobe path
   (and isn't left unrouted / pulled off).
5. Map the **`0xFE00` latch** outputs — do any go to the 245 `DIR`/`/OE`, a UART
   `/CS`, or a UART reset? (Bits of the `0xC9` stock writes.)
6. Continuity-check the data path: AN2131 `D0–D7` ↔ 245 A-side ↔ 245 B-side ↔
   ST16C454 `D0–D7`.

### B) Connected — PROBE firmware (`0x10C0`)

Idle (no hammer):
- `PB7` (T2OUT) average ≈ half-rail ⇒ the 500 kHz UART clock is running. If it
  sits at 0/Vcc, the Timer2 clock isn't reaching PB7 (a *separate* TX problem).
- `WR#`(PC6) and `RD#`(PC7) ≈ Vcc (idle high).
- Note 245 `DIR`, `/OE`, and the `0xFE00` latch output voltages.

Hammer **write** `0x407F`, then measure (power-cycle after):
- `WR#`(PC6): should drop below Vcc → AN2131 *is* emitting WR#. (If not, the
  problem is upstream in the EZ-USB.)
- **ST16C454 `/WR` pin (port-8 UART): does it also drop?** This is the decisive
  read — `/WR` pulsing ⇒ strobe reaches the chip (look at data/direction next);
  `/WR` stuck at Vcc ⇒ the strobe dies in the decode/245/latch.
- 245 `DIR`: does it move vs idle (selecting CPU→UART)?
- UART-side data lines: activity (~half-rail) ⇒ the 245 is passing CPU data.

Controls:
- Hammer **read** `0x407F` (reads work): `RD#` and the UART `/RD` should pulse,
  data lines toggle, `DIR` opposite of the write case. This is the "good"
  reference to compare the write case against.
- Hammer **write** `0x2300` (internal): `WR#`(PC6) should *not* pulse (internal
  access, no external strobe) — confirms your meter/probe placement.

### C) Connected — STOCK firmware (`0x1001`) for comparison

- Repeat the idle measurements; note any difference in 245 `DIR`/`/OE` and the
  `0xFE00` latch outputs vs the probe firmware.
- Send MIDI out a port and measure that UART's `/WR` average — it should pulse.
  **Comparing the same `/WR` pin under (probe hammer-write, failing) vs (stock
  MIDI-out, working) is the single most decisive test:**
  - pulses under stock but not our hammer ⇒ strobe/decode/245 state differs
    under our firmware (look hardest at the `0xFE00` latch and 245 control);
  - pulses under both ⇒ the UART gets the strobe but doesn't latch ⇒ data-bus
    direction/contention or a data setup/hold issue.

## If the multimeter can't resolve it

A logic analyzer / scope on `WR#`, the UART `/WR`/`/CS`, the 245 `DIR`/`/OE`,
and `D0–D7` during a single hammered write is the definitive tool (the plan's
declined escape hatch). The bus-probe has already de-risked the rest: the read
map is confirmed and the firmware is known-good up to this one strobe-delivery
question.

## Phase-3 sequel: the write glue is also marginal AT POWER-ON (2026-06-06)

A second, related symptom surfaced in the Phase-3 class firmware (8-port init):
**exactly one channel would come up with its `LCR` stuck at the reset default
`0x00` (5-bit) instead of `0x03`**, corrupting that port (`0x90`→`0x10`). The
victim was deterministic for a given build but *moved* between builds (port 2 →
3 → 1) as the init code/timing changed, and read-back retry could not repair it
(re-running the identical sequence re-hit the identical bad window). On-device
`LCR` readback confirmed the value; the bus-probe round-tripped the same channel
perfectly. ⇒ the same marginal `0x40xx` write glue (PAL16V8 + 74HC138 +
74HC123), this time failing **only when written right at power-on**.

The tell: every *working* reference configures the UARTs **late**, never at boot
— stock runs `uart_init_all_ports` at the host **START (0xFD)** command (long
after enumeration), and the bus-probe is driven from the host over EP0 well
after boot. **Fix (firmware/main.c):** `board_init` now leaves the ST16C454s in
RESET (PB4 high-Z) through power-on and USB enumeration; `uart_bringup()` then
waits `BOARD_UART_BRINGUP_DELAY_MS` (100 ms), drives PB4 low, and runs
`uart_init`. With the deferred bring-up, all 8 channels latch `LCR=0x03` on the
first pass and every port round-trips. Matching stock's exact `TR2` clock-stop
init sequence at boot did **not** help — it is the *timing relative to power-on*,
not the clock state during the writes.
