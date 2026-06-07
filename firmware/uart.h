/*
 * uart.h - external 16550 (ST16C454) UART backend for MIDEX8 r1.
 *
 * This is the firmware's informal "UART op-set" seam (see the plan): the MIDI
 * bridge in main.c talks only to these five operations, so r2 can reuse them via
 * board config and a future MIDEX3 / r2-extra-port backend can re-implement them
 * over on-chip serial without touching the bridge.
 *
 * Preconditions: board_init() must have run first (RD#/WR# strobes, the Timer2
 * -> PB7 500 kHz XIN clock, and the PB4 RESET de-assert). See board.c / the
 * register map doc. uart_init() then programs each channel's 16550 registers.
 */
#ifndef UART_H
#define UART_H

#include <stdint.h>
#include <stdbool.h>
#include "board_r1.h"

/* A channel's register block lives at BOARD_UART_BASE + port*BOARD_UART_STRIDE
 * in 8051 XDATA; registers are added as the standard 16550 datasheet offsets. */
#define UART_REG(port, off) \
	(*((__xdata volatile uint8_t *)(BOARD_UART_BASE + \
	 (uint16_t)(port) * BOARD_UART_STRIDE + (off))))

/* Initialise every channel 0..NUM_MIDI_PORTS-1: 8N1, divisor 1 (31250 baud from
 * the 500 kHz XIN). The ST16C454 is a 16C450-class part with no FIFO (single-
 * byte RHR). LCR writes are read-back verified (see uart.c). Call only after the
 * late UART bring-up releases reset; configuring at power-on is marginal (see
 * main.c uart_bringup). */
void uart_init(void);

/* TX: true when the channel's transmit holding register can accept a byte. */
bool uart_tx_ready(uint8_t port);

/* TX: push one byte (caller should gate on uart_tx_ready first). */
void uart_putc(uint8_t port, uint8_t b);

/* RX: true when the channel has a received byte waiting. */
bool uart_rx_ready(uint8_t port);

/* RX: read one received byte (caller should gate on uart_rx_ready first). */
uint8_t uart_getc(uint8_t port);

#endif /* UART_H */
