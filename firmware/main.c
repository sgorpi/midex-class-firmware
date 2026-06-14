/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Copyright (C) 2026 Hedde Bosman (sgorpi@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 */

/*
 * MIDEX8 r1 class-compliant MIDI firmware - full 8-port build.
 *
 * Enumerates as a standard USB Audio Class / MIDIStreaming device (VID 0x0A4E,
 * PID 0x10C1) with NUM_MIDI_PORTS bidirectional cables, so the OS's generic
 * USB-MIDI driver (snd-usb-audio on Linux) binds with no custom driver. The
 * polled main loop bridges the single bulk endpoint pair to the external 16550
 * UARTs:
 *
 *   TX (host -> instrument): bridge_out() decodes each EP2-OUT 4-byte USB-MIDI
 *     event packet's CIN into a MIDI byte count and pushes those bytes into the
 *     cable's TX ring, re-arming EP2-OUT immediately; uart_tx_pump() then feeds
 *     the UARTs from the rings one byte/port per pass (non-blocking). This keeps
 *     the slow 31250-baud UART from stalling the loop, so bursts aren't lost.
 *   RX (instrument -> host): midi_rx_pump() (midi_parser.c) polls each cable's
 *     UART and reassembles the raw byte stream into properly framed USB-MIDI
 *     event packets (running status, channel/system-common lengths, SysEx, and
 *     real-time bytes -> correct CINs) on EP2-IN.
 *
 * Bring-up (board_init) and the UART register map are hardware-validated;
 * see ../doc/hardware_register_map.md. Enumeration scaffolding
 * (usb.c, usb_descriptors.c, USBJmpTb.a51, reg_ezusb.h) is vendored from
 * src/ezusb-firmware (OpenULINK fork).
 */
#include "reg_ezusb.h"
#include "usb.h"
#include "delay.h"
#include "board.h"
#include "uart.h"
#include "midi_config.h"
#include "midi_parser.h"

/*
 * All ISRs must be declared (with __interrupt) in the module that contains
 * main() so SDCC generates / reserves the 8051 interrupt vector table. The ISR
 * bodies live in usb.c; the EZ-USB autovector table (USBJmpTb.a51 @0x1B00)
 * dispatches the USB interrupt to them.
 */
extern void sudav_isr(void)    __interrupt SUDAV_ISR;
extern void sof_isr(void)      __interrupt;
extern void sutok_isr(void)    __interrupt;
extern void suspend_isr(void)  __interrupt;
extern void usbreset_isr(void) __interrupt;
extern void ibn_isr(void)      __interrupt;
extern void ep0in_isr(void)    __interrupt;
extern void ep0out_isr(void)   __interrupt;
extern void ep1in_isr(void)    __interrupt;
extern void ep1out_isr(void)   __interrupt;
extern void ep2in_isr(void)    __interrupt;
extern void ep2out_isr(void)   __interrupt;
extern void ep3in_isr(void)    __interrupt;
extern void ep3out_isr(void)   __interrupt;
extern void ep4in_isr(void)    __interrupt;
extern void ep4out_isr(void)   __interrupt;
extern void ep5in_isr(void)    __interrupt;
extern void ep5out_isr(void)   __interrupt;
extern void ep6in_isr(void)    __interrupt;
extern void ep6out_isr(void)   __interrupt;
extern void ep7in_isr(void)    __interrupt;
extern void ep7out_isr(void)   __interrupt;

/* Timer0 RX-capture ISR (body in uart.c). Declared here -- with its vector and
 * register bank -- so SDCC reserves the 8051 Timer0 interrupt vector (0x000B).
 * __using 1: private register bank for safe nesting over the USB interrupt. */
extern void uart_rx_isr(void) __interrupt TF0_VECTOR __using 1;

/*
 * USB-MIDI Code Index Number -> number of MIDI data bytes in the 3-byte payload
 * of a 4-byte event packet (USB MIDI 1.0 spec, table 4-1). Index 0/1 are
 * reserved (0 bytes). Used on the TX path to know how many bytes to forward.
 */
static const __code uint8_t cin_len[16] = {
	0, 0, 2, 3,   /* 0x0 rsvd, 0x1 rsvd, 0x2 2-byte sys-common, 0x3 3-byte    */
	3, 1, 2, 3,   /* 0x4 SysEx start/cont, 0x5 1-byte, 0x6 SysEx-2, 0x7 SysEx-3*/
	3, 3, 3, 3,   /* 0x8 noteoff, 0x9 noteon, 0xA poly-AT, 0xB CC            */
	2, 2, 3, 1    /* 0xC progchg, 0xD chan-AT, 0xE pitchbend, 0xF single byte*/
};

/*
 * Bring up the external-memory bus and the UART clock the way the stock
 * fw_entry does before any UART access. Hardware-validated via the EP0
 * bus-probe loopback. See ../doc/hardware_register_map.md.
 */
static void board_init(void)
{
	/* CPUCS=0 clears CLK24OE (bit1), matching stock fw_entry from boot. (On the
	 * FX, CPUCS.3 -- the 24/48 MHz strap -- is read-only, so this is harmless
	 * there and the on-chip baud setup honours it later.) */
	CPUCS = 0;

#if BOARD_REV == 2
	/* ---- MIDEX8 r2 (EZ-USB FX): hybrid backend bring-up ---------------- */
	/* External-memory + FX strobes and the on-chip-UART pin mux, all via the
	 * port-config registers (see board_r2.h, derived from stock fw_main):
	 *   PORTACFG = OE#               (PA2, FX external-bus output enable)
	 *   PORTCCFG = RD#|WR#|TxD0|RxD0 (PC7/PC6 bus strobes, PC1/PC0 UART0)
	 *   PORTBCFG = TxD1|RxD1         (PB3/PB2 UART1; PB4 stays GPIO for RESET) */
	PORTACFG = BOARD_PORTACFG_VAL;
	PORTCCFG = BOARD_PORTCCFG_VAL;
	PORTBCFG = BOARD_PORTBCFG_VAL;

	/* MOVX cycle stretch = 0 and T2M=0, as stock (Timer2 is unused on r2 -- the
	 * external UART clock is the board 12 MHz crystal, not Timer2->PB7). */
	CKCON &= ~0x27;

	/* Hold BOTH external UARTs in reset through power-on + enumeration, then
	 * release them in uart_bringup() once the external write glue has settled
	 * (same late-bring-up rationale as r1). The two chips have OPPOSITE reset
	 * polarity (see board_r2.h):
	 *   ST16C454 RESET (PB4, active-high): leave PB4 high-Z (pulled high =
	 *     asserted); latch OUTB.4=0 so enabling the driver later drives it low.
	 *   ST16C452 RESET (PC4, active-low): drive PC4 low NOW (= asserted) and
	 *     enable its output; uart_bringup raises it to release. */
	OUTB &= ~BOARD_R454_RESET_OUT_BIT;   /* PB4 latch low, not yet driven   */
	OUTC &= ~BOARD_R452_RESET_OUT_BIT;   /* PC4 = 0 -> ST16C452 in reset     */
	OEC  |= BOARD_R452_RESET_OEC_BIT;    /* drive PC4                        */

#if BOARD_MIRROR_PC2_LOW
	/* PC2: stock drives this output low; purpose untraceable on the PCB.
	 * Mirrored as cheap insurance against it being an external-bus buffer-OE
	 * (see board_r2.h). Drop once bring-up proves ports 0-5 work without it. */
	OUTC &= ~OUTC2;
	OEC  |= BOARD_PC2_OEC_BIT;
#endif

#else
	/* ---- MIDEX8 r1 (EZ-USB AN2131): all-external backend -------------- */
	/* PORTCCFG bit7=RD#, bit6=WR#: PC7/PC6 become the external-memory read/
	 * write strobes. Without this, MOVX to the UART window never latches. */
	PORTCCFG |= 0xC0;

	/* PORTBCFG bit7=T2OUT: route Timer2 clock-out to PB7 = the ST16C454 XIN. */
	PORTBCFG |= 0x80;

	/* Timer2 auto-reload: Fosc/12 = 2 MHz, reload 0xFFFE -> 1 MHz overflow ->
	 * T2OUT toggles at 500 kHz = 31250 baud * 16 (matches divisor-1 init). */
	RCAP2L = BOARD_T2_RCAP2L;
	RCAP2H = BOARD_T2_RCAP2H;
	T2CON = 0x00;
	TR2 = 1;
	/* T2M=0 (Timer2 = Fosc/12) and MOVX cycle stretch = 0 (bits 2:0), exactly
	 * as stock. */
	CKCON &= ~0x27;

	/* PB4 = ST16C454 RESET (active-high). Prepare it as a GPIO with the output
	 * latch low, but DO NOT drive it yet -- leave PB4 high-Z (pulled high) so
	 * the UARTs stay IN RESET through power-on and USB enumeration. They are
	 * released and configured later by uart_bringup(), once the external write
	 * glue has settled. (Stock does the same: PB4 stays high-Z until the host's
	 * START command, which then drives it low and runs uart_init.) Releasing +
	 * configuring at boot is marginal -- one channel intermittently fails to
	 * latch its LCR (comes up in 5-bit mode) and corrupts that port. */
	PORTBCFG &= ~INT4;   /* PB4 = GPIO (clear INT4 alt-function) */
	OUTB     &= ~OUTB4;  /* PB4 output latch = 0 (drives RESET low once enabled) */

	/* 0xFE00.. is external SRAM bookkeeping (not a UART latch). Retained to
	 * match the bring-up state the loopback validated; not load-bearing. */
	*((__xdata uint8_t *)BOARD_GLUE_LATCH) = BOARD_GLUE_VALUE;
	*((__xdata uint8_t *)0xFE02) = 0x00;
	*((__xdata uint8_t *)0xFE01) = 0x10;
#endif
}

/*
 * Per-port host->device TX ring buffers. EP2-OUT is decoded into these and the
 * endpoint re-armed immediately (bridge_out); uart_tx_pump then feeds the
 * 31250-baud UARTs from them one byte per loop pass, non-blocking. This is the
 * fix for the burst-loss bug: the old bridge busy-waited on the slow UART while
 * draining a packet, which starved midi_rx_pump and dropped looped-back bursts
 * (chords / running status / multi-packet SysEx collapsed to their last packet).
 */
static __xdata uint8_t tx_ring[NUM_MIDI_PORTS][MIDI_TX_RING_SIZE];
static __xdata uint8_t tx_head[NUM_MIDI_PORTS];   /* write index */
static __xdata uint8_t tx_tail[NUM_MIDI_PORTS];   /* read index  */

/* Resume offset into OUT2BUF when a packet didn't fit and we left EP2-OUT
 * un-rearmed for back-pressure (the host NAKs until the rings drain). */
static uint8_t out_off;

#define TX_RING_MASK (MIDI_TX_RING_SIZE - 1)

/* Zero the TX ring state. XDATA is NOT auto-initialised on this target (same
 * reason midi_parser_reset exists), so without this the garbage head/tail make
 * uart_tx_pump believe ports have data and spew continuously (stuck OUT LEDs). */
static void tx_reset(void)
{
	uint8_t p;

	for (p = 0; p < NUM_MIDI_PORTS; p++) {
		tx_head[p] = 0;
		tx_tail[p] = 0;
	}
	out_off = 0;
}

/* Free space in port p's ring (one slot reserved to distinguish full/empty). */
static uint8_t tx_room(uint8_t p)
{
	return (uint8_t)(TX_RING_MASK -
			 ((uint8_t)(tx_head[p] - tx_tail[p]) & TX_RING_MASK));
}

/* Host -> instrument: decode EP2-OUT USB-MIDI packets into the per-port TX
 * rings, then re-arm EP2-OUT. Applies back-pressure: if a cable's ring can't
 * hold the packet, stop and leave the endpoint un-rearmed so it is retried
 * (from out_off, no duplication) once uart_tx_pump has drained room. */
static void bridge_out(void)
{
	uint8_t n, i, j, len, cn;

	if (!Semaphore_EP2_out)
		return;

	n = OUT2BC;                         /* bytes the host sent this packet */
	for (i = out_off; (uint8_t)(i + 4) <= n; i += 4) {
		cn  = OUT2BUF[i] >> 4;          /* cable number = high nibble       */
		len = cin_len[OUT2BUF[i] & 0x0F];
		if (cn >= NUM_MIDI_PORTS || len == 0)
			continue;                   /* no such cable / no data -> drop  */
		if (tx_room(cn) < len) {
			out_off = i;                /* ring full: resume here next pass */
			return;                     /* leave EP un-rearmed (host NAKs)  */
		}
		for (j = 0; j < len; j++) {
			tx_ring[cn][tx_head[cn]] = OUT2BUF[i + 1 + j];
			tx_head[cn] = (tx_head[cn] + 1) & TX_RING_MASK;
		}
	}

	out_off = 0;
	Semaphore_EP2_out = 0;
	OUT2BC = 0;                         /* re-arm EP2-OUT to receive again  */
}

/* Drain each port's TX ring to its UART: at most one byte per port per pass
 * (gated by THRE), so the loop never blocks on the slow UART. */
static void uart_tx_pump(void)
{
	uint8_t p;

	for (p = 0; p < NUM_MIDI_PORTS; p++) {
		if (tx_head[p] != tx_tail[p] && uart_tx_ready(p)) {
			uart_putc(p, tx_ring[p][tx_tail[p]]);
			tx_tail[p] = (tx_tail[p] + 1) & TX_RING_MASK;
		}
	}
}

/* Release the ST16C454s from reset and configure them. Deferred until after USB
 * enumeration (see board_init): doing it at power-on is marginal -- the external
 * 0x40xx write glue (PAL16V8/74HC138/74HC123) doesn't reliably latch yet, and a
 * channel comes up in 5-bit mode. Stock and the bus-probe both configure late
 * and never hit this. */
static void uart_bringup(void)
{
	delay_ms(BOARD_UART_BRINGUP_DELAY_MS);  /* let the write glue settle    */
#if BOARD_REV == 2
	/* Release both external UARTs from reset (opposite polarities, see
	 * board_init): ST16C454 by driving PB4 low, ST16C452 by raising PC4 high.
	 * PC4's output driver was already enabled in board_init. */
	OEB  |= BOARD_R454_RESET_OEB_BIT;       /* drive PB4 low -> 454 released  */
	OUTC |= BOARD_R452_RESET_OUT_BIT;       /* PC4 high      -> 452 released  */
	delay_ms(1);                            /* reset-recovery                */
	uart_init();                            /* ext (div 24) + on-chip UART0/1 */
#else
	OEB |= OEB4;                            /* drive PB4 low: RESET released */
	delay_ms(1);                            /* ST16C454 reset-recovery       */
	uart_init();                            /* 8N1, divisor 1, verified      */
#endif
}

void main(void)
{
	/* Bring up the external-memory bus + UART clock (UARTs stay in reset). */
	board_init();

	/* usb_init() performs RENUM re-enumeration: the device drops off the bus
	 * and reappears as 0x0A4E:0x10C1, a class-compliant MIDIStreaming device.
	 * It also runs ISODISAB, which frees the isochronous-EP buffer RAM at
	 * 0x2000 for use as XDATA -- exactly where our TX/RX rings + parser state
	 * live. So the ring/parser state MUST be zeroed AFTER this: doing it before
	 * lets enumeration's iso-buffer activity clobber the indices with garbage,
	 * which made uart_tx_pump spew (stuck OUT LEDs) and corrupt early RX. */
	usb_init();

	/* Global interrupt enable - without this SUDAV never fires and enumeration
	 * times out. */
	EA = 1;

	/* Now that the iso-buffer XDATA is freed, initialise the ring/parser state
	 * (XDATA is not auto-zeroed on this target). */
	tx_reset();
	midi_parser_reset();
	uart_rx_reset();   /* zero the RX capture FIFOs (after usb_init clobber) */

	/* Now that power/clock/enumeration have settled, release the UART reset and
	 * configure the channels (see uart_bringup). */
	uart_bringup();

	/* UART live + FIFOs valid: start the high-priority Timer0 RX-capture tick. */
	uart_rx_start();

	/* Arm EP2-OUT to receive the first host packet (in case no SET_INTERFACE). */
	OUT2BC = 0;

	while (1) {
		bridge_out();      /* EP2-OUT -> per-port TX rings (re-arm fast)   */
		uart_tx_pump();    /* TX rings -> UARTs, one byte/port, non-blocking*/
		midi_rx_pump();    /* UARTs -> parse -> EP2-IN                      */
	}
}
