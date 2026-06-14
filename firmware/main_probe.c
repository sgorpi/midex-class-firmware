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
 * MIDEX8 r1 bus-probe firmware.
 *
 * A minimal EZ-USB (AN2131) firmware whose only job is to validate the static
 * RE findings in ../doc/hardware_register_map.md on real hardware. It
 * enumerates as a vendor-class device (VID 0x0A4E, PID 0x10C0) and exposes two
 * EP0 vendor commands that perform a host-directed MOVX read/write to an
 * arbitrary 8051 XDATA address -- including the external UART bank at
 * 0x4040 + port*8, which is only reachable while the 8051 core is running.
 *
 * Use from the host (see ../host) to drive the 16550 init + THR write and
 * confirm a MIDI byte appears on a physical port, and to read RHR back on the
 * looped-in port. It is retained as a diagnostic alongside the class-compliant
 * firmware (build with `make probe`).
 *
 * Enumeration scaffolding (usb.c, USBJmpTb.a51, reg_ezusb.h, ...) is vendored
 * from src/ezusb-firmware (OpenULINK fork) and kept byte-identical except for
 * the device identity strings.
 */
#include "reg_ezusb.h"
#include "usb.h"
#include "delay.h"
#include "board_r1.h"

/*
 * All ISRs must be declared (with __interrupt) in the module that contains
 * main() so SDCC generates / reserves the 8051 interrupt vector table. The
 * ISR bodies live in usb.c; the EZ-USB autovector table (USBJmpTb.a51 @0x1B00)
 * dispatches the USB interrupt to them. SUDAV_ISR (=13) carries the explicit
 * vector number to reserve the table space; the rest are bare forward decls.
 * (Mirrors src/ezusb-firmware/src/main.c.)
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

/*
 * EP0 vendor commands (bRequest). The target XDATA address travels in wValue;
 * for writes the data byte travels in wIndex, so a write needs no EP0 OUT data
 * stage (keeps the handler trivial and avoids EP0 data-stage races).
 *
 *   read : bmRequestType 0xC0, bRequest 0xB0, wValue=addr            -> 1 byte IN
 *   write: bmRequestType 0x40, bRequest 0xB1, wValue=addr, wIndex=byte
 */
#define VR_READ_XDATA   0xB0
#define VR_WRITE_XDATA  0xB1
/*
 * Multimeter diagnostics: hammer an address with continuous writes/reads so a
 * DC voltmeter can see the WR#/RD# strobe (and whether it reaches the UART) as
 * an average-voltage drop below Vcc. These loop forever -- power-cycle to stop
 * (the host ctrl_transfer will time out, which is expected).
 */
#define VR_HAMMER_WRITE 0xB2   /* wValue = address; loops writing 0x55/0xAA */
#define VR_HAMMER_READ  0xB3   /* wValue = address; loops reading           */
/*
 * Set CKCON (SFR 0x8E) live so the host can sweep the MOVX cycle-stretch
 * (bits 2:0 = 0..7 extra cycles) without re-uploading. The data + chip-select +
 * write strobe all reach the UART, but the write never latches; widening the
 * write strobe is the AN2131's only bus-timing knob. wValue low byte = CKCON.
 */
#define VR_SET_CKCON    0xB4   /* wValue = new CKCON value */

/*
 * Bring up the external-memory bus and the UART clock the way the stock
 * fw_entry does before any UART access. Discovered via the bus-probe: a
 * minimal EZ-USB firmware addresses the UART window but never strobes it.
 * See ../doc/hardware_register_map.md.
 */
static void board_init(void)
{
	/* Match stock fw_entry exactly: CPUCS=0 clears CLK24OE (bit1). Our probe
	 * otherwise powers up with CPUCS=0x42 (CLK24OE set); if the board's PAL is
	 * clocked from anything derived from CLKOUT, that state could differ from
	 * stock. Clearing it from boot (not just poking it at runtime) rules that
	 * out and makes the bus bring-up byte-for-byte stock. */
	CPUCS = 0;

	/* PORTCCFG bit7=RD#, bit6=WR#: switch PC7/PC6 from GPIO to the external
	 * memory read/write strobes. Without this, MOVX to 0x4040+port*8 drives
	 * the address/data bus but never asserts RD#/WR#, so the UART never
	 * latches and reads ghost the last value left on the data bus. */
	PORTCCFG |= 0xC0;

	/* PORTBCFG bit7=T2OUT: route Timer2 clock-out to PB7, which feeds the
	 * ST16C454 XIN clock. */
	PORTBCFG |= 0x80;

	/* Timer2 auto-reload clock generator. Fosc/12 = 2 MHz, reload 0xFFFE
	 * (= 2 counts) -> 1 MHz overflow -> T2OUT toggles at 500 kHz, i.e.
	 * 31250 baud * 16, matching the divisor-1 UART init. */
	RCAP2L = 0xFE;
	RCAP2H = 0xFF;
	T2CON = 0x00;
	TR2 = 1;
	/* Match the stock firmware's CKCON exactly: T2M=0 (Timer2 = Fosc/12) AND
	 * MOVX cycle stretch = 0 (bits 2:0). The AN2131 powers up with stretch=1
	 * (3-cycle MOVX); the stock firmware forces it to 0 (2-cycle). This is the
	 * only bus-setup register that differed from stock, so the external-write
	 * failure is pinned to MOVX timing. */
	CKCON &= ~0x27;     /* clear bit5 (T2M) + bits 2:0 (stretch) */

	/* PB4 = ST16C454 RESET (active-high), PCB-traced and fw-confirmed.
	 * The stock fw_entry clears PORTBCFG.4 (GPIO, not INT4) and OUTB.4 (latch
	 * low), then on the host START command (0xFD in midi_in_aggregate @0x10A8)
	 * sets OEB.4 to actually DRIVE PB4 low, immediately before
	 * uart_init_all_ports (@0x1177). With PB4 high-Z (our previous probe), the
	 * external pull holds RESET high -> the UART sits in reset, so MOVX writes
	 * never latch and reads ghost 0xFF. Driving PB4 low here releases reset and
	 * is the suspected fix for the external-write failure. */
	PORTBCFG &= ~INT4;   /* PB4 = GPIO (clear INT4 alt-function, bit4) */
	OUTB     &= ~OUTB4;  /* PB4 output latch = 0 */
	OEB      |=  OEB4;   /* PB4 = driven output -> ST16C454 RESET deasserted */

	/* Glue/control latch the stock firmware programs before UART use
	 * (exact function TBD; replicated for fidelity). */
	*((__xdata uint8_t *)0xFE00) = 0xC9;
	*((__xdata uint8_t *)0xFE02) = 0x00;
	*((__xdata uint8_t *)0xFE01) = 0x10;
}

/*
 * EXPERIMENT: reads are still serviced in the SUDAV interrupt (their data
 * stage blocks until IN0BC is written, so no race). Writes are deferred to
 * the main loop -- the ISR copies the SETUP fields and flags a pending write;
 * the host serialises transfers, so the tight main loop drains it before the
 * next SETUP arrives. This tests whether the external-bus write only fails
 * from interrupt context (the one execution difference from the stock fw,
 * which writes the UART from its main loop).
 */
static volatile uint16_t pend_addr;
static volatile uint8_t pend_val;
static volatile bool pend_write;

void handle_vendor_command(void)
{
	__xdata uint8_t *addr = (__xdata uint8_t *)setup_data.wValue;

	switch (setup_data.bRequest) {
	case VR_READ_XDATA:
		/* MOVX A,@DPTR - drives the external bus for addr >= 0x2000 */
		IN0BUF[0] = *addr;
		IN0BC = 1;
		break;
	case VR_WRITE_XDATA:
		/* Defer the MOVX write to the main loop. */
		pend_addr = setup_data.wValue;
		pend_val = (uint8_t)(setup_data.wIndex & 0xff);
		pend_write = 1;
		break;
	case VR_HAMMER_WRITE:
		/* Continuous writes so a multimeter can see WR# pulse. Never
		 * returns; power-cycle to stop. */
		for (;;) {
			*addr = 0x55;
			*addr = 0xAA;
		}
	case VR_HAMMER_READ:
		/* Continuous reads so a multimeter can see RD# pulse. */
		for (;;) {
			pend_val = *addr;
		}
	case VR_SET_CKCON:
		/* Live MOVX cycle-stretch sweep: wValue low byte -> CKCON. */
		CKCON = (uint8_t)(setup_data.wValue & 0xff);
		break;
	default:
		STALL_EP0();
		break;
	}
}

void main(void)
{
	/* Bring up the external UART bus + clock before anything touches it. */
	board_init();

	/* usb_init() performs RENUM re-enumeration: the device drops off the
	 * bus and reappears as 0x0A4E:0x10C0. */
	usb_init();

	/* Global interrupt enable - without this the SUDAV interrupt never
	 * fires and enumeration times out (device descriptor read error). */
	EA = 1;

	while (1) {
		if (pend_write) {
			/* External MOVX write executed in main-loop context. */
			*((__xdata uint8_t *)pend_addr) = pend_val;
			pend_write = 0;
		}
	}
}
