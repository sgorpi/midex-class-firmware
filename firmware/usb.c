/***************************************************************************
 *   Copyright (C) 2011 by Martin Schmoelzer                               *
 *   <martin.schmoelzer@student.tuwien.ac.at>                              *
 *   Copyright (C) 2012 by Johann Glaser <Johann.Glaser@gmx.at>            *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 ***************************************************************************/

/**
 * @file USB SETUP / enumeration engine for the class-compliant MIDEX8 firmware.
 *
 * Vendored from src/ezusb-firmware (OpenULINK fork) and trimmed to the
 * class-compliant spike: the descriptor data now lives in usb_descriptors.c
 * (referenced here as externs), and non-standard (class/vendor) control
 * requests are stalled -- a class-compliant MIDIStreaming device needs no
 * device-specific control requests.
 *
 * Assumptions (to minimise code size): exactly one configuration and one
 * alternate setting, so Set Configuration is a no-op.
 */

#include "usb.h"
#include "common.h"
#include "delay.h"
#include "io.h"
#include "uart.h"   /* uart_rx_overflows + VENDOR_REQ_* codes */

/* Also update external declarations in "usb.h" if making changes to these. */
volatile bool Semaphore_Command = 0;
volatile bool Semaphore_EP2_out = 0;
volatile bool Semaphore_EP2_in  = 0;

volatile __xdata __at 0x7FE8 struct setup_data setup_data;

/* Descriptors live in usb_descriptors.c. The configuration-and-below block is
 * one contiguous __code byte array (config_block) so the SUDPTR auto-length
 * engine can stream it from its wTotalLength field. */
extern __code struct usb_device_descriptor device_descriptor;
extern __code uint8_t config_block[];
extern __code struct usb_language_descriptor language_descriptor;
extern __code struct usb_string_descriptor *__code en_string_descriptors[];

static void usb_handle_setup_data(void);

void sudav_isr(void) __interrupt SUDAV_ISR {
  CLEAR_IRQ();

  usb_handle_setup_data();

  USBIRQ = SUDAVIR;
  EP0CS |= HSNAK;
}

void sof_isr(void)      __interrupt SOF_ISR      { }
void sutok_isr(void)    __interrupt SUTOK_ISR    { }
void suspend_isr(void)  __interrupt SUSPEND_ISR  { }
void usbreset_isr(void) __interrupt USBRESET_ISR { }
void ibn_isr(void)      __interrupt IBN_ISR      { }

void ep0in_isr(void)    __interrupt EP0IN_ISR    { }
void ep0out_isr(void)   __interrupt EP0OUT_ISR   { }
void ep1in_isr(void)    __interrupt EP1IN_ISR    { }
void ep1out_isr(void)   __interrupt EP1OUT_ISR   { }

/**
 * EP2 IN: called after the transfer from uC->Host has finished: we sent data
 */
void ep2in_isr(void)    __interrupt EP2IN_ISR {
  Semaphore_EP2_in = 1;

  CLEAR_IRQ();
  IN07IRQ = IN2IR;     // Clear IN2 IRQ
}

/**
 * EP2 OUT: called after the transfer from Host->uC has finished: we got data
 */
void ep2out_isr(void)   __interrupt EP2OUT_ISR {
  Semaphore_EP2_out = 1;

  CLEAR_IRQ();
  OUT07IRQ = OUT2IR;    // Clear OUT2 IRQ
}

void ep3in_isr(void)    __interrupt EP3IN_ISR    { }
void ep3out_isr(void)   __interrupt EP3OUT_ISR   { }
void ep4in_isr(void)    __interrupt EP4IN_ISR    { }
void ep4out_isr(void)   __interrupt EP4OUT_ISR   { }
void ep5in_isr(void)    __interrupt EP5IN_ISR    { }
void ep5out_isr(void)   __interrupt EP5OUT_ISR   { }
void ep6in_isr(void)    __interrupt EP6IN_ISR    { }
void ep6out_isr(void)   __interrupt EP6OUT_ISR   { }
void ep7in_isr(void)    __interrupt EP7IN_ISR    { }
void ep7out_isr(void)   __interrupt EP7OUT_ISR   { }

/**
 * Return the control/status register for an endpoint
 *
 * @param ep endpoint address
 * @return on success: pointer to Control & Status register for endpoint
 *  specified in \a ep
 * @return on failure: NULL
 */
static __xdata uint8_t* usb_get_endpoint_cs_reg(uint8_t ep) {
  /* Mask direction bit */
  uint8_t ep_num = ep & USB_ENDPOINT_ADDRESS_MASK;

  switch (ep_num) {
  case 0:
    return &EP0CS;
    break;
  case 1:
    return ep & USB_ENDPOINT_DIR_MASK ? &IN1CS : &OUT1CS;
    break;
  case 2:
    return ep & USB_ENDPOINT_DIR_MASK ? &IN2CS : &OUT2CS;
    break;
  case 3:
    return ep & USB_ENDPOINT_DIR_MASK ? &IN3CS : &OUT3CS;
    break;
  case 4:
    return ep & USB_ENDPOINT_DIR_MASK ? &IN4CS : &OUT4CS;
    break;
  case 5:
    return ep & USB_ENDPOINT_DIR_MASK ? &IN5CS : &OUT5CS;
    break;
  case 6:
    return ep & USB_ENDPOINT_DIR_MASK ? &IN6CS : &OUT6CS;
    break;
  case 7:
    return ep & USB_ENDPOINT_DIR_MASK ? &IN7CS : &OUT7CS;
    break;
  }

  return NULL;
}

static void usb_reset_data_toggle(uint8_t ep) {
  /* TOGCTL register:
     +----+-----+-----+------+-----+-------+-------+-------+
     | Q  |  S  |  R  |  IO  |  0  |  EP2  |  EP1  |  EP0  |
     +----+-----+-----+------+-----+-------+-------+-------+

     To reset data toggle bits, we have to write the endpoint direction (IN/OUT)
     to the IO bit and the endpoint number to the EP2..EP0 bits. Then, in a
     separate write cycle, the R bit needs to be set.
  */
  uint8_t togctl_value = (ep & 0x80 >> 3) | (ep & 0x7);

  /* First step: Write EP number and direction bit */
  TOGCTL = togctl_value;

  /* Second step: Set R bit */
  togctl_value |= TOG_R;
  TOGCTL = togctl_value;
}

/**
 * Handle GET_STATUS request.
 *
 * @return on success: true
 * @return on failure: false
 */
static bool usb_handle_get_status(void) {
  uint8_t *ep_cs;

  switch (setup_data.bmRequestType) {
  case USB_RECIP_GS_DEVICE:
    /* Two byte response: Byte 0, Bit 0 = self-powered, Bit 1 = remote wakeup.
     *                    Byte 1: reserved, reset to zero */
    IN0BUF[0] = 0;
    IN0BUF[1] = 0;

    /* Send response */
    IN0BC = 2;
    break;
  case USB_RECIP_GS_INTERFACE:
    /* Always return two zero bytes according to USB 1.1 spec, p. 191 */
    IN0BUF[0] = 0;
    IN0BUF[1] = 0;

    /* Send response */
    IN0BC = 2;
    break;
  case USB_RECIP_GS_ENDPOINT:
    /* Get stall bit for endpoint specified in low byte of wIndex */
    ep_cs = usb_get_endpoint_cs_reg(setup_data.wIndex & 0xff);

    if (*ep_cs & EPSTALL) {
      IN0BUF[0] = 0x01;
    }
    else {
      IN0BUF[0] = 0x00;
    }

    /* Second byte sent has to be always zero */
    IN0BUF[1] = 0;

    /* Send response */
    IN0BC = 2;
    break;
  default:
    return false;
    break;
  }

  return true;
}

/**
 * Handle CLEAR_FEATURE request.
 *
 * @return on success: true
 * @return on failure: false
 */
static bool usb_handle_clear_feature(void) {
  __xdata uint8_t *ep_cs;

  switch (setup_data.bmRequestType) {
  case USB_RECIP_CF_DEVICE:
    /* Clear remote wakeup not supported: stall EP0 */
    STALL_EP0();
    break;
  case USB_RECIP_CF_ENDPOINT:
    if (setup_data.wValue == 0) {
      /* Unstall the endpoint specified in wIndex */
      ep_cs = usb_get_endpoint_cs_reg(setup_data.wIndex);
      if (!ep_cs) {
        return false;
      }
      *ep_cs &= ~EPSTALL;
    }
    else {
      /* Unsupported feature, stall EP0 */
      STALL_EP0();
    }
    break;
  default:
    /* Unsupported recipient */
    break;
  }

  return true;
}

/**
 * Handle SET_FEATURE request.
 *
 * @return on success: true
 * @return on failure: false
 */
static bool usb_handle_set_feature(void) {
  __xdata uint8_t *ep_cs;

  switch (setup_data.bmRequestType) {
  case USB_RECIP_SF_DEVICE:
    if (setup_data.wValue == 2) {
      return true;
    }
    break;
  case USB_RECIP_SF_ENDPOINT:
    if (setup_data.wValue == 0) {
      /* Stall the endpoint specified in wIndex */
      ep_cs = usb_get_endpoint_cs_reg(setup_data.wIndex);
      if (!ep_cs) {
        return false;
      }
      *ep_cs |= EPSTALL;
    }
    else {
      /* Unsupported endpoint feature */
      return false;
    }
    break;
  default:
    /* Unsupported recipient */
    break;
  }

  return true;
}

/**
 * Handle GET_DESCRIPTOR request.
 *
 * @return on success: true
 * @return on failure: false
 */
static bool usb_handle_get_descriptor(void) {
  __xdata uint8_t descriptor_type;
  __xdata uint8_t descriptor_index;

  descriptor_type = (setup_data.wValue & 0xff00) >> 8;
  descriptor_index = setup_data.wValue & 0x00ff;

  switch (descriptor_type) {
  case USB_DESCRIPTOR_TYPE_DEVICE:
    SUDPTRH = HI8(&device_descriptor);
    SUDPTRL = LO8(&device_descriptor);
    break;
  case USB_DESCRIPTOR_TYPE_CONFIGURATION:
    /* SUDPTR streams wTotalLength bytes of the contiguous config_block. */
    SUDPTRH = HI8(config_block);
    SUDPTRL = LO8(config_block);
    break;
  case USB_DESCRIPTOR_TYPE_STRING:
    if (setup_data.wIndex == 0) {
      /* Supply language descriptor */
      SUDPTRH = HI8(&language_descriptor);
      SUDPTRL = LO8(&language_descriptor);
    }
    else if (setup_data.wIndex == USB_LANG_ENGLISH_US) {
      /* Supply string descriptor */
      SUDPTRH = HI8(en_string_descriptors[descriptor_index - 1]);
      SUDPTRL = LO8(en_string_descriptors[descriptor_index - 1]);
    }
    else {
      return false;
    }
    break;
  default:
    /* Unsupported descriptor type */
    return false;
    break;
  }

  return true;
}

/**
 * Handle SET_INTERFACE request: reset the MIDIStreaming bulk endpoints (EP2).
 */
static void usb_handle_set_interface(void) {
  /* Reset Data Toggle */
  usb_reset_data_toggle(USB_DIR_IN  | 2);
  usb_reset_data_toggle(USB_DIR_OUT | 2);

  /* Unstall the IN endpoint, leave it not-busy (ready to be loaded). */
  IN2CS = 0;

  /* Unstall the OUT endpoint and arm it to receive */
  OUT2CS = 0;
  OUT2BC = 0;
}

/**
 * Handle the arrival of a USB Control Setup Packet.
 */
static void usb_handle_setup_data(void) {
  switch (setup_data.bRequest) {
    case USB_REQ_GET_STATUS:
      if (!usb_handle_get_status()) {
        STALL_EP0();
      }
      break;
    case USB_REQ_CLEAR_FEATURE:
      if (!usb_handle_clear_feature()) {
        STALL_EP0();
      }
      break;
    case 2: case 4:
      /* Reserved values */
      STALL_EP0();
      break;
    case USB_REQ_SET_FEATURE:
      if (!usb_handle_set_feature()) {
        STALL_EP0();
      }
      break;
    case USB_REQ_SET_ADDRESS:
      /* Handled by USB core */
      break;
    case USB_REQ_SET_DESCRIPTOR:
      /* Set Descriptor not supported. */
      STALL_EP0();
      break;
    case USB_REQ_GET_DESCRIPTOR:
      if (!usb_handle_get_descriptor()) {
        STALL_EP0();
      }
      break;
    case USB_REQ_GET_CONFIGURATION:
      /* We have only one configuration, return its value (1). */
      IN0BUF[0] = 1;
      IN0BC = 1;
      break;
    case USB_REQ_SET_CONFIGURATION:
      /* we have only one configuration -> nothing to do */
      break;
    case USB_REQ_GET_INTERFACE:
      /* Single alternate setting (0) for every interface. */
      IN0BUF[0] = 0;
      IN0BC = 1;
      break;
    case USB_REQ_SET_INTERFACE:
      usb_handle_set_interface();
      break;
    case USB_REQ_SYNCH_FRAME:
      /* Isochronous endpoints not used -> nothing to do */
      break;
    default:
      /* Vendor requests (bmRequestType bits 6:5 == 10b) expose the RX-overflow
       * diagnostic counter. The OS class driver never issues these, so the
       * device stays class-compliant; only an explicit libusb/pyusb control
       * transfer reaches here. Everything else (class/reserved) stalls. */
      if ((setup_data.bmRequestType & 0x60) == 0x40) {
        if (setup_data.bRequest == VENDOR_REQ_GET_RX_OVERFLOWS &&
            (setup_data.bmRequestType & 0x80)) {            /* device->host IN */
          IN0BUF[0] = uart_rx_overflows;
          IN0BC = 1;
        } else if (setup_data.bRequest == VENDOR_REQ_CLR_RX_OVERFLOWS &&
                   !(setup_data.bmRequestType & 0x80)) {    /* host->device OUT */
          uart_rx_overflows = 0;
          /* zero-length status stage is ACKed by the core HSNAK in sudav_isr */
        } else {
          STALL_EP0();
        }
      } else {
        STALL_EP0();
      }
      break;
  }
}

/**
 * USB initialization. Configures USB interrupts, endpoints and performs
 * ReNumeration.
 */
void usb_init(void) {
  /* Mark endpoint 2 IN & OUT as valid */
  IN07VAL  = IN2VAL;
  OUT07VAL = OUT2VAL;

  /* Make sure no isochronous endpoints are marked valid */
  INISOVAL  = 0;
  OUTISOVAL = 0;

  /* Disable isochronous endpoints. This makes the isochronous data buffers
   * available as 8051 XDATA memory at address 0x2000 - 0x27FF */
  ISOCTL = ISODISAB;

  /* Enable USB Autovectoring */
  USBBAV |= AVEN;

  /* Enable SUDAV interrupt */
  USBIEN |= SUDAVIE;

  /* Enable EP2 OUT & IN interrupts */
  OUT07IEN = OUT2IEN;
  IN07IEN  = IN2IEN;

  /* Enable USB interrupt (EIE register) */
  EUSB = 1;

  /* Perform ReNumeration */
  USBCS = DISCON | RENUM;
  delay_ms(200);
  USBCS = DISCOE | RENUM;
}
