# MIDEX hardware register map (RE findings)

Single source of truth for the device-specific XDATA / register interface of the
Steinberg MIDEX family, recovered by **static Ghidra disassembly** of the stock
proprietary firmware images in [`doc/firmware/`](../../../doc/firmware/). The
firmware project's `board_r1.h` / `board_r2.h` are derived *from this document*.

Companion docs: [`doc/analysis.md`](../../../doc/analysis.md) (USB wire protocol),
[`doc/firmware_upload_process.md`](../../../doc/firmware_upload_process.md) (RAM
download path), [`doc/Midex hardware components.md`](../../../doc/Midex%20hardware%20components.md)
(PCB BOM).

**Confidence legend:** ✅ confirmed in disassembly · ⚠️ inferred, needs hardware
confirmation (bus-probe / scope) · ❓ open question.

Ghidra import language for all three images: `8051:BE:16:default` (Ghidra's 8051
is big-endian). Addresses below are CODE-space unless prefixed `XDATA:`.

---

## MIDEX8 r1 — AN2131 + 2× ST16C454 (8 ports) — PRIMARY TARGET

Firmware: `doc/firmware/midex8_firmware_combined.bin` (6647 bytes, PID `0x1001`).
Reset vector `0x0000 → LJMP 0x01E3` (`fw_entry`).

> **Hardware-confirmation status (bus-probe): READ *and* WRITE confirmed ✅
> (2026-06-05).** The EP0 MOVX probe reads back real 16550 register values at
> `0x4040 + port*8`, **and** — after the bring-up gap was found — external writes
> now land: UART register writes stick and a full **MIDI loopback** (cable OUT→IN)
> round-trips bytes on both port 1 and port 8. The missing step was driving
> **AN2131 PB4 low** (`OEB.4`) to de-assert the **ST16C454 RESET** (active-high);
> the probe had left PB4 high-Z. See §2.5 and the full resolution in
> [bus_write_debug.md](bus_write_debug.md). Channel↔port mapping confirmed
> **linear**: `port N (1-based) = 0x4040 + (N-1)*8`.

### 1. External UART bank — the load-bearing finding ✅

The eight MIDI ports are eight **16550-class UART channels** (2× ST16C454 quad
UART) memory-mapped in the 8051 external XDATA space:

```
UART[port].base  = 0x4040 + port*8          port = 0..7
                   0x4040 0x4048 0x4050 0x4058 0x4060 0x4068 0x4070 0x4078
register address = base + offset             standard 16550 datasheet offsets
```

Address-bit decode (drives the 74HC138 + PAL16V8 chip-select logic on the PCB):

| 8051 addr bits | meaning |
|----------------|---------|
| A15..A8 = 0x40 | DPH constant — selects the UART bank window 0x4000–0x40FF |
| A6  (0x40)     | UART-bank enable (always set on every access) |
| A5..A3         | port select (0..7) → `port << 3` |
| A2..A0         | 16550 register offset (0..7) |

16550 register offsets used by the firmware (standard datasheet layout):

| Offset | DLAB=0 | DLAB=1 | Used as |
|--------|--------|--------|---------|
| 0 | RHR / THR | DLL | RX read, TX write, divisor-low |
| 1 | IER       | DLM | divisor-high (IER left 0 — interrupts unused) |
| 3 | LCR       |     | line control |
| 4 | MCR       |     | modem control |
| 5 | LSR       |     | line status (polled) |

> Note: Ghidra renders the `port<<3` index as a 3-step rotate expression
> `((p<<1|p>>7)<<1|…)`; for `p∈0..7` this is exactly `p<<3`. Verified by
> reconciling the RX poller (`0x0AAA`) against the init loop (`0x1177`).

### 2. UART init sequence ✅ — `FUN_CODE_1177` @ `0x1177`

Loops 8 ports (`offset` stepping 0,8,…,56), writing each channel:

```
LCR (off3) = 0x83   ; DLAB=1, 8 data bits, 1 stop, no parity
DLL (off0) = 0x01   ; divisor low  = 1
DLM (off1) = 0x00   ; divisor high = 0   -> divisor = 1
LCR (off3) = 0x03   ; DLAB=0, 8N1
MCR (off4) = 0x00   ; modem control cleared
```

- **Divisor = 1.** 16550 baud = `f_uart_in / (16 × divisor)`. For MIDI 31250 baud
  this implies a **UART input (XIN) clock of 31250 × 16 = 500 kHz**. ⚠️ The PCB
  oscillator/clock source is not listed in the BOM doc — confirm 500 kHz on
  hardware (the bus-probe can read back a known byte to validate framing).
- **FIFOs are NOT enabled** (no write to FCR / offset 2): stock firmware runs the
  UARTs in 16450 (single-byte) mode. Our class firmware *may* enable the 16-byte
  FIFOs (write FCR=0x07) — an improvement, not a requirement.
- `TR2` (timer2) is stopped around the init and a short settle delay follows.

### 2.5. External-bus + UART-clock preconditions ✅ (found via bus-probe)

The static map gave the right UART addresses but missed the **bring-up the bus
needs before any UART access works**. A minimal EZ-USB firmware that just MOVXes
to `0x4078` gets *bus ghosting* — every read returns the last value driven on the
data bus — because the strobes and clock are off. `fw_entry` programs these; our
firmware must too:

| Register | Write | Why |
|----------|-------|-----|
| `PORTCCFG` (`0x7F95`) | `\|= 0xC0` | bit7=RD#, bit6=WR#. Switches PC7/PC6 from GPIO to the **external-memory read/write strobes**. Without this the UART is addressed but never latched. **This is the precondition that was missing.** |
| `PORTBCFG` (`0x7F94`) | `\|= 0x80` | bit7=T2OUT. Routes **Timer2 clock-out to PB7**, which is the ST16C454 **XIN clock**. |
| `RCAP2L/H` (`0xCA/0xCB`) | `0xFE / 0xFF` | Timer2 auto-reload = `0xFFFE` (2 counts). |
| `CKCON` (`0x8E`) | `&= ~0x20` | T2M=0 → Timer2 input = Fosc/12 = 2 MHz. |
| `T2CON` (`0xC8`) | `0x00`, then `TR2=1` | Run Timer2. 2 MHz / 2 = 1 MHz overflow → T2OUT toggles at **500 kHz**. |
| `PORTBCFG` (`0x7F94`) | `&= ~0x10` | bit4=0 → **PB4 = GPIO** (not INT4 alt-fn). |
| `OUTB` (`0x7F97`) | `&= ~0x10` | PB4 output latch = **0**. |
| `OEB` (`0x7F9D`) | `\|= 0x10` | **PB4 = driven output → ST16C454 RESET (active-high) de-asserted.** ✅ **This was the missing precondition** — without it the UART sits in reset, ignores writes (readback `0xFF`) and reads its reset-default registers. Stock sets `OEB.4` on the host START (`0xFD`) command, just before `uart_init_all_ports`; releases it (`OEB &= ~0x10`) on STOP (`0xF5`). |
| `0xFE00` | `0xC9` (then `0xFE02=0`, `0xFE01=0x10`) | **NOT a UART latch** — this is external **SRAM** (CY62256) the fw uses for bookkeeping (`ep4out_process` reads back `0xFE02`); not load-bearing for the UART. |

This **confirms the divisor-1 / 500 kHz finding**: 500 kHz XIN ÷ (16 × 1) = 31250
baud, and the 500 kHz is generated on-chip by Timer2→PB7, not an external
crystal. The Timer2 SFRs are not XDATA, so this must live in firmware (the host
bus-probe cannot reach SFRs).

> **PB4 = UART RESET (PCB-traced + fw-confirmed + loopback-validated).** PALLV16
> pin 6 → ST16C454 pin 37 (RESET) → AN2131 PB4. The ST16C454 RESET is **active
> high**, so PB4 must be **driven low** for the chip to operate. This single step
> turned the long-standing "external writes never land" failure (see
> [bus_write_debug.md](bus_write_debug.md)) into a working MIDI loopback.
>
> **⚠ TIMING (Phase-3, 2026-06-06): release RESET + run `uart_init` LATE, not at
> `board_init`.** Stock drives `OEB.4` and calls `uart_init_all_ports` only on the
> host START (`0xFD`) cmd — i.e. **after** USB enumeration — keeping the UARTs in
> reset (PB4 high-Z) from power-on until then. That lateness is load-bearing: the
> `0x40xx` write glue (PAL16V8/74HC138/74HC123) is **marginal right at power-on**,
> where a channel intermittently fails to latch its `LCR` (comes up `0x00` = 5-bit,
> corrupting that port, `0x90`→`0x10`). The first class build did
> `PORTBCFG`/`OUTB`/`OEB` **and** `uart_init` at `board_init` and hit exactly that
> (one dead channel, victim moving with code timing; retry/verify and stock's
> `TR2` clock-stop did **not** help). **Fix:** `board_init` does only
> `PORTBCFG.4=0`/`OUTB.4=0` (PB4 stays high-Z = in reset); a deferred
> `uart_bringup()` (after USB up + `BOARD_UART_BRINGUP_DELAY_MS`=100 ms) sets
> `OEB.4` then runs `uart_init`. All 8 channels then latch on the first pass.
> Full chase: [phase3_build.md](phase3_build.md) + [bus_write_debug.md](bus_write_debug.md).

### 3. MIDI data path ✅

**TX (host → instrument)** — polled in the main loop (`fw_entry`, per-port
dispatch around `0x03E3`–`0x05B1`):
```
if (LSR[port] & 0x20)           ; bit5 THRE = transmit holding reg empty
    THR[port] = midi_byte       ; write 0x4040 + port*8
```
Source bytes are the host's EP4-OUT MIDI stream parsed into per-port INTMEM ring
buffers.

**RX (instrument → host)** — `FUN_CODE_0AAA` @ `0x0AAA` (Timer1 ISR), sweeps all
8 ports each tick:
```
if (LSR[port] & 0x01)           ; bit0 DR = data ready
    b = RHR[port]               ; read 0x4040 + port*8
    push b into INTMEM 0x90xx ring buffer (per port)
```
The main loop then timestamps/formats those bytes into the proprietary EP2-IN
report (`0x0D84 → 0x0DE3 → 0x0F61/0x0FB2 → 0x0140B`) and the EP2-IN handler
`FUN_CODE_0693` copies them to the endpoint buffer. **Our class firmware discards
this whole timestamp/format layer** and emits plain 4-byte USB-MIDI packets.

### 3.5. Port A = per-port RX interrupt lines — wired but unused by stock ✅

PCB-traced (r1) and confirmed against the firmware: each ST16C454 channel's
active interrupt output (16/Intel mode → one INT pin per channel, INTA–INTD)
is wired to a bit of the AN2131 **Port A**:

```
PA0  PA1  PA2  PA3   = chip-1  INTA INTB INTC INTD   (MIDI ports 0..3)
PA4  PA5  PA6  PA7   = chip-2  INTA INTB INTC INTD   (MIDI ports 4..7)
```

i.e. **`PINSA` bit N = RX/IRQ status of MIDI port N**. This lines up with the
'138 chip-select decode (addr A3–A5 = port select) and the firmware's own port
index (`0x4040 + port*8`). Traced lines that confirm the mapping: chip-1 INTC→PA2,
chip-1 INTD→PA3, chip-2 INTD→PA7 — all exactly where this table predicts.

**Stock firmware ignores this path entirely.** A whole-image search finds *no*
access to `PORTACFG (0x7F93)`, `PINSA (0x7F99)`, or `OEA (0x7F9C)`, and the UART
init never enables `IER` (stays 0), so the ST16C454 INT pins are not even driven.
Stock RX is pure LSR-bit0 polling in the Timer1 ISR (§3 above). Port A is left in
its reset default (input), so the lines sit dormant.

> **Class-firmware opportunity:** enable `IER` bit0 (ERBFI) per channel, leave
> Port A as input (no `OEA`/`PORTACFG` write needed — reset default), then read
> `PINSA (0x7F99)` *once* per poll to get an 8-bit "which ports have RX data"
> bitmap — replacing 8 LSR `MOVX` reads with 1 in the idle case. Verify the
> INTSEL strap (pin 65) / INT polarity to read the bitmap the right way round.

### 4. EZ-USB (AN2131) register & endpoint-buffer usage ✅

| XDATA addr | name | usage in firmware |
|------------|------|-------------------|
| `0x7F92` | CPUCS | reset/8051-control (also the firmware-download target) |
| `0x7F95`,`0x7F94`,`0x7F97`,`0x7FA1`,`0x7FAC`,`0x7FAE`,`0x7FAF` | USB/port cfg | enumeration + I/O config in `fw_entry` |
| `0x7FA5` / `0x7FA6` | I2CS / I2DAT | I²C EEPROM access (`FUN_CODE_14C1/14C9/14D1/1462/1491`) |
| `0x7FB8/7FB9` | IN2CS / IN2BC | EP2-IN (MIDI-in) control + byte count |
| `0x7FC0/7FC1` | OUT2CS / OUT2BC | EP2-OUT (timing/start-stop) |
| `0x7FC8/7FC9` | OUT6CS / OUT6BC | EP6-OUT (LED commands) |
| `0x7FCC/7FCD` | — | (r2 uses a second OUT EP here; r1 leaves it) |
| `0x7FD0/7FD1` | OUT4CS / OUT4BC | EP4-OUT (MIDI-out) |
| `0x7FA9/7FAA` | IN/OUT arming | endpoint arm registers |
| `0x7E00` | EP2-IN buffer | RX→host bytes copied here, count→`0x7FB9` |
| `0x7BC0` | EP4-OUT buffer | host MIDI-out read from here |
| `0x7DC0` | EP6-OUT buffer | host LED commands read from here |
| `0x7C00` | EP-IN buffer  | EP6-IN reply staging |

The download/CPUCS path (`0x7F92`, vendor request `0xA0`) is already proven —
see `doc/firmware_upload_process.md`. Our firmware keeps the skeleton's RENUM
re-enumeration and SUDPTR descriptor delivery; only the descriptor *content* and
the EP data handling change.

### 5. Glue latch & scratch RAM ⚠️/❓

| XDATA addr | observation | purpose |
|------------|-------------|---------|
| `0x4045 + p*8` (LSR) bit5/bit0 | polled for TX/RX | (part of UART, see above) |
| `0x405e` (port-3 MSR, bit5) | read in vendor cmd `0x80` (`0x07FB`) and `0x0835` | a front-panel switch / cable-detect input ❓ |
| `0xFE00` | one-shot write `0xC9` at startup (`0x035A`); `0xFE01`=param, `0xFE02`=0 | glue/control latch (reset strobe or mode latch) ❓ |
| `0xFF00..0xFF2F` | per-port MIDI-in formatting state, zeroed by `FUN_CODE_121B` | scratch RAM (5×8-byte blocks) |
| `0x0170/0x0171` | timing constants in `0x0835` | scratch |

- **LEDs:** no dedicated LED latch was positively identified on r1 in this pass
  (the proprietary firmware drives LEDs via the EP6 command stream, not a simple
  XDATA latch). LEDs are **deferred** per the plan — leave dark. Revisit `0xFE00`
  and the EP6 path in the cosmetic pass.

### 6. Interrupt vector table ✅

| Vector | addr | handler | role |
|--------|------|---------|------|
| reset  | `0x0000` | `0x01E3` `fw_entry` | init + main loop |
| Timer0 | `0x000B` | `0x0A5A` | 16-bit MIDI timestamp counter + fractional accumulator |
| Timer1 | `0x001B` | `0x0AAA` | **UART RX poller (all 8 ports)** |
| Serial1| `0x0033` | `0x0B17` | clears CCON bit (minimal) |
| USB    | `0x0043` | `0x0100` | EZ-USB autovector (SETUP / EP service) |
| INT3/I²C | `0x004B` | `0x137F` | I²C completion |

Timer0 reload `0x38` mode 2; Timer1 reload `0x46` mode 2; Timer2 reload
`RCAP2 = 0xFFFE` (the 25.6 ms / timing base). These belong to the proprietary
timing scheme and are **not needed** by the class firmware.

---

## MIDEX8 r2 — CY7C646 (FX) + ST16C454 + ST16C452 — Phase 5 (stub)

Firmware: `doc/firmware/midex8r2_combined.bin` (not yet disassembled in depth).
**MIDEX8 r2 has 8 ports** (same as r1), but the BOM
([`Midex hardware components.md`](../../../doc/Midex%20hardware%20components.md))
lists only **1× ST16C454 (4) + 1× ST16C452 (2) = 6 external UART channels** — so
**the last 2 ports must be driven some other way. ❓ Leading hypothesis: the
EZ-USB FX (CY7C646) on-chip serial** (the FX has on-chip UARTs, the same kind
MIDEX3 uses). That would make r2 a **hybrid backend**: 6 external-16550 (MOVX,
like r1) + 2 on-chip-serial. So r2 is **not** a near-mechanical repeat of r1: its
Phase-5 delta is the external UART base window + the **extra-2-port mechanism**
(disassemble for SCON/SBUF + 2nd on-chip serial) + FX RAM ceiling `0x1B3F`.
**Note `0x7FE5` is AUTODATA — present on the AN2131 too, NOT an FX-only init
register** (re-examine what r2 actually needs). Full RE deferred to Phase 5.

---

## MIDEX3 — 3 ports — future-phase delta (scoping only, no hardware)

Firmware: `doc/firmware/midex3_combined.bin` (6419 bytes, Mac-derived).
Reset vector `0x0000 → LJMP 0x01C8` (`fw_entry`). **Statically analyzed for effort
estimation only — not on the build path.**

### Shared with MIDEX8 ✅ (most of the codebase)
- Same EZ-USB-family 8051 build and enumeration scaffolding.
- **Identical 16-bit timestamp engine** (Timer0 ISR `0x0C0D` mirrors r1's `0x0A5A`
  byte-for-byte in structure) and **identical MIDI-parser main-loop skeleton**
  (running-status / SysEx `0xF0`/`0xF7` handling, the same `0x20` busy mask).
- Same `0x____00 = 0xC9` glue-latch idiom (here at `0x1B00`, vs r1 `0xFE00`).
- → USB, descriptor, and parser layers of our class firmware are reusable as-is.

### The big divergence ⚠️ — MIDI backend is **on-chip serial, not external 16550s**
MIDEX3 has **no `0x40xx` UART window and no 16550 init** (`0x83→LCR` signature
absent). Instead it uses the MCU's **on-chip serial ports**:
- **Port 0** → on-chip UART0: `SCON = …|0x56`, `ES=1`, TX kicked via `TI=1`.
- **Port 1** → a second on-chip serial: armed via `FIFLG`/`EC=1`/`CF=1`, TX via
  the `FIFLG` bit. Two active serial ISRs — Serial0 `0x0CA4` (vec `0x23`) and
  Serial1 `0x0C69` (vec `0x33`).
- **Port 2** → toggles an I/O-port bit (`0x7F96 ^ 0x20`); likely a bit-banged /
  port-driven third channel. ❓ confirm mechanism on hardware.
- **Baud:** Timer1 mode 2, `TH1=TL1=0xF4` (reload 12), `PCON|0x80` (SMOD=1),
  `CKCON` (SFR `0x8E`) bit4 set (T1M → timer1 clock = CLK/4). With a 24 MHz core:
  `(2/32) × 24e6/(4×12) = 31250 baud` ✅ (math checks out for MIDI).

### Chip-ID note ❓
Two on-chip UARTs in use (Serial0 **and** Serial1 vectors active) is *suggestive*
of a **dual-UART FX-class part (CY7C646)** rather than the single-UART AN2131 —
behavioral evidence the plan's register-map diff couldn't provide. Not definitive;
the `0x7Fxx` maps are byte-identical between the parts, so confirm via the PCB chip
marking or loader-mode silicon ID when hardware is available.

### MIDEX3 future-phase effort estimate
Larger than an r2-style `board_*.h` addition: the external-16550 MOVX backend does
**not** apply. A MIDEX3 port needs a **separate on-chip-serial UART backend**
(SCON/SBUF + second serial + port-2 channel + timer1 baud setup) behind the same
`uart.c` interface, plus a 3-port descriptor set. USB/descriptor/parser layers are
shared. Gate on acquiring real MIDEX3 hardware (the `0x1100→0x1101` upload path is
still unverified — see `firmware_upload_process.md`).

---

## Summary table — what `board_r1.h` encodes

| Parameter | r1 value | source |
|-----------|----------|--------|
| VID | `0x0A4E` | device | 
| Port count | 8 | main loop `!= 8` |
| UART backend | external 16550 (MOVX) | `0x1177`, `0x0AAA` |
| UART base(port) | `0x4040 + port*8` (linear, port 1-based = `0x4040+(N-1)*8`) ✅ | `0x1177` + loopback |
| Reg offsets | std 16550 (THR0 IER1 LCR3 MCR4 LSR5) ✅ | `0x1177` + loopback |
| Divisor | 1 → 500 kHz XIN (Timer2→PB7) ✅ | `0x1177` + loopback (31250 baud verified) |
| RD#/WR# enable | `PORTCCFG \|= 0xC0` ✅ | bus-probe |
| **UART RESET (de-assert)** | **`PB4` low: `PORTBCFG&=~0x10; OUTB&=~0x10; OEB\|=0x10`** ✅ | `midi_in_aggregate` `0x10A8` + loopback |
| UART clock | Timer2 clock-out on PB7, 500 kHz ✅ | bus-probe + loopback |
| LSR THRE / DR bits | 0x20 / 0x01 ✅ | TX loop / `0x0AAA` + loopback |
| FIFO | disabled by stock fw (we may enable) | `0x1177` |
| CPUCS | `0x7F92` | `fw_entry` |
