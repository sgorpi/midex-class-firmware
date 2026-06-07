/*
 * board_r1.h - MIDEX8 r1 (AN2131 + 2x ST16C454) board configuration.
 *
 * Derived from static RE of doc/firmware/midex8_firmware_combined.bin.
 * See src/midex-class-firmware/doc/hardware_register_map.md for the full
 * analysis and confidence levels. Values marked CONFIRM need hardware
 * validation via the EP0 bus-probe before the Phase 2 spike is trusted.
 */
#ifndef BOARD_R1_H
#define BOARD_R1_H

/* ---- USB identity (Phase 2 spike fills descriptors) -------------------- */
#define BOARD_USB_VID        0x0A4E   /* Steinberg                         */
#define BOARD_USB_PID        0x10C1   /* fresh class-compliant PID, outside
                                       * the snd-usb-midex table so that
                                       * snd-usb-audio wins binding         */
#define BOARD_NUM_PORTS      8        /* main loop iterates ports 0..7      */

/* ---- External UART bank (16550-class, memory-mapped XDATA) ------------- */
/* UART[port] base = BOARD_UART_BASE + port * BOARD_UART_STRIDE             */
#define BOARD_UART_BACKEND_EXTERNAL_16550  1
#define BOARD_UART_BASE      0x4040   /* port 0 register block             */
#define BOARD_UART_STRIDE    0x08     /* 8 bytes per channel               */

/* Standard 16550 datasheet register offsets (added to a channel base) */
#define UART_RHR  0   /* read:  receive holding register (DLAB=0)          */
#define UART_THR  0   /* write: transmit holding register (DLAB=0)         */
#define UART_DLL  0   /* divisor latch low  (DLAB=1)                       */
#define UART_DLM  1   /* divisor latch high (DLAB=1)                       */
#define UART_IER  1   /* interrupt enable (DLAB=0) - left 0, polled        */
/* offset 2: ISR on read, no register on write. The ST16C454 is a 16C450-class
 * part with NO FIFO (datasheet Table 4: 12 registers, no FCR/IIR-FIFO). */
#define UART_LCR  3   /* line control                                      */
#define UART_MCR  4   /* modem control                                     */
#define UART_LSR  5   /* line status                                       */
#define UART_MSR  6   /* modem status                                      */
#define UART_SCR  7   /* scratch                                           */

/* LSR status bits the bridge polls */
#define UART_LSR_DR    0x01   /* data ready (RX)                           */
#define UART_LSR_THRE  0x20   /* transmit holding reg empty (TX)           */

/* ---- Line setup: 8N1, divisor 1 --------------------------------------- */
/* CONFIRMED on hardware (loopback.py): divisor 1 = 500 kHz UART XIN / 16 = */
/* 31250 baud; bytes round-trip clean over a physical MIDI OUT->IN cable.   */
#define BOARD_UART_LCR_DLAB  0x83   /* DLAB=1 | 8N1                         */
#define BOARD_UART_LCR_8N1   0x03   /* DLAB=0 | 8N1                         */
#define BOARD_UART_DIVISOR   1
/* Read-back retry cap for each LCR write in uart_init (defensive against any
 * residual marginal write; normally latches first try with the late bring-up). */
#define BOARD_UART_LCR_MAX_TRIES 50

/* Delay (ms) after enumeration before releasing UART RESET + running uart_init.
 * The 0x40xx write glue is marginal right at power-on (a channel comes up in
 * 5-bit mode); stock and the bus-probe both configure the UARTs well after
 * boot. Deferring past power-on is the actual fix. See main.c uart_bringup(). */
#define BOARD_UART_BRINGUP_DELAY_MS 100

/* ---- External-bus + UART-clock bring-up (see board_init in main.c) ----- */
/* These MUST be programmed before any UART access or reads ghost the bus.   */
#define BOARD_PORTCCFG_BUS   0xC0   /* PORTCCFG: enable RD#(b7)/WR#(b6) strobes */
#define BOARD_PORTBCFG_CLK   0x80   /* PORTBCFG: route Timer2 clock-out to PB7  */
#define BOARD_T2_RCAP2L      0xFE   /* Timer2 reload low                       */
#define BOARD_T2_RCAP2H      0xFF   /* Timer2 reload high (0xFFFE = 2 counts)  */
/* Fosc/12 = 2 MHz, /2 overflow, T2OUT toggle -> 500 kHz UART XIN.            */

/* ---- Timer0 = RX-capture tick (see uart_rx_isr + timer_isr_rx_capture_design.md) */
/* Mode 2 (8-bit auto-reload), Fosc/12 = 0.5 us/tick. Reload 0x46 = 256-186 ->
 * 186 ticks -> ~93 us hardware tick; uart_rx_isr applies a /3 software prescaler
 * so the (expensive) full 8-port LSR sweep runs every ~279 us. Runs at HIGH
 * priority (PT0) so it preempts the low-priority USB interrupt.
 *   Why the prescaler: measured on HW the 8-port sweep is ~100 us (per-port the
 *   SDCC code recomputes each channel's XDATA address: ~24 instr + 1 MOVX, x8 @
 *   0.5 us/cycle). At a flat 100 us tick that nearly saturates the CPU (round-trip
 *   latency tripled 2->6 ms, throughput halved). The /3 prescaler -> ~279 us
 *   effective (still inside the ~640 us RHR+shift overrun window) drops the cost
 *   to ~45% and restores most performance. This mirrors stock fw (Timer1 RX poll
 *   93 us + /3 = 279 us effective). FOLLOW-UP optimisation (PINSA RX-bitmap, see
 *   the Port A block below): read the 8-bit RX-pending bitmap in 1 MOVX and only
 *   touch RHR for flagged ports -> ~6 % CPU even at a flat 100 us, but needs the
 *   PINSA polarity/wiring verified on HW first. */
#define BOARD_T0_RELOAD    0x46  /* Timer0 TH0/TL0 reload: ~93 us @ Fosc/12     */
#define BOARD_T0_PRESCALE  3     /* run the LSR sweep every 3rd tick -> ~279 us */

/* ST16C454 RESET release (CONFIRMED: this is what unblocked external writes). */
/* RESET (pin 37) is ACTIVE HIGH and wired to AN2131 PB4 (PALLV16 pin6 trace). */
/* Drive PB4 low: PORTBCFG.4=0 (GPIO, not INT4), OUTB.4=0 (latch low), then    */
/* OEB.4=1 (enable the output driver). Stock does OEB.4 on the host START      */
/* (0xFD) cmd just before uart_init; our class fw does all three at board_init.*/
/* Without this the UART stays in reset: writes read back 0xFF, reads ghost     */
/* reset-default registers. See doc/bus_write_debug.md (RESOLVED 2026-06-05).  */
#define BOARD_UART_RESET_PORTB_BIT 0x10  /* PB4 mask in PORTBCFG/OUTB/OEB      */
#define BOARD_PORTBCFG           0x7F94  /* clear bit4 -> PB4 = GPIO           */
#define BOARD_OUTB               0x7F97  /* clear bit4 -> PB4 latch low        */
#define BOARD_OEB                0x7F9D  /* set   bit4 -> PB4 driven (RESET=0) */

/* 0xFE00 is external SRAM (CY62256) bookkeeping, NOT a UART glue latch -- the  */
/* stock 0xC9 write there is not load-bearing for the UART (kept for fidelity). */
#define BOARD_GLUE_LATCH     0xFE00 /* SRAM byte stock fw writes 0xC9 (not UART)*/
#define BOARD_GLUE_VALUE     0xC9

/* ---- Port A = per-port RX interrupt lines (PCB-traced + fw-confirmed) -- */
/* Each ST16C454 channel INT output (16/Intel mode) is wired to a Port A bit:
 *   PA0..3 = chip1 INTA..D (ports 0..3), PA4..7 = chip2 INTA..D (ports 4..7)
 * So PINSA bit N == RX-pending for MIDI port N. Stock fw ignores this (IER=0,
 * no PINSA access) and polls LSR instead. Our fw may enable IER ERBFI and read
 * PINSA once per poll as an 8-bit RX bitmap (1 MOVX vs 8 LSR reads). Port A is
 * input at reset, so no OEA/PORTACFG write is needed. Check INTSEL/polarity. */
#define BOARD_PORTACFG       0x7F93   /* leave 0 (GPIO) - reset default      */
#define BOARD_PINSA          0x7F99   /* read: PA input pins = RX-IRQ bitmap */
#define BOARD_OEA            0x7F9C   /* leave 0 (input) - reset default     */
#define BOARD_RXIRQ_BIT(port) (1u << (port)) /* PINSA bit for a MIDI port    */
#define UART_IER_ERBFI       0x01     /* IER bit0: enable RX-data interrupt  */

/* ---- EZ-USB control / endpoint buffers (AN2131) ----------------------- */
/* Class firmware uses the EP2 bulk pair (EP2-IN + EP2-OUT) for MIDI. The bridge
 * code addresses these via the reg_ezusb.h SFRX symbols (IN2BUF/OUT2BUF/IN2BC/
 * OUT2BC); the addresses are recorded here for documentation. */
#define EZUSB_CPUCS          0x7F92
#define EZUSB_EP2IN_BUF      0x7E00   /* device->host MIDI (IN2BUF)        */
#define EZUSB_EP2IN_BC       0x7FB9   /* IN2BC                             */
#define EZUSB_EP2OUT_BUF     0x7DC0   /* host->device MIDI (OUT2BUF)       */
#define EZUSB_EP2OUT_BC      0x7FC9   /* OUT2BC                            */

/* ---- Not present / not used on r1 ------------------------------------- */
#define BOARD_HAS_FX_INIT    0        /* r2 sets 0x7FE5; r1 does not        */
#define BOARD_HAS_LED_LATCH  0        /* no XDATA LED latch found; deferred */

#endif /* BOARD_R1_H */
