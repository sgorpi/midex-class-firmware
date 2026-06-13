/*
 * board_r2.h - MIDEX8 r2 (EZ-USB FX CY7C646 + ST16C454 + ST16C452) config.
 *
 * r2 is a HYBRID MIDI backend (see ../doc/midex8_r1_vs_r2.md, derived from the
 * stock r2 disassembly fw_main@CODE:204 / uart_ext_init_6ports@169A and the
 * CY7C646 TRM, then cross-checked against the user's PCB trace + datasheets):
 *
 *   ports 0-5  -> 6 external 16550 channels (ST16C454 x1 + ST16C452 x1) at the
 *                 same 0x4040 + port*8 window as r1, but DIVISOR 24 (fixed 12 MHz
 *                 external clock from the board crystal, not Timer2->PB7).
 *   ports 6-7  -> the FX's two on-chip UARTs (UART0 PC0/PC1, UART1 PB2/PB3),
 *                 Timer1 baud gen, polled like the external ports.
 *
 * FX port/SFR identity (TRM section 4): IOA=PORTA, IOB=PORTB, IOC=PORTC,
 * IOD=PORTD. We drive the GPIO via the XDATA OUTx/OEx registers (reg_ezusb.h),
 * which is the same mechanism board_r1.h uses for PB4.
 */
#ifndef BOARD_R2_H
#define BOARD_R2_H

/* ---- USB identity ------------------------------------------------------ */
#define BOARD_USB_VID        0x0A4E   /* Steinberg                          */
#define BOARD_USB_PID        0x10C2   /* class-compliant r2 PID, distinct
                                       * from r1's 0x10C1 and outside the
                                       * loader / snd-usb-midex tables       */
#define BOARD_NUM_PORTS      8        /* main loop iterates ports 0..7       */
#define BOARD_REV            2        /* hardware revision (see board.h)     */

/* ---- UART backend split (see uart.c dispatcher) ----------------------- */
/* Hybrid: ports 0..5 external 16550, ports 6..7 on-chip FX UART0/UART1. */
#define BOARD_HAS_ONCHIP_UART   1
#define BOARD_NUM_EXT_PORTS     6     /* ports 0..5 external 16550           */
#define BOARD_ONCHIP_PORT_FIRST 6     /* ports 6,7 = on-chip UART0,UART1      */

/* ---- External UART bank (16550-class, memory-mapped XDATA) ------------- */
/* UART[port] base = BOARD_UART_BASE + port * BOARD_UART_STRIDE             */
#define BOARD_UART_BACKEND_EXTERNAL_16550  1
#define BOARD_UART_BASE      0x4040   /* port 0 register block (same as r1) */
#define BOARD_UART_STRIDE    0x08     /* 8 bytes per channel                */

/* Standard 16550 datasheet register offsets (added to a channel base) */
#define UART_RHR  0   /* read:  receive holding register (DLAB=0)          */
#define UART_THR  0   /* write: transmit holding register (DLAB=0)         */
#define UART_DLL  0   /* divisor latch low  (DLAB=1)                       */
#define UART_DLM  1   /* divisor latch high (DLAB=1)                       */
#define UART_IER  1   /* interrupt enable (DLAB=0) - left 0, polled        */
#define UART_LCR  3   /* line control                                      */
#define UART_MCR  4   /* modem control                                     */
#define UART_LSR  5   /* line status                                       */
#define UART_MSR  6   /* modem status                                      */
#define UART_SCR  7   /* scratch                                           */

/* LSR status bits the bridge polls */
#define UART_LSR_DR    0x01   /* data ready (RX)                           */
#define UART_LSR_THRE  0x20   /* transmit holding reg empty (TX)           */

/* ---- Line setup: 8N1, divisor 24 -------------------------------------- */
/* Divisor 24 (stock uart_ext_init_6ports DLL=0x18, unconditional) => the
 * external XIN is a fixed 12 MHz board crystal: 12e6 / (16*24) = 31250 baud,
 * independent of the 24/48 MHz core strap. r2 does NOT generate the clock on
 * Timer2->PB7 like r1. */
#define BOARD_UART_LCR_DLAB  0x83   /* DLAB=1 | 8N1                         */
#define BOARD_UART_LCR_8N1   0x03   /* DLAB=0 | 8N1                         */
#define BOARD_UART_DIVISOR   24
#define BOARD_UART_LCR_MAX_TRIES 50

/* Delay (ms) after enumeration before releasing UART RESET + running uart_init
 * (same late-bring-up rationale as r1; see main.c uart_bringup). */
#define BOARD_UART_BRINGUP_DELAY_MS 100

/* ---- External-UART RESET wiring (CONFIRMED: PCB trace + datasheet + fw) - */
/* Two chips with OPPOSITE reset polarity, each on a distinct FX port pin.
 * Both are driven as a STATIC level (held, not pulsed), exactly as stock.   */
/* ST16C454 (ports 0-3): RESET pin37 ACTIVE-HIGH on PB4 (IOB.4 / "P1.4").    */
/*   Released by driving PB4 LOW: OEB.4=1 (drive) + OUTB.4=0.                 */
#define BOARD_R454_RESET_OEB_BIT   OEB4    /* OEB bit4 -> PB4 output         */
#define BOARD_R454_RESET_OUT_BIT   OUTB4   /* OUTB bit4=0 -> PB4 low (run)   */
/* ST16C452 (ports 4-5): Master Reset pin39 ACTIVE-LOW on PC4 (IOC.4/"P2.4").*/
/*   Released by driving PC4 HIGH: OEC.4=1 (drive) + OUTC.4=1.                */
#define BOARD_R452_RESET_OEC_BIT   OEC4    /* OEC bit4 -> PC4 output         */
#define BOARD_R452_RESET_OUT_BIT   OUTC4   /* OUTC bit4=1 -> PC4 high (run)  */

/* ---- FX port-mux + external-bus bring-up (see board_init in main.c) ----- */
/* PORTACFG.2=OE# : the FX external-memory output-enable strobe (stock sets
 * this; r1/AN2131 did not need it). PORTCCFG b7=RD#, b6=WR# (external bus) and
 * b1=TxD0, b0=RxD0 (on-chip UART0 pins). PORTBCFG b3=TxD1, b2=RxD1 (on-chip
 * UART1 pins); b4 must stay GPIO (INT4 alt cleared) so it can drive the 454
 * RESET. */
#define BOARD_PORTACFG_VAL   OE              /* PA2 = OE# external-bus strobe */
#define BOARD_PORTCCFG_VAL   (RD | WR | TXD0 | RXD0)  /* PC7/6/1/0           */
#define BOARD_PORTBCFG_VAL   (TXD1 | RXD1)   /* PB3/PB2 on-chip UART1         */

/* ---- Timer0 = RX-capture tick (shared core, see uart.c uart_rx_isr) ---- */
/* Same design as r1 and stock r2's Timer2 poller: mode 2, Fosc/12 = 0.5 us/tick,
 * reload 0x46 (~93 us) with a /3 software prescaler -> ~279 us effective sweep,
 * inside the single-byte RHR/SBUF overrun window. (r2 uses Timer1 for the
 * on-chip-UART baud gen, so RX capture stays on Timer0 as on r1.) */
#define BOARD_T0_RELOAD    0x46
#define BOARD_T0_PRESCALE  3

/* ---- On-chip UART0/UART1 (ports 6,7) ---------------------------------- */
/* Both UARTs in mode 1 (8-bit, variable baud), REN=1, shared Timer1 baud gen.
 * SCONx = SM1|REN (0x50); the TX-ready (TI) flag is seeded =1 in uart_onchip_init
 * so the first uart_putc is allowed (polled TX, see uart_onchip.c). */
#define BOARD_SCON_MODE1_REN  0x50   /* SM1=1 (mode1) | REN=1                */
/* Timer1 mode 2 (8-bit auto-reload) baud generator, CKCON.T1M=1 (CLKOUT/4):
 *   baud = (2^SMOD / 32) * Fcore / (4 * (256 - reload))
 * reload 0xF4 (=12 counts): @24MHz with SMOD=1 -> 31250; @48MHz with SMOD=0 ->
 * 31250. So SMOD is set at 24 MHz and cleared at 48 MHz (clock-agnostic). */
#define BOARD_ONCHIP_T1_RELOAD   0xF4
/* CPUCS bit3 = the FX read-only "24/48" core-clock strap (1 => 48 MHz). On the
 * AN2131 this bit reads 0, so this test is r2-only. */
#define BOARD_CPUCS_48MHZ        0x08

/* ---- EZ-USB control / endpoint buffers -------------------------------- */
/* Identical addresses to r1 (the FX is register-compatible for the classic
 * 8-endpoint core; confirmed in stock fw_main: OUT2BUF 0x7DC0 etc.). */
#define EZUSB_CPUCS          0x7F92
#define EZUSB_EP2IN_BUF      0x7E00
#define EZUSB_EP2IN_BC       0x7FB9
#define EZUSB_EP2OUT_BUF     0x7DC0
#define EZUSB_EP2OUT_BC      0x7FC9

/* ---- Stock GPIO of unverified purpose --------------------------------- */
/* PC2 (IOC.2): stock drives it as an output held LOW. Not traceable on the
 * PCB, and if it is a data-bus buffer-OE / decoder enable for the external
 * 16550s, dropping it would silently kill ports 0-5. Mirrored verbatim as
 * cheap insurance; drop it once bring-up proves the external ports work
 * without it. (The LED/button GPIO PA3/PA4/PA5/PORTD/PB5/PB7 are dropped.) */
#define BOARD_MIRROR_PC2_LOW  1
#define BOARD_PC2_OEC_BIT     OEC2

#endif /* BOARD_R2_H */
