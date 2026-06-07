/*
 * midi_parser.h - USB-MIDI 1.0 RX stream parser (instrument -> host).
 *
 * Phase 3 replacement for the spike's CIN=0xF single-byte passthrough. The
 * bridge calls midi_rx_pump() each main-loop pass; the parser reassembles each
 * UART channel's raw MIDI byte stream into properly framed 4-byte USB-MIDI
 * event packets and ships them on EP2-IN. See midi_parser.c for the framing
 * rules. Per-port state lives in XDATA; midi_parser_reset() clears it at boot.
 */
#ifndef MIDI_PARSER_H
#define MIDI_PARSER_H

/* Clear every cable's parser state (call once after uart_init). */
void midi_parser_reset(void);

/* Poll all cables, parse received MIDI bytes, ship one EP2-IN bulk packet. */
void midi_rx_pump(void);

#endif /* MIDI_PARSER_H */
