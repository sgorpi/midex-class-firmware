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
 * uart_onchip.h - EZ-USB FX on-chip UART backend (MIDEX8 r2 ports 6 & 7).
 *
 * The second backend behind the uart.c op-set dispatcher (the first is
 * uart_ext.c). Drives the two MIDI ports BOARD_ONCHIP_PORT_FIRST (UART0,
 * PC0=RxD0/PC1=TxD0) and BOARD_ONCHIP_PORT_FIRST+1 (UART1, PB2=RxD1/PB3=TxD1).
 * Only compiled/linked when BOARD_HAS_ONCHIP_UART (r2); MIDEX3 will reuse it.
 *
 * Model: POLLED, symmetric with the external backend. TX is gated on the UART's
 * TI flag in the main-loop pump; RX is captured by the shared Timer0 ISR in
 * uart.c, which polls RI and reads SBUF/SBUF1 directly (no serial interrupts).
 *
 *   A possible throughput optimisation (see ../doc/midex8_r1_vs_r2.md) is the
 *   stock interrupt-driven model -- enable ES0/ES1, let the serial ISRs flag
 *   RX + reload TX, and move the SBUF read out of the Timer0 ISR. That would
 *   remove the ~279 us single-byte-SBUF capture latency at the cost of two ISRs
 *   and cross-bank flag plumbing; the polled model measures well within the UART
 *   line-rate ceiling, so it is not used.
 */
#ifndef UART_ONCHIP_H
#define UART_ONCHIP_H

#include <stdint.h>
#include <stdbool.h>
#include "board.h"

/* Configure UART0 + UART1: mode 1, REN=1, shared Timer1 baud gen (31250 baud,
 * clock-agnostic via the CPUCS.3 24/48 MHz strap). Seeds each TI flag so the
 * first uart_onchip_putc is allowed. Call from the late UART bring-up. */
void uart_onchip_init(void);

/* TX: true when the on-chip UART for this port can accept a byte (TI set). */
bool uart_onchip_tx_ready(uint8_t port);

/* TX: push one byte (caller gates on uart_onchip_tx_ready first). */
void uart_onchip_putc(uint8_t port, uint8_t b);

#endif /* UART_ONCHIP_H */
