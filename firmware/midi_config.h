/*
 * midi_config.h - build-time configuration for the class-compliant MIDI bridge.
 *
 * NUM_MIDI_PORTS is the single knob that scales the firmware:
 *   - Phase 2 spike: 2 (proves the 0x4040 + port*8 per-port stride end-to-end)
 *   - Phase 3 full : 8 (all r1 ports)
 * It must stay in sync with the hand-packed descriptor block in usb_descriptors.c
 * (a _Static_assert there guards the config wTotalLength).
 */
#ifndef MIDI_CONFIG_H
#define MIDI_CONFIG_H

#define NUM_MIDI_PORTS   8

/* USB-MIDI bulk endpoints (the OpenULINK skeleton already configures EP2). The
 * host->device (TX) stream arrives on EP2-OUT; device->host (RX) goes on EP2-IN.
 * See the endpoint decision in the plan (EP2-IN + EP2-OUT pair). */
#define MIDI_EP_OUT_ADDR  0x02   /* bulk OUT: host -> device (-> UART THR)   */
#define MIDI_EP_IN_ADDR   0x82   /* bulk IN : device -> host (<- UART RHR)   */
#define MIDI_EP_MAXPKT    64

/* Per-port host->device TX ring buffer (bytes). EP2-OUT is drained into these
 * rings and re-armed immediately; a non-blocking pump feeds the 31250-baud
 * UARTs from them. This decouples EP servicing from the slow UART so bursts
 * (chords, running status, SysEx) are not lost and midi_rx_pump is never
 * starved. MUST be a power of two (index masking). 8 ports * 128 = 1 KB XDATA. */
#define MIDI_TX_RING_SIZE  128

/* Device->host ring (bytes) holding parsed 4-byte USB-MIDI packets while EP2-IN
 * is busy. midi_rx_pump always drains the UARTs into this ring and ships from it
 * when the endpoint is free, so EP2-IN back-pressure never blocks RX servicing.
 * (The ST16C454 itself has no RX FIFO -- a 1-byte RHR -- so a *sustained* stream
 * can still overrun if the main-loop poll is delayed; see uart.c TODO.) MUST be
 * a power of two. */
#define MIDI_RX_RING_SIZE  256

/* Per-port raw RX byte FIFO between the high-priority Timer0 capture ISR
 * (uart_rx_isr) and the main-loop parser. The FIFO-less ST16C454 RHR must be
 * read within ~320 us (one byte time @ 31250 baud) or it overruns; the ISR
 * drains it every ~100 us into this FIFO so a sustained stream (long SysEx)
 * cannot drop a byte even when the main loop or a USB ISR is busy. MUST be a
 * power of two (index masking); one slot is reserved to tell full from empty.
 * 16 B/port is ample with high-priority capture (the FIFO stays nearly empty)
 * -- bump it if the vendor RX-overflow counter ever trips. 8 ports * 16 = 128 B
 * XDATA. */
#define MIDEX_RX_FIFO_SIZE  16

#endif /* MIDI_CONFIG_H */
