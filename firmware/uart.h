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

/* ---- RX capture (timer-ISR backed) ------------------------------------- */
/* The high-priority Timer0 ISR (uart_rx_isr, in uart.c; its vector is declared
 * in main.c) is the SOLE reader of the chip RHR: every ~100 us it pushes each
 * port's received byte into a per-port software FIFO. The bridge consumes those
 * bytes through the two ops below -- they touch the FIFO, NOT the chip. This is
 * the r1 backend's RX half of the op-set seam; an r2/MIDEX3 backend can capture
 * differently behind the same uart_rx_available/uart_rx_dequeue contract. */

/* Zero the per-port RX FIFOs + the overflow counter. XDATA is not auto-zeroed on
 * this target; call after usb_init (which frees/clobbers the 0x2000 region) and
 * before uart_rx_start. */
void uart_rx_reset(void);

/* Program + enable the Timer0 RX-capture tick (mode 2, ~100 us, high priority).
 * Call after uart_rx_reset (FIFOs valid) and uart_bringup (UART configured). */
void uart_rx_start(void);

/* RX consume: true if port has a captured byte waiting in its FIFO. */
bool uart_rx_available(uint8_t port);

/* RX consume: pop one captured byte (gate on uart_rx_available first). */
uint8_t uart_rx_dequeue(uint8_t port);

/* Saturating count of bytes the capture ISR dropped because a port FIFO was full
 * (0 on a healthy run). Read/cleared by the host via the vendor control request
 * (see usb.c). */
extern volatile uint8_t uart_rx_overflows;

/* Vendor control-request codes the host uses to read/clear uart_rx_overflows.
 * Vendor type, so the OS class driver ignores them -- device stays class
 * compliant; only an explicit libusb/pyusb transfer sees them. */
#define VENDOR_REQ_GET_RX_OVERFLOWS  0x01   /* IN,  returns 1 byte           */
#define VENDOR_REQ_CLR_RX_OVERFLOWS  0x02   /* OUT, clears the counter        */

#endif /* UART_H */
