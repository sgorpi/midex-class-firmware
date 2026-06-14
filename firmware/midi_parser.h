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
 * midi_parser.h - USB-MIDI 1.0 RX stream parser (instrument -> host).
 *
 * The bridge calls midi_rx_pump() each main-loop pass; the parser reassembles each
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
