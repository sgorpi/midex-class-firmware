/*
 * usb_descriptors.c - USB Audio Class 1.0 / MIDIStreaming descriptors for the
 * class-compliant MIDEX8 firmware (Phase 2 spike, NUM_MIDI_PORTS cables).
 *
 * The device exposes one AudioControl interface (no units) plus one
 * MIDIStreaming interface carrying NUM_MIDI_PORTS bidirectional cables over a
 * single bulk IN + bulk OUT endpoint pair (EP2). Per the USB-MIDI 1.0 topology,
 * each port p (0-based) gets four jacks:
 *
 *   id 4p+1  Embedded MIDI IN  jack  <- fed by the bulk-OUT endpoint
 *   id 4p+2  External MIDI IN  jack  <- physical DIN IN socket
 *   id 4p+3  Embedded MIDI OUT jack  -> feeds the bulk-IN endpoint   (src 4p+2)
 *   id 4p+4  External MIDI OUT jack  -> physical DIN OUT socket      (src 4p+1)
 *
 * Data flow (host plays out a DIN):  bulk-OUT -> EmbeddedIN(4p+1) -> ExternalOUT(4p+4) -> DIN OUT
 * Data flow (DIN into the host)   :  DIN IN -> ExternalIN(4p+2) -> EmbeddedOUT(4p+3) -> bulk-IN
 *
 * Cable number n in a USB-MIDI event packet maps to the n-th jack listed in the
 * class-specific endpoint descriptor, i.e. cable n <-> UART port n (linear).
 *
 * The whole config-and-below block is one contiguous __code byte array so the
 * EZ-USB SUDPTR auto-length engine can stream it: for a CONFIGURATION request it
 * reads wTotalLength from the descriptor and sends that many bytes. (This is why
 * the array must stay contiguous and wTotalLength must be exact.)
 */
#include "usb.h"
#include "midi_config.h"

/* The 2-port descriptor below is hand-unrolled. Phase 3 (8 ports) extends the
 * jack list + endpoint assoc arrays and bumps the length constants. */
#if NUM_MIDI_PORTS != 2
#error "usb_descriptors.c is hand-packed for 2 ports; extend it for Phase 3."
#endif

#define ID_VENDOR   0x0A4E   /* Steinberg                                     */
#define ID_PRODUCT  0x10C1   /* fresh class-compliant PID (snd-usb-audio binds)*/
#define BCD_DEVICE  0x0100   /* firmware version 1.00                          */

/* --- USB Audio Class / MIDIStreaming descriptor constants ----------------- */
#define DSC_CS_INTERFACE   0x24
#define DSC_CS_ENDPOINT    0x25
#define MS_SUB_HEADER      0x01   /* CS interface: MS_HEADER / EP: MS_GENERAL  */
#define MS_SUB_MIDI_IN     0x02
#define MS_SUB_MIDI_OUT    0x03
#define JACK_EMBEDDED      0x01
#define JACK_EXTERNAL      0x02
#define SUBCLASS_AUDIOCONTROL  0x01
#define SUBCLASS_MIDISTREAMING 0x03

/* MIDI IN  jack descriptor (6 bytes). */
#define DESC_MIDI_IN_JACK(jtype, id) \
	6, DSC_CS_INTERFACE, MS_SUB_MIDI_IN, (jtype), (id), 0
/* MIDI OUT jack descriptor with a single input pin (9 bytes). */
#define DESC_MIDI_OUT_JACK(jtype, id, srcid) \
	9, DSC_CS_INTERFACE, MS_SUB_MIDI_OUT, (jtype), (id), 1, (srcid), 1, 0

/* Length constants (recomputed if NUM_MIDI_PORTS changes). 2 ports:
 *   config 9 + std-AC-if 9 + cs-AC-hdr 9 + std-MS-if 9 + cs-MS-hdr 7
 *   + jacks 2*(6+6+9+9)=60 + std-OUT-EP 9 + cs-OUT-EP 6 + std-IN-EP 9 + cs-IN-EP 6 */
#define CONFIG_TOTAL_LEN   133
/* CS MS-interface-header wTotalLength = header(7) + all jack descriptors(60). */
#define MS_TOTAL_LEN       67

__code struct usb_device_descriptor device_descriptor = {
	/* .bLength            = */ sizeof(struct usb_device_descriptor),
	/* .bDescriptorType    = */ USB_DESCRIPTOR_TYPE_DEVICE,
	/* .bcdUSB             = */ 0x0110,            /* USB 1.1                  */
	/* .bDeviceClass       = */ 0x00,             /* class defined per-iface  */
	/* .bDeviceSubClass    = */ 0x00,
	/* .bDeviceProtocol    = */ 0x00,
	/* .bMaxPacketSize0    = */ 64,
	/* .idVendor           = */ ID_VENDOR,
	/* .idProduct          = */ ID_PRODUCT,
	/* .bcdDevice          = */ BCD_DEVICE,
	/* .iManufacturer      = */ 1,
	/* .iProduct           = */ 2,
	/* .iSerialNumber      = */ 3,
	/* .bNumConfigurations = */ 1
};

/* Entire configuration block, hand-packed and contiguous (see file header). */
__code uint8_t config_block[CONFIG_TOTAL_LEN] = {
	/* --- Configuration descriptor (9) --- */
	9, USB_DESCRIPTOR_TYPE_CONFIGURATION,
	(CONFIG_TOTAL_LEN & 0xff), (CONFIG_TOTAL_LEN >> 8), /* wTotalLength       */
	2,        /* bNumInterfaces: AudioControl + MIDIStreaming                  */
	1,        /* bConfigurationValue                                           */
	0,        /* iConfiguration                                                */
	0x80,     /* bmAttributes: bus-powered, reserved bit set                   */
	50,       /* bMaxPower: 100 mA                                             */

	/* --- Standard AudioControl interface (9): interface 0, no endpoints --- */
	9, USB_DESCRIPTOR_TYPE_INTERFACE,
	0,        /* bInterfaceNumber                                              */
	0,        /* bAlternateSetting                                             */
	0,        /* bNumEndpoints                                                 */
	USB_CLASS_AUDIO, SUBCLASS_AUDIOCONTROL, 0x00,
	0,        /* iInterface                                                    */

	/* --- Class-specific AC interface header (9) --- */
	9, DSC_CS_INTERFACE,
	MS_SUB_HEADER,        /* HEADER                                            */
	0x00, 0x01,           /* bcdADC 1.00                                       */
	0x09, 0x00,           /* wTotalLength of AC descriptors (just this header) */
	1,                    /* bInCollection: 1 streaming interface              */
	1,                    /* baInterfaceNr(1): the MIDIStreaming interface     */

	/* --- Standard MIDIStreaming interface (9): interface 1, 2 endpoints --- */
	9, USB_DESCRIPTOR_TYPE_INTERFACE,
	1,        /* bInterfaceNumber                                              */
	0,        /* bAlternateSetting                                             */
	2,        /* bNumEndpoints: bulk OUT + bulk IN                             */
	USB_CLASS_AUDIO, SUBCLASS_MIDISTREAMING, 0x00,
	0,        /* iInterface                                                    */

	/* --- Class-specific MS interface header (7) --- */
	7, DSC_CS_INTERFACE,
	MS_SUB_HEADER,
	0x00, 0x01,                            /* bcdMSC 1.00                      */
	(MS_TOTAL_LEN & 0xff), (MS_TOTAL_LEN >> 8), /* wTotalLength (hdr + jacks)  */

	/* --- Port 0 jacks (ids 1..4) --- */
	DESC_MIDI_IN_JACK(JACK_EMBEDDED, 1),
	DESC_MIDI_IN_JACK(JACK_EXTERNAL, 2),
	DESC_MIDI_OUT_JACK(JACK_EMBEDDED, 3, 2),   /* src = External IN (2)        */
	DESC_MIDI_OUT_JACK(JACK_EXTERNAL, 4, 1),   /* src = Embedded IN (1)        */

	/* --- Port 1 jacks (ids 5..8) --- */
	DESC_MIDI_IN_JACK(JACK_EMBEDDED, 5),
	DESC_MIDI_IN_JACK(JACK_EXTERNAL, 6),
	DESC_MIDI_OUT_JACK(JACK_EMBEDDED, 7, 6),
	DESC_MIDI_OUT_JACK(JACK_EXTERNAL, 8, 5),

	/* --- Standard bulk OUT endpoint (9, audio-class layout) --- */
	9, USB_DESCRIPTOR_TYPE_ENDPOINT,
	MIDI_EP_OUT_ADDR,     /* 0x02                                              */
	USB_ENDPOINT_TYPE_BULK,
	(MIDI_EP_MAXPKT & 0xff), (MIDI_EP_MAXPKT >> 8),
	0,                    /* bInterval                                         */
	0,                    /* bRefresh (audio EP descriptor)                    */
	0,                    /* bSynchAddress                                     */

	/* --- Class-specific bulk OUT endpoint (4 + 2): embedded MIDI IN jacks --- */
	6, DSC_CS_ENDPOINT,
	MS_SUB_HEADER,        /* MS_GENERAL                                        */
	2,                    /* bNumEmbMIDIJack                                   */
	1, 5,                 /* baAssocJackID: cable 0 -> jack 1, cable 1 -> jack 5 */

	/* --- Standard bulk IN endpoint (9) --- */
	9, USB_DESCRIPTOR_TYPE_ENDPOINT,
	MIDI_EP_IN_ADDR,      /* 0x82                                              */
	USB_ENDPOINT_TYPE_BULK,
	(MIDI_EP_MAXPKT & 0xff), (MIDI_EP_MAXPKT >> 8),
	0, 0, 0,

	/* --- Class-specific bulk IN endpoint (6): embedded MIDI OUT jacks --- */
	6, DSC_CS_ENDPOINT,
	MS_SUB_HEADER,
	2,                    /* bNumEmbMIDIJack                                   */
	3, 7                  /* baAssocJackID: cable 0 -> jack 3, cable 1 -> jack 7 */
};

/* Build-time guard: the array length must equal the wTotalLength we advertise. */
typedef char config_block_len_check[(sizeof(config_block) == CONFIG_TOTAL_LEN) ? 1 : -1];

__code struct usb_language_descriptor language_descriptor = {
	/* .bLength = */         4,
	/* .bDescriptorType = */ USB_DESCRIPTOR_TYPE_STRING,
	/* .wLANGID = */         {USB_LANG_ENGLISH_US}
};

__code struct usb_string_descriptor strManufacturer =
	STR_DESCR(9, 'S','t','e','i','n','b','e','r','g');
__code struct usb_string_descriptor strProduct =
	STR_DESCR(10, 'M','I','D','E','X','8',' ','U','A','C');
__code struct usb_string_descriptor strSerialNumber =
	STR_DESCR(6, '0','0','0','0','0','1');

/* Indexed by (string index - 1); matches iManufacturer/iProduct/iSerialNumber. */
__code struct usb_string_descriptor *__code en_string_descriptors[3] = {
	&strManufacturer,
	&strProduct,
	&strSerialNumber
};
