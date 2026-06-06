/*
 * uart.c - external 16550 (ST16C454) UART backend for MIDEX8 r1.
 * See uart.h for the op-set contract and bring-up preconditions.
 */
#include "uart.h"
#include "midi_config.h"

void uart_init(void)
{
	uint8_t port;

	for (port = 0; port < NUM_MIDI_PORTS; port++) {
		/* Mirror the stock fw 16550 init (FUN_CODE_1177), divisor 1:
		 *   LCR = 0x83  -> DLAB=1, 8N1
		 *   DLL = 1, DLM = 0  -> divisor 1 (500 kHz / 16 = 31250 baud)
		 *   LCR = 0x03  -> DLAB=0, 8N1
		 *   MCR = 0
		 * then, unlike stock, enable the 16-byte FIFOs (FCR=0x07) to relax
		 * the polling deadline once all 8 ports are swept (Phase 3). */
		UART_REG(port, UART_LCR) = BOARD_UART_LCR_DLAB;
		UART_REG(port, UART_DLL) = BOARD_UART_DIVISOR;
		UART_REG(port, UART_DLM) = 0x00;
		UART_REG(port, UART_LCR) = BOARD_UART_LCR_8N1;
		UART_REG(port, UART_MCR) = 0x00;
		UART_REG(port, UART_FCR) = BOARD_UART_FCR_ENABLE;
		/* IER stays 0: RX is polled via LSR (the PINSA IRQ-bitmap path is a
		 * deferred optimisation, see the register map doc). */
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
