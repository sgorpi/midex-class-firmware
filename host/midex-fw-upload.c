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
 * midex-fw-upload - cross-platform RAM firmware uploader for the Steinberg
 * MIDEX family (Cypress EZ-USB AN2131 / CY7C646 FX).
 *
 * Implements the verified single-stage "Anchor Download" sequence:
 *     CPUCS = 1   (hold 8051 in reset)
 *     0xA0 writes (stream the firmware image into on-chip RAM)
 *     CPUCS = 0   (release 8051 -> firmware boots, device re-enumerates)
 *
 * The device must be in a loader PID (0x1000 / 0x1010 / 0x1100) so the
 * EZ-USB ROM handles the 0xA0 request. A bare power-cycle of an operational
 * device reverts it to the loader PID (RAM is volatile -> unbrickable).
 *
 * Input is an Intel HEX (.ihx) image as produced by SDCC.
 *
 * See doc/firmware_upload_process.md and
 * src/midex-class-firmware/doc/hardware_register_map.md.
 *
 * Build: make            (needs libusb-1.0)
 * Usage: sudo ./midex-fw-upload firmware.ihx
 *        sudo ./midex-fw-upload -d 0a4e:1000 firmware.ihx
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libusb.h>

#define MIDEX_VID 0x0a4e
#define CPUCS_ADDR 0x7F92
#define FW_REQ 0xA0
#define FW_REQTYPE (LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | \
		    LIBUSB_RECIPIENT_DEVICE) /* 0x40 */
#define MAX_CHUNK 64 /* 0xA0 wLength ceiling */
#define CTRL_TIMEOUT_MS 5000

/* Loader PIDs that accept the ROM 0xA0 download. */
static const uint16_t loader_pids[] = { 0x1000, 0x1010, 0x1100 };

/* ---- Intel HEX parsing ------------------------------------------------- */

struct fw_record {
	uint16_t addr;
	uint8_t len;
	uint8_t data[256];
};

static int hexbyte(const char *s, uint8_t *out)
{
	char buf[3] = { s[0], s[1], 0 };
	char *end;
	long v = strtol(buf, &end, 16);

	if (end != buf + 2)
		return -1;
	*out = (uint8_t)v;
	return 0;
}

/*
 * Parse an Intel HEX file into an ordered array of records. Handles record
 * types 00 (data), 01 (EOF) and 04/02 (extended address). The MIDEX images
 * live below 0x2000 so the upper-16-bit base is normally zero, but we honour
 * it defensively. Returns the record count, or -1 on error. Caller frees
 * *records.
 */
static int parse_ihex(const char *path, struct fw_record **records, int *count)
{
	FILE *f = fopen(path, "r");
	char line[600];
	struct fw_record *recs = NULL;
	int n = 0, cap = 0;
	uint32_t base = 0;

	if (!f) {
		fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno));
		return -1;
	}

	while (fgets(line, sizeof(line), f)) {
		uint8_t len, type, sum = 0, b;
		uint16_t addr;
		size_t i;

		if (line[0] != ':')
			continue; /* skip blank / stray lines */
		if (hexbyte(line + 1, &len) || hexbyte(line + 3, &b))
			goto bad;
		addr = (uint16_t)b << 8;
		if (hexbyte(line + 5, &b))
			goto bad;
		addr |= b;
		if (hexbyte(line + 7, &type))
			goto bad;
		sum = len + (addr >> 8) + (addr & 0xff) + type;

		if (type == 0x01)
			break; /* EOF */
		if (type == 0x02 || type == 0x04) {
			/* extended (segment/linear) base address */
			uint8_t hi, lo;

			if (hexbyte(line + 9, &hi) || hexbyte(line + 11, &lo))
				goto bad;
			sum += hi + lo;
			base = (type == 0x04) ? ((uint32_t)((hi << 8) | lo) << 16)
					      : ((uint32_t)((hi << 8) | lo) << 4);
			continue;
		}
		if (type != 0x00)
			continue; /* ignore start-address records etc. */

		if (n == cap) {
			cap = cap ? cap * 2 : 256;
			recs = realloc(recs, cap * sizeof(*recs));
			if (!recs)
				goto bad;
		}
		recs[n].addr = (uint16_t)((base + addr) & 0xffff);
		recs[n].len = len;
		for (i = 0; i < len; i++) {
			if (hexbyte(line + 9 + i * 2, &b))
				goto bad;
			recs[n].data[i] = b;
			sum += b;
		}
		/* checksum: two's complement of the running sum */
		if (hexbyte(line + 9 + len * 2, &b))
			goto bad;
		if ((uint8_t)(sum + b) != 0) {
			fprintf(stderr, "checksum error on record %d\n", n);
			goto bad;
		}
		n++;
	}

	fclose(f);
	*records = recs;
	*count = n;
	return 0;
bad:
	fprintf(stderr, "malformed Intel HEX in %s\n", path);
	fclose(f);
	free(recs);
	return -1;
}

/* ---- USB upload -------------------------------------------------------- */

static int anchor_write(libusb_device_handle *h, uint16_t addr,
			const uint8_t *data, uint16_t len)
{
	int r = libusb_control_transfer(h, FW_REQTYPE, FW_REQ, addr, 0,
					(unsigned char *)data, len,
					CTRL_TIMEOUT_MS);
	return (r == len) ? 0 : r;
}

static int cpucs(libusb_device_handle *h, uint8_t reset)
{
	return anchor_write(h, CPUCS_ADDR, &reset, 1);
}

static libusb_device_handle *open_midex(libusb_context *ctx, uint16_t want_vid,
					int want_pid)
{
	libusb_device **list;
	libusb_device_handle *h = NULL;
	ssize_t n = libusb_get_device_list(ctx, &list);
	ssize_t i;

	for (i = 0; i < n; i++) {
		struct libusb_device_descriptor d;
		size_t k;
		int match = 0;

		if (libusb_get_device_descriptor(list[i], &d))
			continue;
		if (d.idVendor != want_vid)
			continue;
		if (want_pid >= 0) {
			match = (d.idProduct == want_pid);
		} else {
			for (k = 0; k < sizeof(loader_pids) / 2; k++)
				if (d.idProduct == loader_pids[k])
					match = 1;
		}
		if (!match)
			continue;
		printf("Found MIDEX %04x:%04x\n", d.idVendor, d.idProduct);
		if (libusb_open(list[i], &h)) {
			fprintf(stderr, "libusb_open failed (need root?)\n");
			h = NULL;
		}
		break;
	}
	libusb_free_device_list(list, 1);
	return h;
}

int main(int argc, char **argv)
{
	const char *path = NULL;
	uint16_t vid = MIDEX_VID;
	int pid = -1; /* auto: any loader PID */
	struct fw_record *recs = NULL;
	libusb_context *ctx = NULL;
	libusb_device_handle *h;
	int count = 0, i, r, rc = 1;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-d") && i + 1 < argc) {
			unsigned int v, p;

			if (sscanf(argv[++i], "%x:%x", &v, &p) != 2) {
				fprintf(stderr, "bad -d VID:PID\n");
				return 1;
			}
			vid = (uint16_t)v;
			pid = (int)p;
		} else if (argv[i][0] != '-') {
			path = argv[i];
		} else {
			fprintf(stderr,
				"usage: %s [-d VID:PID] firmware.ihx\n", argv[0]);
			return 1;
		}
	}
	if (!path) {
		fprintf(stderr, "usage: %s [-d VID:PID] firmware.ihx\n", argv[0]);
		return 1;
	}

	if (parse_ihex(path, &recs, &count))
		return 1;
	printf("Parsed %d HEX records from %s\n", count, path);

	if (libusb_init(&ctx)) {
		fprintf(stderr, "libusb_init failed\n");
		goto out_recs;
	}

	h = open_midex(ctx, vid, pid);
	if (!h) {
		fprintf(stderr,
			"No MIDEX loader device found. Power-cycle the device so "
			"it reverts to a loader PID (0x1000/0x1010/0x1100), and "
			"ensure snd-usb-midex is not auto-uploading stock firmware.\n");
		goto out_ctx;
	}

	libusb_set_auto_detach_kernel_driver(h, 1);
	if (libusb_claim_interface(h, 0))
		/* The ROM loader has no real interface driver; claim is best
		 * effort and not fatal if it fails. */
		fprintf(stderr, "warning: could not claim interface 0\n");

	printf("Holding 8051 in reset (CPUCS=1)...\n");
	r = cpucs(h, 1);
	if (r) {
		fprintf(stderr, "CPUCS=1 failed: %s\n", libusb_strerror(r));
		goto out_close;
	}

	printf("Uploading %d records...\n", count);
	for (i = 0; i < count; i++) {
		uint16_t off = 0;

		while (off < recs[i].len) {
			uint16_t chunk = recs[i].len - off;

			if (chunk > MAX_CHUNK)
				chunk = MAX_CHUNK;
			r = anchor_write(h, recs[i].addr + off,
					 recs[i].data + off, chunk);
			if (r) {
				fprintf(stderr,
					"write @0x%04x failed: %s\n",
					recs[i].addr + off, libusb_strerror(r));
				goto out_close;
			}
			off += chunk;
		}
	}

	printf("Releasing 8051 (CPUCS=0)...\n");
	r = cpucs(h, 0);
	/* The device renumerates as the 8051 starts, racing the round-trip;
	 * NO_DEVICE here means success. */
	if (r && r != LIBUSB_ERROR_NO_DEVICE) {
		fprintf(stderr, "CPUCS=0 failed: %s\n", libusb_strerror(r));
		goto out_close;
	}
	printf("Done. Firmware booted; device should re-enumerate shortly.\n");
	rc = 0;

out_close:
	libusb_close(h);
out_ctx:
	libusb_exit(ctx);
out_recs:
	free(recs);
	return rc;
}
