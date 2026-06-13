/*
 * uart_ext.h - external 16550 (ST16C454 / ST16C452) UART backend.
 *
 * One of the two backends behind the uart.c op-set dispatcher (the other is
 * uart_onchip.c). Drives MIDI ports 0 .. BOARD_NUM_EXT_PORTS-1, each a 16550
 * channel memory-mapped at BOARD_UART_BASE + port*BOARD_UART_STRIDE in XDATA.
 *
 * On r1 this is the only backend (all 8 ports). On r2 it covers ports 0-5; the
 * shared Timer0 RX-capture ISR in uart.c reads these channels' RHRs through the
 * UART_REG macro below (kept here so the ISR and this backend share one
 * definition).
 *
 * Preconditions: board_init() (RD#/WR# strobes, clock) and the external-UART
 * RESET release must have run first. See main.c board_init / uart_bringup.
 */
#ifndef UART_EXT_H
#define UART_EXT_H

#include <stdint.h>
#include <stdbool.h>
#include "board.h"

/* A channel's register block lives at BOARD_UART_BASE + port*BOARD_UART_STRIDE
 * in 8051 XDATA; registers are added as the standard 16550 datasheet offsets. */
#define UART_REG(port, off) \
	(*((__xdata volatile uint8_t *)(BOARD_UART_BASE + \
	 (uint16_t)(port) * BOARD_UART_STRIDE + (off))))

/* Initialise external channels 0 .. BOARD_NUM_EXT_PORTS-1: 8N1, BOARD_UART_DIVISOR
 * (31250 baud). The ST16C45x are 16C450-class parts with no FIFO (single-byte
 * RHR). LCR writes are read-back verified. Call only after the late UART
 * bring-up releases reset. */
void uart_ext_init(void);

/* TX: true when the channel's transmit holding register can accept a byte. */
bool uart_ext_tx_ready(uint8_t port);

/* TX: push one byte (caller gates on uart_ext_tx_ready first). */
void uart_ext_putc(uint8_t port, uint8_t b);

#endif /* UART_EXT_H */
