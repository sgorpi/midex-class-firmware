############################################################################
# MIDEX8 bus-probe / class firmware (EZ-USB AN2131, r1).
# Build with SDCC. Vendored scaffolding from src/ezusb-firmware (OpenULINK).
############################################################################

PREFIX =
CC = $(PREFIX)sdcc
AS2 := $(PREFIX)asx8051    # SDCC <= 2.9
AS3 := $(PREFIX)sdas8051   # SDCC >= 3.0
WAS2 := $(strip $(shell which $(AS2)))
WAS3 := $(strip $(shell which $(AS3)))
ifneq "$(WAS3)" ""
  AS = $(AS3)
else ifneq "$(WAS2)" ""
  AS = $(AS2)
else
  $(error Could not find a suitable 8051 assembler (sdas8051/asx8051).)
endif

PACKIHX = $(PREFIX)packihx
SIZE = size

# Board revision select: `make` (r1, default) or `make BOARD=r2`.
#   r1 = EZ-USB AN2131 + 2x ST16C454 (8 external 16550 ports).
#   r2 = EZ-USB FX CY7C646, hybrid: 6 external 16550 + 2 on-chip UARTs.
# The chosen board adds -DBOARD_R<n> so board.h picks board_r<n>.h, and (r2 only)
# links the on-chip-UART backend. See board.h / board_r2.h.
BOARD ?= r1
ifeq ($(BOARD),r2)
  BOARD_CFLAGS  = -DBOARD_R2
  CLASS_IHX     = midex-class-r2.ihx
  ONCHIP_OBJECTS = uart_onchip.rel
else ifeq ($(BOARD),r1)
  BOARD_CFLAGS  =
  CLASS_IHX     = midex-class-r1.ihx
  ONCHIP_OBJECTS =
else
  $(error Unknown BOARD '$(BOARD)' (use r1 or r2))
endif

# The Phase-1 bus-probe diagnostic is a frozen r1-only snapshot (main_probe.c
# pins board_r1.h directly), so its name does not track BOARD.
PROBE_IHX = midex-probe-r1.ihx

# Sources live alongside the headers in this flat directory.
SRC_DIR     = .
INCLUDE_DIR = .

# AN2131 has 8 KB on-chip RAM (0x0000-0x1FFF). The USB autovector jump table
# is placed at 0x1B00 (see USBJmpTb.a51), so code must fit below that.
CODE_SIZE = 0x1B00
# The isochronous endpoint buffers (freed by ISODISAB in usb_init) are usable
# as XDATA at 0x2000-0x27FF.
XRAM_LOC  = 0x2000
XRAM_SIZE = 0x0800

CFLAGS  = --std-sdcc99 --opt-code-size --model-small $(BOARD_CFLAGS)
LDFLAGS = --code-loc 0x0000 --code-size $(CODE_SIZE) --xram-loc $(XRAM_LOC) \
          --xram-size $(XRAM_SIZE) --iram-size 256 --model-small

# uart.rel = dispatcher + shared RX-capture core; uart_ext.rel = external 16550
# backend (both boards); uart_onchip.rel = FX on-chip UART backend (r2 only).
CLASS_OBJECTS = main.rel usb.rel usb_descriptors.rel uart.rel uart_ext.rel \
                $(ONCHIP_OBJECTS) midi_parser.rel delay.rel USBJmpTb.rel
PROBE_OBJECTS = main_probe.rel usb_probe.rel delay.rel USBJmpTb.rel
HEADERS = usb.h common.h delay.h reg_ezusb.h io.h board.h board_r1.h board_r2.h \
          uart.h uart_ext.h uart_onchip.h midi_config.h midi_parser.h

.SUFFIXES:
.PHONY: all class probe clean hex both

all: class probe

# Build BOTH revision images (midex-class-r1.ihx + midex-class-r2.ihx) in one go,
# for the host installer (midex-fwctl.sh ships whichever exist). The objects are
# board-specific (-DBOARD_R2), so each board needs a fresh compile; we drop the
# intermediates between boards but keep both .ihx outputs.
both:
	$(MAKE) clean
	$(MAKE) BOARD=r1 class
	rm -f *.rel *.asm *.lst *.rst *.sym *.lk *.map *.mem *.cdb
	$(MAKE) BOARD=r2 class

class: $(CLASS_IHX)
	$(SIZE) $(CLASS_IHX) || true

probe: $(PROBE_IHX)
	$(SIZE) $(PROBE_IHX) || true

$(CLASS_IHX): $(CLASS_OBJECTS)
	$(CC) -mmcs51 $(LDFLAGS) -o $@ $^

$(PROBE_IHX): $(PROBE_OBJECTS)
	$(CC) -mmcs51 $(LDFLAGS) -o $@ $^

%.rel: $(SRC_DIR)/%.c $(HEADERS)
	$(CC) -c $(CFLAGS) -mmcs51 -I$(INCLUDE_DIR) -o $@ $<

%.rel: $(SRC_DIR)/%.a51
	$(AS) -lsgo $@ $<

hex: $(CLASS_IHX)
	$(PACKIHX) $(CLASS_IHX) > $(basename $(CLASS_IHX)).hex

clean:
	rm -f *.asm *.lst *.rel *.rst *.sym *.ihx *.hex *.lnk *.map *.mem *.cdb *.lk
