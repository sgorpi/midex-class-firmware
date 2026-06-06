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

#define NUM_MIDI_PORTS   2

/* USB-MIDI bulk endpoints (the OpenULINK skeleton already configures EP2). The
 * host->device (TX) stream arrives on EP2-OUT; device->host (RX) goes on EP2-IN.
 * See the endpoint decision in the plan (EP2-IN + EP2-OUT pair). */
#define MIDI_EP_OUT_ADDR  0x02   /* bulk OUT: host -> device (-> UART THR)   */
#define MIDI_EP_IN_ADDR   0x82   /* bulk IN : device -> host (<- UART RHR)   */
#define MIDI_EP_MAXPKT    64

#endif /* MIDI_CONFIG_H */
