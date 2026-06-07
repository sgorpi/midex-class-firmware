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
		 * NOTE: the ST16C454 is a 16C450-class part with NO FIFO
		 * (datasheet: 12 registers, offset 2 = ISR on read / nothing
		 * on write). RX is a single-byte RHR, so it must be read within
		 * ~320 us @ 31250 baud or it overruns. The high-priority Timer0
		 * capture ISR (uart_rx_isr) services it every ~100 us, which
		 * makes even sustained RX overrun-proof. IER stays 0 (no chip
		 * RX interrupt; the timer poll drives capture). */
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

/* ---- RX capture: per-port software FIFO filled by the Timer0 ISR -------- */
/* SPSC: uart_rx_isr writes head, the main loop (uart_rx_dequeue) writes tail.
 * Each index is a single byte (atomic R/W on the 8051), so no locking is needed.
 * One slot is reserved to tell full from empty. XDATA is not auto-zeroed (see
 * uart_rx_reset). */
static __xdata uint8_t rx_fifo[NUM_MIDI_PORTS][MIDEX_RX_FIFO_SIZE];
static __xdata uint8_t rx_fifo_head[NUM_MIDI_PORTS];   /* ISR writes  */
static __xdata uint8_t rx_fifo_tail[NUM_MIDI_PORTS];   /* main writes */

#define RX_FIFO_MASK (MIDEX_RX_FIFO_SIZE - 1)

volatile uint8_t uart_rx_overflows;

/* Software /N prescaler for the Timer0 tick (ISR-private; see uart_rx_isr and
 * BOARD_T0_PRESCALE). Written only by uart_rx_start (before the timer runs) and
 * the ISR, so no concurrency. XDATA is not auto-zeroed -> initialised in
 * uart_rx_start. */
static __xdata uint8_t rx_tick_prescale;

void uart_rx_reset(void)
{
	uint8_t port;

	for (port = 0; port < NUM_MIDI_PORTS; port++) {
		rx_fifo_head[port] = 0;
		rx_fifo_tail[port] = 0;
	}
	uart_rx_overflows = 0;
}

void uart_rx_start(void)
{
	/* Timer0 mode 2 (8-bit auto-reload), leave the Timer1 nibble untouched.
	 * CKCON T0M is already 0 from board_init -> Fosc/12 = 0.5 us/tick. */
	TMOD = (TMOD & 0xF0) | 0x02;
	TH0 = BOARD_T0_RELOAD;     /* reload value (auto-reloaded by hardware)   */
	TL0 = BOARD_T0_RELOAD;     /* initial count                              */
	rx_tick_prescale = 1;      /* fire the first LSR sweep on the next tick   */
	PT0 = 1;                   /* high priority: preempt the USB interrupt   */
	ET0 = 1;                   /* enable Timer0 interrupt                     */
	TR0 = 1;                   /* run                                         */
}

bool uart_rx_available(uint8_t port)
{
	return rx_fifo_head[port] != rx_fifo_tail[port];
}

uint8_t uart_rx_dequeue(uint8_t port)
{
	uint8_t b = rx_fifo[port][rx_fifo_tail[port]];

	rx_fifo_tail[port] = (rx_fifo_tail[port] + 1) & RX_FIFO_MASK;
	return b;
}

/*
 * Timer0 ISR = high-priority RX capture (the fix for sustained-RX overrun). The
 * hardware tick is ~93 us; a /BOARD_T0_PRESCALE software prescaler runs the actual
 * sweep every ~279 us. On a sweep it visits every port and, for each whose 16550
 * reports a received byte (LSR Data Ready), reads the single-byte RHR and pushes
 * it into that port's software FIFO. Servicing the FIFO-less ST16C454 RHR on this
 * hard ~279 us cadence (well inside the ~640 us RHR+shift overrun window) makes it
 * immune to main-loop / low-priority-USB-ISR delay, so a sustained stream (long
 * SysEx) can no longer overrun the RHR.
 *
 * The prescaler exists because the 8-port sweep is ~100 us (per-port XDATA-address
 * recompute, x8); a flat 93 us tick nearly saturated the CPU and tripled latency.
 * The 2-of-3 ticks that skip the sweep return after the cheap decrement below.
 * (Cheaper still, later: the PINSA RX-bitmap pre-filter -- see board_r1.h.)
 *
 * Self-contained ON PURPOSE: it reads the chip via the UART_REG macro and writes
 * the FIFO inline, calling NO function the main loop also calls -- a shared
 * non-reentrant call would risk SDCC overlay corruption when this ISR preempts
 * the main loop. __using 1 gives it a private register bank (bank 0 = main/USB).
 * Mode-2 auto-reload clears TF0 on vector entry, so there is no manual ack.
 */
void uart_rx_isr(void) __interrupt TF0_VECTOR __using 1
{
	uint8_t port;
	uint8_t head;

	if (--rx_tick_prescale != 0)
		return;                  /* not a sweep tick: cheap early-out  */
	rx_tick_prescale = BOARD_T0_PRESCALE;

	for (port = 0; port < NUM_MIDI_PORTS; port++) {
		if ((UART_REG(port, UART_LSR) & UART_LSR_DR) == 0)
			continue;
		head = (rx_fifo_head[port] + 1) & RX_FIFO_MASK;
		if (head == rx_fifo_tail[port]) {
			/* FIFO full: read RHR anyway to clear DR (discard the
			 * byte) and bump the saturating overflow counter. */
			(void)UART_REG(port, UART_RHR);
			if (uart_rx_overflows != 0xFF)
				uart_rx_overflows++;
		} else {
			rx_fifo[port][rx_fifo_head[port]] =
				UART_REG(port, UART_RHR);
			rx_fifo_head[port] = head;
		}
	}
}
