/*
 * uart.c - UART op-set dispatcher + shared RX-capture core.
 * See uart.h for the public op-set contract.
 *
 * The per-port operations (init / TX-ready / putc) are split into two backends
 * behind this dispatcher: uart_ext.c (external 16550, ports < ONCHIP_PORT_FIRST)
 * and, on r2, uart_onchip.c (FX on-chip UART0/UART1, the remaining ports).
 *
 * The RX-capture half is NOT split: it is a single, self-contained, high-
 * priority Timer0 ISR that must read every port in one pass (external RHRs via
 * MOVX and, on r2, the on-chip SBUF/SBUF1). Splitting it across the backend
 * files would force shared non-reentrant calls from the ISR, which risks SDCC
 * overlay corruption when it preempts the main loop -- so it lives here in the
 * shared core, deliberately spanning both backends (same self-contained
 * rationale as the original r1 ISR). The per-port software FIFOs it fills, and
 * the bridge's RX-consume ops, live here too.
 */
#include "reg_ezusb.h"
#include "uart.h"
#include "uart_ext.h"
#if BOARD_HAS_ONCHIP_UART
#include "uart_onchip.h"
#endif
#include "midi_config.h"

/* ---- Op-set dispatcher -------------------------------------------------- */

void uart_init(void)
{
	uart_ext_init();
#if BOARD_HAS_ONCHIP_UART
	uart_onchip_init();
#endif
}

bool uart_tx_ready(uint8_t port)
{
#if BOARD_HAS_ONCHIP_UART
	if (port >= BOARD_ONCHIP_PORT_FIRST)
		return uart_onchip_tx_ready(port);
#endif
	return uart_ext_tx_ready(port);
}

void uart_putc(uint8_t port, uint8_t b)
{
#if BOARD_HAS_ONCHIP_UART
	if (port >= BOARD_ONCHIP_PORT_FIRST) {
		uart_onchip_putc(port, b);
		return;
	}
#endif
	uart_ext_putc(port, b);
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
	/* Timer0 mode 2 (8-bit auto-reload), leave the Timer1 nibble untouched
	 * (on r2 Timer1 is the on-chip-UART baud gen). CKCON T0M is already 0 from
	 * board_init -> Fosc/12 = 0.5 us/tick. */
	TMOD = (TMOD & 0xF0) | 0x02;
	TH0 = BOARD_T0_RELOAD;     /* reload value (auto-reloaded by hardware)   */
	TL0 = BOARD_T0_RELOAD;     /* initial count                              */
	rx_tick_prescale = 1;      /* fire the first capture sweep on next tick  */
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

/* Push one already-read byte into a port's FIFO, or count an overflow if full.
 * Macro (not a function) so the self-contained ISR makes no shared call; uses
 * the ISR-local `head`. The caller must have already consumed the source
 * register (RHR/SBUF) so the byte is read even when the FIFO is full (clears the
 * chip's DR / the UART's RI). */
#define RX_STORE(p, byte) do {                                  \
		head = (uint8_t)(rx_fifo_head[p] + 1) & RX_FIFO_MASK;   \
		if (head == rx_fifo_tail[p]) {                          \
			if (uart_rx_overflows != 0xFF)                      \
				uart_rx_overflows++;                            \
		} else {                                                \
			rx_fifo[p][rx_fifo_head[p]] = (byte);               \
			rx_fifo_head[p] = head;                             \
		}                                                       \
	} while (0)

/*
 * Timer0 ISR = high-priority RX capture (the fix for sustained-RX overrun). The
 * hardware tick is ~93 us; a /BOARD_T0_PRESCALE software prescaler runs the
 * actual sweep every ~279 us. On a sweep it reads every port whose UART reports
 * a received byte and pushes it into that port's software FIFO. Servicing the
 * FIFO-less RHR / single-byte SBUF on this hard cadence (well inside the overrun
 * window) makes a sustained stream (long SysEx) immune to main-loop / USB-ISR
 * delay.
 *
 * Self-contained ON PURPOSE: it reads the chips via the UART_REG macro / SBUF
 * SFRs and writes the FIFO inline (RX_STORE is a macro), calling NO function the
 * main loop also calls -- a shared non-reentrant call would risk SDCC overlay
 * corruption when this ISR preempts the main loop. __using 1 gives it a private
 * register bank (bank 0 = main/USB). Mode-2 auto-reload clears TF0 on vector
 * entry, so there is no manual ack.
 */
void uart_rx_isr(void) __interrupt TF0_VECTOR __using 1
{
	uint8_t port;
	uint8_t head;
	uint8_t b;

	if (--rx_tick_prescale != 0)
		return;                  /* not a sweep tick: cheap early-out  */
	rx_tick_prescale = BOARD_T0_PRESCALE;

	/* External 16550 channels (all ports on r1; ports 0..5 on r2). */
	for (port = 0; port < BOARD_ONCHIP_PORT_FIRST; port++) {
		if ((UART_REG(port, UART_LSR) & UART_LSR_DR) == 0)
			continue;
		b = UART_REG(port, UART_RHR);   /* read -> clears DR even if full */
		RX_STORE(port, b);
	}

#if BOARD_HAS_ONCHIP_UART
	/* On-chip UART0 -> port ONCHIP_PORT_FIRST. Poll RI (no serial IRQ in the
	 * polled model); read SBUF within ~320 us of arrival (single-byte buffer). */
	if (RI_0) {
		b = SBUF0;
		RI_0 = 0;
		RX_STORE(BOARD_ONCHIP_PORT_FIRST, b);
	}
	/* On-chip UART1 -> port ONCHIP_PORT_FIRST + 1. */
	if (RI_1) {
		b = SBUF1;
		RI_1 = 0;
		RX_STORE(BOARD_ONCHIP_PORT_FIRST + 1, b);
	}
#endif
}
