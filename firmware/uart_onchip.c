/*
 * uart_onchip.c - EZ-USB FX on-chip UART backend (MIDEX8 r2 ports 6 & 7).
 * See uart_onchip.h for the contract and the polled-vs-interrupt-driven model.
 *
 * The RX-capture half lives in the shared uart.c Timer0 ISR (it polls RI and
 * reads SBUF0/SBUF1 inline). This file owns only init + the polled TX ops.
 */
#include "reg_ezusb.h"
#include "uart_onchip.h"

void uart_onchip_init(void)
{
	/* UART0 + UART1: mode 1 (8-bit, Timer1 baud), receiver enabled. */
	SCON0 = BOARD_SCON_MODE1_REN;
	SCON1 = BOARD_SCON_MODE1_REN;

	/* Timer1 = mode 2 (8-bit auto-reload) baud generator, clocked CLKOUT/4
	 * (CKCON.T1M=1). Leave the Timer0 nibble (the RX-capture tick) untouched. */
	TMOD = (TMOD & 0x0F) | M11;   /* timer1 mode 2 (M11=1, M10=0)            */
	CKCON |= T1M;                 /* timer1 clock = CLKOUT/4                  */
	TH1 = BOARD_ONCHIP_T1_RELOAD;
	TL1 = BOARD_ONCHIP_T1_RELOAD;

	/* Baud doubling (SMOD): set at 24 MHz, clear at 48 MHz, so the same
	 * reload yields 31250 baud on either core-clock strap. SMOD0 is PCON.7
	 * (UART0); SMOD1 is EICON.7 (UART1). CPUCS.3 is the read-only 24/48 strap
	 * (reads 0 on the AN2131, so this is r2-only behaviour). */
	if (CPUCS & BOARD_CPUCS_48MHZ) {
		PCON &= ~SMOD0;
		SMOD1 = 0;
	} else {
		PCON |= SMOD0;
		SMOD1 = 1;
	}

	TR1 = 1;          /* run the shared baud timer                          */
	ES0 = 0;          /* polled model: no on-chip-UART RX/TX interrupts      */
	ES1 = 0;

	/* Seed TX-ready: TI=1 means "holding register empty", so the first
	 * uart_onchip_putc is allowed (the UART clears/sets TI thereafter). */
	TI_0 = 1;
	TI_1 = 1;
}

bool uart_onchip_tx_ready(uint8_t port)
{
	return (port == BOARD_ONCHIP_PORT_FIRST) ? (bool)TI_0 : (bool)TI_1;
}

void uart_onchip_putc(uint8_t port, uint8_t b)
{
	/* Clear TI, then load SBUF; the UART re-sets TI when the byte has shifted
	 * out (observed by the next uart_onchip_tx_ready poll). */
	if (port == BOARD_ONCHIP_PORT_FIRST) {
		TI_0 = 0;
		SBUF0 = b;
	} else {
		TI_1 = 0;
		SBUF1 = b;
	}
}
