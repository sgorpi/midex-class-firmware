/*
 * MIDEX8 r1 class-compliant MIDI firmware - Phase 2 spike.
 *
 * Enumerates as a standard USB Audio Class / MIDIStreaming device (VID 0x0A4E,
 * PID 0x10C1) with NUM_MIDI_PORTS bidirectional cables, so the OS's generic
 * USB-MIDI driver (snd-usb-audio on Linux) binds with no custom driver. The
 * polled main loop bridges the single bulk endpoint pair to the external 16550
 * UARTs:
 *
 *   TX (host -> instrument): drain EP2-OUT, decode each 4-byte USB-MIDI event
 *     packet's CIN into a MIDI byte count, and write those bytes to the cable's
 *     UART THR.
 *   RX (instrument -> host): poll each cable's UART LSR; emit every received
 *     byte as a CIN=0xF single-byte passthrough packet on EP2-IN. (The spike
 *     does not parse the RX stream yet; Phase 3 adds a real parser.)
 *
 * Bring-up (board_init) and the UART register map are the hardware-validated
 * Phase-1 findings; see ../doc/hardware_register_map.md. Enumeration scaffolding
 * (usb.c, usb_descriptors.c, USBJmpTb.a51, reg_ezusb.h) is vendored from
 * src/ezusb-firmware (OpenULINK fork).
 */
#include "reg_ezusb.h"
#include "usb.h"
#include "delay.h"
#include "board_r1.h"
#include "uart.h"
#include "midi_config.h"

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
 * fw_entry does before any UART access. Hardware-validated in Phase 1 (the EP0
 * bus-probe loopback). See ../doc/hardware_register_map.md.
 */
static void board_init(void)
{
	/* CPUCS=0 clears CLK24OE (bit1), matching stock fw_entry from boot. */
	CPUCS = 0;

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

	/* PB4 = ST16C454 RESET (active-high). Drive it LOW to release the UARTs
	 * from reset: PB4=GPIO, latch 0, then enable the output driver. THIS is the
	 * Phase-1 unlock without which external writes never land. */
	PORTBCFG &= ~INT4;   /* PB4 = GPIO (clear INT4 alt-function) */
	OUTB     &= ~OUTB4;  /* PB4 output latch = 0 */
	OEB      |=  OEB4;   /* PB4 = driven output -> RESET de-asserted */

	/* 0xFE00.. is external SRAM bookkeeping (not a UART latch). Retained to
	 * match the bring-up state the Phase-1 loopback validated; not load-bearing. */
	*((__xdata uint8_t *)BOARD_GLUE_LATCH) = BOARD_GLUE_VALUE;
	*((__xdata uint8_t *)0xFE02) = 0x00;
	*((__xdata uint8_t *)0xFE01) = 0x10;
}

/* Host -> instrument: decode EP2-OUT USB-MIDI packets to UART THR writes. */
static void bridge_tx(void)
{
	uint8_t n, i, j, len, cn;

	if (!Semaphore_EP2_out)
		return;

	n = OUT2BC;                         /* bytes the host sent this packet */
	for (i = 0; (uint8_t)(i + 4) <= n; i += 4) {
		cn  = OUT2BUF[i] >> 4;          /* cable number = high nibble       */
		len = cin_len[OUT2BUF[i] & 0x0F];
		if (cn >= NUM_MIDI_PORTS)
			continue;                   /* no such cable -> drop            */
		for (j = 0; j < len; j++) {
			while (!uart_tx_ready(cn))
				;                        /* wait for THR empty (drains at baud) */
			uart_putc(cn, OUT2BUF[i + 1 + j]);
		}
	}

	Semaphore_EP2_out = 0;
	OUT2BC = 0;                         /* re-arm EP2-OUT to receive again  */
}

/* Instrument -> host: poll each cable's UART, emit single-byte EP2-IN packets. */
static void bridge_rx(void)
{
	uint8_t count = 0;
	uint8_t p;

	/* Only load a new IN packet once the previous one has been collected. */
	if (IN2CS & EPBSY)
		return;

	for (p = 0; p < NUM_MIDI_PORTS; p++) {
		while (uart_rx_ready(p) && count <= (MIDI_EP_MAXPKT - 4)) {
			uint8_t b = uart_getc(p);
			IN2BUF[count++] = (p << 4) | 0x0F;  /* CN=p, CIN=0xF single byte */
			IN2BUF[count++] = b;
			IN2BUF[count++] = 0;
			IN2BUF[count++] = 0;
		}
	}

	if (count)
		IN2BC = count;                  /* ship it (sets EPBSY)             */
}

void main(void)
{
	/* Bring up the external UART bus + clock, then init the 16550 channels. */
	board_init();
	uart_init();

	/* usb_init() performs RENUM re-enumeration: the device drops off the bus
	 * and reappears as 0x0A4E:0x10C1, a class-compliant MIDIStreaming device. */
	usb_init();

	/* Global interrupt enable - without this SUDAV never fires and enumeration
	 * times out. */
	EA = 1;

	/* Arm EP2-OUT to receive the first host packet (in case no SET_INTERFACE). */
	OUT2BC = 0;

	while (1) {
		bridge_tx();
		bridge_rx();
	}
}
