/*
 * midi_parser.c - USB-MIDI 1.0 RX stream parser (instrument -> host).
 *
 * Phase 3: replaces the spike's CIN=0xF single-byte passthrough. Each UART
 * channel carries a raw MIDI byte stream; this parser reassembles it into
 * proper 4-byte USB-MIDI event packets (USB MIDI 1.0 spec, table 4-1) with the
 * correct Code Index Number, honouring:
 *   - channel voice message lengths (note off/on, poly-AT, CC = 2 data bytes;
 *     program change, channel-AT = 1; pitch bend = 2),
 *   - running status: a data byte arriving with no fresh status reuses the last
 *     channel voice status byte,
 *   - system common messages (MTC quarter frame F1, song position F2, song
 *     select F3, tune request F6) and their CINs; F1-F3/F6 cancel running
 *     status,
 *   - SysEx framed as CIN 0x4 (start/continue, 3 bytes per packet) chunks and a
 *     CIN 0x5/0x6/0x7 end packet carrying the trailing 1/2/3 bytes incl. F7,
 *   - system real-time bytes (0xF8-0xFF) which may interleave anywhere: each is
 *     emitted as a CIN 0xF single and never disturbs running status or SysEx.
 *
 * Parser state is per port (one independent stream each) and lives in XDATA;
 * the AN2131's internal RAM is reserved for the stack. midi_rx_pump() polls
 * every cable's UART, feeds received bytes through the parser into a device->host
 * ring, and ships EP2-IN bulk packets from the ring when the endpoint is free.
 */
#include "reg_ezusb.h"
#include "uart.h"
#include "midi_config.h"
#include <stdint.h>

/* Per-cable MIDI input parser state. */
struct port_parser {
	uint8_t status;   /* current status byte: channel-voice running status
			   * (0x80-0xEF) or an in-progress system-common status
			   * (0xF1-0xF3); 0 = none                              */
	uint8_t cin;      /* CIN to stamp on the next completed message         */
	uint8_t need;     /* data bytes required to complete it (1 or 2)        */
	uint8_t have;     /* data bytes accumulated so far (0 or 1)             */
	uint8_t d0;       /* first accumulated data byte (when need == 2)       */
	uint8_t in_sysex; /* 1 while between F0 and F7                          */
	uint8_t sxn;      /* bytes buffered toward the current SysEx chunk (0-2)*/
	uint8_t sx[3];    /* the buffered SysEx bytes                           */
};

static __xdata struct port_parser parser[NUM_MIDI_PORTS];

/* Device->host ring of parsed 4-byte USB-MIDI packets. midi_rx_pump always
 * drains the UARTs into this and ships from it when the endpoint is free, so
 * EP2-IN back-pressure never blocks RX servicing. */
static __xdata uint8_t rx_ring[MIDI_RX_RING_SIZE];
static uint8_t rx_head;            /* write index */
static uint8_t rx_tail;            /* read index  */

#define RX_RING_MASK (MIDI_RX_RING_SIZE - 1)

/* Bytes currently queued in the ring (multiple of 4). */
static uint8_t rx_ring_count(void)
{
	return (uint8_t)(rx_head - rx_tail) & RX_RING_MASK;
}

void midi_parser_reset(void)
{
	uint8_t p;

	for (p = 0; p < NUM_MIDI_PORTS; p++) {
		parser[p].status = 0;
		parser[p].have = 0;
		parser[p].in_sysex = 0;
		parser[p].sxn = 0;
	}
	rx_head = 0;                /* XDATA is not auto-zeroed on this target */
	rx_tail = 0;
}

/* Append one 4-byte USB-MIDI event packet (cable cn) to the device->host ring.
 * The caller (midi_rx_pump) guarantees room for 4 bytes before parsing a byte. */
static void emit(uint8_t cn, uint8_t cin, uint8_t b0, uint8_t b1, uint8_t b2)
{
	rx_ring[rx_head] = (cn << 4) | cin; rx_head = (rx_head + 1) & RX_RING_MASK;
	rx_ring[rx_head] = b0;              rx_head = (rx_head + 1) & RX_RING_MASK;
	rx_ring[rx_head] = b1;              rx_head = (rx_head + 1) & RX_RING_MASK;
	rx_ring[rx_head] = b2;              rx_head = (rx_head + 1) & RX_RING_MASK;
}

/* Feed one received byte from cable cn through its parser. Emits 0 or 1 packet. */
static void parse_byte(uint8_t cn, uint8_t b)
{
	__xdata struct port_parser *ps = &parser[cn];

	/* System real-time (0xF8-0xFF): single byte, may interleave anywhere;
	 * does not affect running status or an in-progress SysEx. */
	if (b >= 0xF8) {
		emit(cn, 0x0F, b, 0, 0);
		return;
	}

	if (b >= 0x80) {
		/* --- Status byte --- */
		if (b == 0xF0) {
			/* SysEx start: begin buffering; F0 is the first byte. */
			ps->status = 0;
			ps->have = 0;
			ps->in_sysex = 1;
			ps->sx[0] = 0xF0;
			ps->sxn = 1;
			return;
		}
		if (b == 0xF7) {
			/* SysEx end: flush the partial chunk with F7 appended. */
			if (ps->in_sysex) {
				ps->sx[ps->sxn++] = 0xF7;
				/* sxn 1 -> CIN 0x5, 2 -> 0x6, 3 -> 0x7. */
				emit(cn, (uint8_t)(0x04 + ps->sxn), ps->sx[0],
				     ps->sxn > 1 ? ps->sx[1] : 0,
				     ps->sxn > 2 ? ps->sx[2] : 0);
				ps->in_sysex = 0;
				ps->sxn = 0;
			} else {
				/* Stray F7 (no open SysEx): emit as a single. */
				emit(cn, 0x05, 0xF7, 0, 0);
			}
			ps->status = 0;
			return;
		}
		/* Any other status aborts an in-progress SysEx. */
		ps->in_sysex = 0;
		ps->sxn = 0;
		ps->have = 0;
		if (b < 0xF0) {
			/* Channel voice 0x80-0xEF: CIN = high nibble; sets
			 * running status. Program change (0xC) / channel-AT
			 * (0xD) take one data byte, the rest take two. */
			ps->status = b;
			ps->cin = b >> 4;
			ps->need = (ps->cin == 0x0C || ps->cin == 0x0D) ? 1 : 2;
		} else {
			/* System common 0xF1-0xF6 (no running status). */
			switch (b) {
			case 0xF1:           /* MTC quarter frame: 1 data byte */
			case 0xF3:           /* song select:       1 data byte */
				ps->status = b;
				ps->cin = 0x02;
				ps->need = 1;
				break;
			case 0xF2:           /* song position:     2 data bytes*/
				ps->status = b;
				ps->cin = 0x03;
				ps->need = 2;
				break;
			default:             /* F6 tune req + undefined F4/F5  */
				emit(cn, 0x05, b, 0, 0);
				ps->status = 0;
				break;
			}
		}
		return;
	}

	/* --- Data byte 0x00-0x7F --- */
	if (ps->in_sysex) {
		ps->sx[ps->sxn++] = b;
		if (ps->sxn == 3) {
			emit(cn, 0x04, ps->sx[0], ps->sx[1], ps->sx[2]);
			ps->sxn = 0;
		}
		return;
	}
	if (ps->status == 0)
		return;                  /* data with no status -> junk, drop */

	if (ps->need == 2 && ps->have == 0) {
		ps->d0 = b;              /* buffer first of two data bytes     */
		ps->have = 1;
		return;
	}
	/* Message complete. USB-MIDI packets are self-contained: MIDI_0 is always
	 * the status byte (running status is expanded here), MIDI_1/2 the data. */
	if (ps->need == 1)
		emit(cn, ps->cin, ps->status, b, 0);
	else
		emit(cn, ps->cin, ps->status, ps->d0, b);
	ps->have = 0;
	if (ps->status >= 0xF0)
		ps->status = 0;          /* system common: no running status  */
	/* channel voice: status/cin/need persist for running status        */
}

void midi_rx_pump(void)
{
	uint8_t p, n;

	/*
	 * Always drain the UARTs first -- even while EP2-IN is busy -- so EP2-IN
	 * back-pressure never blocks RX servicing. Parsed packets queue in rx_ring;
	 * stop pulling from a UART once the ring lacks room for another packet.
	 * (The ST16C454 has no RX FIFO -- a 1-byte RHR -- so a sustained stream can
	 * still overrun if the main loop is delayed; see uart.c TODO re: timer-ISR
	 * RX.)
	 */
	for (p = 0; p < NUM_MIDI_PORTS; p++) {
		while (uart_rx_ready(p)) {
			if (rx_ring_count() > (uint8_t)(MIDI_RX_RING_SIZE - 1 - 4))
				break;
			parse_byte(p, uart_getc(p));
		}
	}

	/* Ship a packet to the host only when the endpoint is free. */
	if (IN2CS & EPBSY)
		return;
	n = 0;
	while (rx_ring_count() >= 4 && n <= (uint8_t)(MIDI_EP_MAXPKT - 4)) {
		IN2BUF[n++] = rx_ring[rx_tail]; rx_tail = (rx_tail + 1) & RX_RING_MASK;
		IN2BUF[n++] = rx_ring[rx_tail]; rx_tail = (rx_tail + 1) & RX_RING_MASK;
		IN2BUF[n++] = rx_ring[rx_tail]; rx_tail = (rx_tail + 1) & RX_RING_MASK;
		IN2BUF[n++] = rx_ring[rx_tail]; rx_tail = (rx_tail + 1) & RX_RING_MASK;
	}
	if (n)
		IN2BC = n;               /* ship it (sets EPBSY)              */
}
