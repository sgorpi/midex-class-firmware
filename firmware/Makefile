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

# Two firmwares share the vendored enumeration scaffolding:
#   - the class-compliant MIDI spike (the active deliverable, default target)
#   - the Phase-1 bus-probe diagnostic (frozen *_probe.c snapshots, retained)
SPIKE_IHX = midex-spike-r1.ihx
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

CFLAGS  = --std-sdcc99 --opt-code-size --model-small
LDFLAGS = --code-loc 0x0000 --code-size $(CODE_SIZE) --xram-loc $(XRAM_LOC) \
          --xram-size $(XRAM_SIZE) --iram-size 256 --model-small

SPIKE_OBJECTS = main.rel usb.rel usb_descriptors.rel uart.rel midi_parser.rel delay.rel USBJmpTb.rel
PROBE_OBJECTS = main_probe.rel usb_probe.rel delay.rel USBJmpTb.rel
HEADERS = usb.h common.h delay.h reg_ezusb.h io.h board_r1.h uart.h midi_config.h midi_parser.h

.SUFFIXES:
.PHONY: all spike probe clean hex

all: spike probe

spike: $(SPIKE_IHX)
	$(SIZE) $(SPIKE_IHX) || true

probe: $(PROBE_IHX)
	$(SIZE) $(PROBE_IHX) || true

$(SPIKE_IHX): $(SPIKE_OBJECTS)
	$(CC) -mmcs51 $(LDFLAGS) -o $@ $^

$(PROBE_IHX): $(PROBE_OBJECTS)
	$(CC) -mmcs51 $(LDFLAGS) -o $@ $^

%.rel: $(SRC_DIR)/%.c $(HEADERS)
	$(CC) -c $(CFLAGS) -mmcs51 -I$(INCLUDE_DIR) -o $@ $<

%.rel: $(SRC_DIR)/%.a51
	$(AS) -lsgo $@ $<

hex: $(SPIKE_IHX)
	$(PACKIHX) $(SPIKE_IHX) > $(basename $(SPIKE_IHX)).hex

clean:
	rm -f *.asm *.lst *.rel *.rst *.sym *.ihx *.hex *.lnk *.map *.mem *.cdb *.lk
