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
 * uart_ext.c - external 16550 (ST16C454 / ST16C452) UART backend.
 * See uart_ext.h for the contract and bring-up preconditions.
 *
 * Extracted from the original single-backend uart.c; the RX-capture half (the
 * Timer0 ISR + per-port FIFOs) now lives in the shared uart.c core, which reads
 * these channels' RHRs via the UART_REG macro in uart_ext.h.
 */
#include "uart_ext.h"
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

void uart_ext_init(void)
{
	uint8_t port;

	for (port = 0; port < BOARD_NUM_EXT_PORTS; port++) {
		/* 16C450-class init, BOARD_UART_DIVISOR (XIN / 16 = 31250 baud):
		 *   LCR = 0x83  -> DLAB=1, 8N1
		 *   DLL = divisor, DLM = 0
		 *   LCR = 0x03  -> DLAB=0, 8N1
		 *   MCR = 0
		 * The two LCR writes are read-back verified: DLAB-set so the divisor
		 * writes hit the divisor latch (not THR/IER), and the final 8N1 so the
		 * line config is correct.
		 *
		 * The ST16C45x are 16C450-class (no FIFO): RX is a single-byte RHR, so
		 * it must be read within ~320 us @ 31250 baud or it overruns. The
		 * high-priority Timer0 capture ISR (uart_rx_isr in uart.c) services it
		 * every ~279 us. IER stays 0 (no chip RX interrupt; the timer poll
		 * drives capture). */
		uart_write_lcr(port, BOARD_UART_LCR_DLAB);
		UART_REG(port, UART_DLL) = BOARD_UART_DIVISOR;
		UART_REG(port, UART_DLM) = 0x00;
		uart_write_lcr(port, BOARD_UART_LCR_8N1);
		UART_REG(port, UART_MCR) = 0x00;
		UART_REG(port, UART_IER) = 0x00;
	}
}

bool uart_ext_tx_ready(uint8_t port)
{
	return (UART_REG(port, UART_LSR) & UART_LSR_THRE) != 0;
}

void uart_ext_putc(uint8_t port, uint8_t b)
{
	UART_REG(port, UART_THR) = b;
}
