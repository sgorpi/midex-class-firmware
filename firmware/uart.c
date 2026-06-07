/*
 * uart.c - external 16550 (ST16C454) UART backend for MIDEX8 r1.
 * See uart.h for the op-set contract and bring-up preconditions.
 */
#include "reg_ezusb.h"
#include "uart.h"
#include "midi_config.h"
#include "delay.h"

/* Write LCR and read it back, retrying until it latches (or the cap is hit).
 * Defensive against any residual marginality in the external write glue; with
 * the late bring-up (see main.c uart_bringup) the first write normally sticks. */
static void uart_write_lcr(uint8_t port, uint8_t val)
{
	uint8_t t;

	for (t = 0; t < BOARD_UART_LCR_MAX_TRIES; t++) {
		UART_REG(port, UART_LCR) = val;
		if (UART_REG(port, UART_LCR) == val)
			return;
		delay_us(10);
	}
}

void uart_init(void)
{
	uint8_t port;

	for (port = 0; port < NUM_MIDI_PORTS; port++) {
		/* 16C450-class init, divisor 1 (500 kHz XIN / 16 = 31250 baud):
		 *   LCR = 0x83  -> DLAB=1, 8N1
		 *   DLL = 1, DLM = 0  -> divisor 1
		 *   LCR = 0x03  -> DLAB=0, 8N1
		 *   MCR = 0
		 * The two LCR writes are read-back verified: the DLAB-set so the divisor
		 * writes are guaranteed to hit the divisor latch (not THR/IER), and the
		 * final 8N1 so the line config is correct.
		 *
		 * NOTE: the ST16C454 is a 16C450-class part with NO FIFO (datasheet:
		 * only 12 registers, offset 2 = ISR on read / nothing on write). RX is a
		 * single-byte RHR, so it must be read within one byte time (~320 us @
		 * 31250 baud) or it overruns. The main-loop poll keeps up for normal
		 * traffic but can be starved by USB ISRs on a *sustained* stream (long
		 * SysEx), dropping a byte. TODO: drain RHR from a timer ISR (as stock
		 * does) to make sustained RX overrun-proof. */
		uart_write_lcr(port, BOARD_UART_LCR_DLAB);
		UART_REG(port, UART_DLL) = BOARD_UART_DIVISOR;
		UART_REG(port, UART_DLM) = 0x00;
		uart_write_lcr(port, BOARD_UART_LCR_8N1);
		UART_REG(port, UART_MCR) = 0x00;
		/* IER stays 0: RX is polled (the PINSA IRQ-bitmap path is a deferred
		 * optimisation, see the register map doc). */
		UART_REG(port, UART_IER) = 0x00;
	}
}

bool uart_tx_ready(uint8_t port)
{
	return (UART_REG(port, UART_LSR) & UART_LSR_THRE) != 0;
}

void uart_putc(uint8_t port, uint8_t b)
{
	UART_REG(port, UART_THR) = b;
}

bool uart_rx_ready(uint8_t port)
{
	return (UART_REG(port, UART_LSR) & UART_LSR_DR) != 0;
}

uint8_t uart_getc(uint8_t port)
{
	return UART_REG(port, UART_RHR);
}
