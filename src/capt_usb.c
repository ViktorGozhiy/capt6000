/*
 * capt_usb.c -- USB transport and CAPT packet layer.
 * Part of capt6000, a CUPS driver for the Canon LBP6000/LBP6018.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Copyright (C) 2026  Viktor Hozhyi <https://github.com/ViktorGozhiy/capt6000>
 *
 * The packet framing, the BCD reply-length quirk and the layout of the
 * status record come from the captdriver project:
 *   Copyright (C) 2013  Alexey Galakhov <agalakhov@gmail.com>
 *
 * This program is free software under the terms of the GNU General
 * Public License, version 3 or any later version; see the file
 * LICENSE.  There is NO WARRANTY, to the extent permitted by law.
 *
 * The backend owns the device outright: CAPT needs a reply read back
 * before the next command goes out, and interposing CUPS' back and
 * side channels between the two halves of that exchange is exactly
 * what makes the existing CAPT drivers fragile.
 */

#include "capt.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libusb-1.0/libusb.h>

#define CANON_VID		0x04A9

#define USB_CLASS_PRINTER	7
#define USB_SUBCLASS_PRINTER	1
#define USB_PROTO_BIDIR		2

#define IO_TIMEOUT_MS		20000
#define REPLY_BUF		4096

struct capt_dev {
	libusb_context *ctx;
	libusb_device_handle *handle;
	int interface;
	int claimed;
	unsigned char ep_out;
	unsigned char ep_in;
	char device_id[1024];

	/* Offline mode: write the wire stream to a file and answer
	 * commands ourselves.  Used to exercise the whole CUPS chain
	 * and to capture exactly what would go to the printer. */
	FILE *sink;
	unsigned sink_pages;
};

static int trace;
static int status_trace;

void capt_set_trace(int on)
{
	trace = on;
}

void capt_set_status_trace(int on)
{
	status_trace = on;
}

static void trace_buf(const char *dir, const uint8_t *buf, size_t size)
{
	size_t i, shown = size > 64 ? 64 : size;

	if (!trace)
		return;
	fprintf(stderr, "DEBUG: capt6000: %s %zu bytes:", dir, size);
	for (i = 0; i < shown; ++i) {
		if (i % 16 == 0)
			fprintf(stderr, "\nDEBUG: capt6000:  ");
		fprintf(stderr, " %02X", buf[i]);
	}
	if (shown < size)
		fprintf(stderr, " ... (%zu more)", size - shown);
	fprintf(stderr, "\n");
}

/* ------------------------------------------------------------------ */

static int printer_interface(libusb_device *dev,
			     struct libusb_config_descriptor *cfg,
			     int *iface, unsigned char *ep_out,
			     unsigned char *ep_in)
{
	int i, a, e;

	(void) dev;
	for (i = 0; i < cfg->bNumInterfaces; ++i) {
		const struct libusb_interface *itf = &cfg->interface[i];

		for (a = 0; a < itf->num_altsetting; ++a) {
			const struct libusb_interface_descriptor *d =
				&itf->altsetting[a];
			unsigned char out = 0, in = 0;

			if (d->bInterfaceClass != USB_CLASS_PRINTER ||
			    d->bInterfaceSubClass != USB_SUBCLASS_PRINTER ||
			    d->bInterfaceProtocol != USB_PROTO_BIDIR)
				continue;

			for (e = 0; e < d->bNumEndpoints; ++e) {
				const struct libusb_endpoint_descriptor *ep =
					&d->endpoint[e];

				if ((ep->bmAttributes & 3) !=
				    LIBUSB_TRANSFER_TYPE_BULK)
					continue;
				if (ep->bEndpointAddress & 0x80)
					in = ep->bEndpointAddress;
				else
					out = ep->bEndpointAddress;
			}
			if (out && in) {
				*iface = d->bInterfaceNumber;
				*ep_out = out;
				*ep_in = in;
				return 0;
			}
		}
	}
	return -1;
}

/* IEEE 1284 device ID, class specific GET_DEVICE_ID request. */
static int fetch_device_id(libusb_device_handle *h, int iface, char *out,
			   size_t outlen)
{
	unsigned char buf[1024];
	int n, len;

	n = libusb_control_transfer(h,
		LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_CLASS |
			LIBUSB_RECIPIENT_INTERFACE,
		0 /* GET_DEVICE_ID */, 0, (uint16_t) (iface << 8),
		buf, sizeof(buf) - 1, 5000);
	if (n < 2)
		return -1;

	len = (buf[0] << 8) | buf[1];		/* big endian, includes itself */
	if (len < 2 || len > n)
		len = n;
	len -= 2;
	if ((size_t) len >= outlen)
		len = (int) outlen - 1;
	memcpy(out, buf + 2, (size_t) len);
	out[len] = '\0';
	return 0;
}

static int device_id_field(const char *id, const char *key, char *out,
			   size_t outlen)
{
	size_t keylen = strlen(key);
	const char *p = id;

	while (p && *p) {
		const char *colon = strchr(p, ':');
		const char *semi = strchr(p, ';');
		const char *end = semi ? semi : p + strlen(p);

		while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
			++p;
		if (colon && colon < end &&
		    (size_t) (colon - p) == keylen &&
		    strncasecmp(p, key, keylen) == 0) {
			size_t n = (size_t) (end - colon - 1);
			if (n >= outlen)
				n = outlen - 1;
			memcpy(out, colon + 1, n);
			out[n] = '\0';
			return 0;
		}
		if (!semi)
			break;
		p = semi + 1;
	}
	return -1;
}

int capt_device_model(const struct capt_dev *dev, char *out, size_t outlen)
{
	if (device_id_field(dev->device_id, "MDL", out, outlen) == 0)
		return 0;
	return device_id_field(dev->device_id, "MODEL", out, outlen);
}

const char *capt_device_id(const struct capt_dev *dev)
{
	return dev->device_id;
}

int capt_usb_enumerate(capt_enum_cb cb, void *user)
{
	libusb_context *ctx = NULL;
	libusb_device **list = NULL;
	ssize_t count, i;
	int found = 0;

	if (libusb_init(&ctx) != 0)
		return -1;

	count = libusb_get_device_list(ctx, &list);
	for (i = 0; i < count; ++i) {
		struct libusb_device_descriptor dd;
		struct libusb_config_descriptor *cfg = NULL;
		libusb_device_handle *h = NULL;
		char id[1024] = "";
		char make[128] = "Canon";
		char model[128] = "";
		char serial[128] = "";
		int iface;
		unsigned char epo, epi;

		if (libusb_get_device_descriptor(list[i], &dd) != 0)
			continue;
		if (dd.idVendor != CANON_VID)
			continue;
		if (libusb_get_active_config_descriptor(list[i], &cfg) != 0)
			continue;
		if (printer_interface(list[i], cfg, &iface, &epo, &epi) != 0) {
			libusb_free_config_descriptor(cfg);
			continue;
		}
		libusb_free_config_descriptor(cfg);

		if (libusb_open(list[i], &h) != 0)
			continue;
		if (fetch_device_id(h, iface, id, sizeof(id)) == 0) {
			device_id_field(id, "MFG", make, sizeof(make));
			if (device_id_field(id, "MDL", model,
					    sizeof(model)) != 0)
				device_id_field(id, "MODEL", model,
						sizeof(model));
			if (device_id_field(id, "SERN", serial,
					    sizeof(serial)) != 0)
				device_id_field(id, "SN", serial,
						sizeof(serial));
		}
		if (!serial[0] && dd.iSerialNumber) {
			unsigned char s[128];
			if (libusb_get_string_descriptor_ascii(h,
					dd.iSerialNumber, s, sizeof(s)) > 0)
				snprintf(serial, sizeof(serial), "%s", s);
		}
		libusb_close(h);

		/* Only CAPT devices; PCL and UFR printers are none of
		 * our business even though they are Canon too. */
		if (!strstr(id, "CAPT") && !strstr(id, "capt"))
			continue;
		if (!model[0])
			continue;

		cb(make, model, serial, id, user);
		++found;
	}

	if (list)
		libusb_free_device_list(list, 1);
	libusb_exit(ctx);
	return found;
}

/*
 * Bring the endpoints back to a known state.  A previous driver that
 * died mid-transfer - or, memorably, a CUPS queue that spent a while
 * pushing PostScript at a printer that speaks CAPT - leaves the bulk
 * pipes halted and unread replies sitting in the device.  Clear the
 * stalls and swallow whatever is still queued so the first real
 * command does not get an answer meant for someone else.
 */
static void resync_endpoints(struct capt_dev *dev)
{
	uint8_t scratch[1024];
	int i;

	libusb_clear_halt(dev->handle, dev->ep_out);
	libusb_clear_halt(dev->handle, dev->ep_in);

	for (i = 0; i < 64; ++i) {
		int done = 0;
		int rc = libusb_bulk_transfer(dev->handle, dev->ep_in, scratch,
					      (int) sizeof(scratch), &done, 150);
		if (rc != 0 || done == 0)
			break;
		if (trace)
			fprintf(stderr, "DEBUG: capt6000: discarded %d stale "
					"bytes\n", done);
	}
}

struct capt_dev *capt_usb_open(const char *serial, char *err, size_t errlen)
{
	struct capt_dev *dev;
	libusb_device **list = NULL;
	ssize_t count, i;
	int rc;

	const char *sink_path = getenv("CAPT_SINK");

	dev = calloc(1, sizeof(*dev));
	if (!dev) {
		snprintf(err, errlen, "out of memory");
		return NULL;
	}

	if (sink_path && *sink_path) {
		dev->sink = fopen(sink_path, "wb");
		if (!dev->sink) {
			snprintf(err, errlen, "cannot write %s", sink_path);
			free(dev);
			return NULL;
		}
		snprintf(dev->device_id, sizeof(dev->device_id),
			 "MFG:Canon;MDL:LBP6000/LBP6018;CMD:CAPT;VER:3.0;"
			 "CLS:PRINTER;DES:Canon LBP6000/LBP6018");
		fprintf(stderr, "DEBUG: capt6000: offline mode, writing the "
				"CAPT stream to %s\n", sink_path);
		return dev;
	}

	if (libusb_init(&dev->ctx) != 0) {
		snprintf(err, errlen, "cannot initialise libusb");
		free(dev);
		return NULL;
	}

	snprintf(err, errlen, "no Canon CAPT printer found");
	count = libusb_get_device_list(dev->ctx, &list);
	for (i = 0; i < count; ++i) {
		struct libusb_device_descriptor dd;
		struct libusb_config_descriptor *cfg = NULL;
		char sern[128] = "";

		if (libusb_get_device_descriptor(list[i], &dd) != 0)
			continue;
		if (dd.idVendor != CANON_VID)
			continue;
		if (libusb_get_active_config_descriptor(list[i], &cfg) != 0)
			continue;
		rc = printer_interface(list[i], cfg, &dev->interface,
				       &dev->ep_out, &dev->ep_in);
		libusb_free_config_descriptor(cfg);
		if (rc != 0)
			continue;

		if (libusb_open(list[i], &dev->handle) != 0) {
			snprintf(err, errlen,
				 "cannot open the printer (permissions?)");
			continue;
		}

		if (fetch_device_id(dev->handle, dev->interface,
				    dev->device_id,
				    sizeof(dev->device_id)) != 0)
			dev->device_id[0] = '\0';

		if (device_id_field(dev->device_id, "SERN", sern,
				    sizeof(sern)) != 0)
			device_id_field(dev->device_id, "SN", sern,
					sizeof(sern));
		if (!sern[0] && dd.iSerialNumber)
			libusb_get_string_descriptor_ascii(dev->handle,
				dd.iSerialNumber, (unsigned char *) sern,
				sizeof(sern));

		if (serial && *serial && strcmp(serial, sern) != 0) {
			libusb_close(dev->handle);
			dev->handle = NULL;
			continue;
		}
		break;
	}
	if (list)
		libusb_free_device_list(list, 1);

	if (!dev->handle) {
		libusb_exit(dev->ctx);
		free(dev);
		return NULL;
	}

	/* usblp will have grabbed the printer; take it away politely. */
	libusb_set_auto_detach_kernel_driver(dev->handle, 1);
	if (libusb_kernel_driver_active(dev->handle, dev->interface) == 1)
		libusb_detach_kernel_driver(dev->handle, dev->interface);

	rc = libusb_claim_interface(dev->handle, dev->interface);
	if (rc != 0) {
		snprintf(err, errlen, "cannot claim the printer interface: %s",
			 libusb_strerror((enum libusb_error) rc));
		libusb_close(dev->handle);
		libusb_exit(dev->ctx);
		free(dev);
		return NULL;
	}
	dev->claimed = 1;
	resync_endpoints(dev);

	return dev;
}

/*
 * Last resort when the printer will not talk at all: a port reset,
 * which makes the device forget everything and come back fresh.  The
 * handle does not survive, so the caller has to reopen.
 */
int capt_usb_reset(struct capt_dev *dev)
{
	int rc;

	if (!dev || dev->sink)
		return 0;
	if (!dev->handle)
		return -1;
	fprintf(stderr, "DEBUG: capt6000: resetting the USB device\n");
	rc = libusb_reset_device(dev->handle);
	if (rc != 0 && rc != LIBUSB_ERROR_NOT_FOUND) {
		fprintf(stderr, "ERROR: capt6000: USB reset failed: %s\n",
			libusb_strerror((enum libusb_error) rc));
		return -1;
	}
	return 0;
}

void capt_close(struct capt_dev *dev)
{
	if (!dev)
		return;
	if (dev->sink) {
		fclose(dev->sink);
		free(dev);
		return;
	}
	if (dev->claimed)
		libusb_release_interface(dev->handle, dev->interface);
	if (dev->handle)
		libusb_close(dev->handle);
	if (dev->ctx)
		libusb_exit(dev->ctx);
	free(dev);
}

/* ------------------------------------------------------------------ */
/* Packet layer                                                        */
/* ------------------------------------------------------------------ */

static int bulk_write(struct capt_dev *dev, const uint8_t *buf, size_t size)
{
	trace_buf("send", buf, size);
	if (dev->sink)
		return fwrite(buf, 1, size, dev->sink) == size ? 0 : -1;
	while (size) {
		int chunk = size > INT32_MAX ? INT32_MAX : (int) size;
		int done = 0;
		int rc = libusb_bulk_transfer(dev->handle, dev->ep_out,
					      (unsigned char *) buf, chunk,
					      &done, IO_TIMEOUT_MS);
		if (rc != 0 && !(rc == LIBUSB_ERROR_TIMEOUT && done > 0)) {
			fprintf(stderr, "ERROR: capt6000: USB write failed: "
					"%s\n",
				libusb_strerror((enum libusb_error) rc));
			return -1;
		}
		buf += done;
		size -= (size_t) done;
	}
	return 0;
}

static int bulk_read(struct capt_dev *dev, uint8_t *buf, size_t size,
		     int timeout_ms)
{
	int done = 0;
	int rc = libusb_bulk_transfer(dev->handle, dev->ep_in, buf,
				      (int) size, &done, timeout_ms);

	if (rc != 0 && rc != LIBUSB_ERROR_TIMEOUT) {
		fprintf(stderr, "ERROR: capt6000: USB read failed: %s\n",
			libusb_strerror((enum libusb_error) rc));
		return -1;
	}
	if (rc == LIBUSB_ERROR_TIMEOUT && done == 0) {
		fprintf(stderr, "ERROR: capt6000: printer did not reply\n");
		return -1;
	}
	trace_buf("recv", buf, (size_t) done);
	return done;
}

static size_t frame(uint8_t *out, uint16_t cmd, const void *data, size_t size)
{
	uint16_t total = (uint16_t) (size + 4);

	out[0] = (uint8_t) (cmd & 0xFF);
	out[1] = (uint8_t) (cmd >> 8);
	out[2] = (uint8_t) (total & 0xFF);
	out[3] = (uint8_t) (total >> 8);
	if (size)
		memcpy(out + 4, data, size);
	return size + 4;
}

int capt_send(struct capt_dev *dev, uint16_t cmd, const void *data, size_t size)
{
	uint8_t hdr[4];
	uint8_t *packet;
	int rc;

	if (size + 4 > 0xFFFF) {
		fprintf(stderr, "ERROR: capt6000: packet too large\n");
		return -1;
	}
	if (dev->sink && cmd == CAPT_PRINT_DATA_END)
		dev->sink_pages += 1;
	if (size == 0) {
		frame(hdr, cmd, NULL, 0);
		return bulk_write(dev, hdr, 4);
	}
	packet = malloc(size + 4);
	if (!packet)
		return -1;
	frame(packet, cmd, data, size);
	rc = bulk_write(dev, packet, size + 4);
	free(packet);
	return rc;
}

/* Some firmware revisions encode the reply size in BCD.  Really. */
static uint16_t decode_size(uint8_t lo, uint8_t hi, size_t got)
{
	uint16_t bin = (uint16_t) (lo | (hi << 8));
	unsigned a = (hi >> 4) & 0xF, b = hi & 0xF;
	unsigned c = (lo >> 4) & 0xF, d = lo & 0xF;

	if (bin == got)
		return bin;
	if (a <= 9 && b <= 9 && c <= 9 && d <= 9) {
		uint16_t bcd = (uint16_t) (a * 1000 + b * 100 + c * 10 + d);
		if (bcd == got)
			return bcd;
	}
	return bin;
}

/*
 * Offline mode reply generator.  Produces the shape of answer the
 * state machine expects so the whole pipeline can be exercised, and
 * nothing more: it is a test scaffold, not a printer emulator.
 */
static size_t sink_reply(struct capt_dev *dev, uint16_t cmd, uint8_t *buf,
			 size_t bufsize)
{
	size_t payload = 2;

	memset(buf, 0, bufsize);
	buf[0] = (uint8_t) (cmd & 0xFF);
	buf[1] = (uint8_t) (cmd >> 8);

	switch (cmd) {
	case CAPT_CHKSTATUS:
	case CAPT_CHKXSTATUS:
	case CAPT_CHKJOBSTAT:
		payload = 84;
		/* every page counter reports the last page we were
		 * handed, so the state machine walks straight through */
		buf[4 + 14] = (uint8_t) (dev->sink_pages & 0xFF);
		buf[4 + 15] = (uint8_t) (dev->sink_pages >> 8);
		buf[4 + 16] = buf[4 + 14];
		buf[4 + 17] = buf[4 + 15];
		buf[4 + 18] = buf[4 + 14];
		buf[4 + 19] = buf[4 + 15];
		buf[4 + 20] = buf[4 + 14];
		buf[4 + 21] = buf[4 + 15];
		buf[4 + 34] = buf[4 + 14];
		buf[4 + 35] = buf[4 + 15];
		break;
	case CAPT_JOB_BEGIN:
		payload = 4;
		buf[4 + 2] = 0x01;	/* job id 1 */
		break;
	default:
		break;
	}

	buf[2] = (uint8_t) ((payload + 4) & 0xFF);
	buf[3] = (uint8_t) ((payload + 4) >> 8);
	return payload + 4;
}

int capt_sendrecv(struct capt_dev *dev, uint16_t cmd, const void *data,
		  size_t size, uint8_t *reply, size_t *reply_size)
{
	uint8_t buf[REPLY_BUF];
	size_t got = 0;
	uint16_t want;
	int n;

	if (capt_send(dev, cmd, data, size) < 0)
		return -1;

	if (dev->sink) {
		n = (int) sink_reply(dev, cmd, buf, sizeof(buf));
	} else {
		n = bulk_read(dev, buf, sizeof(buf), IO_TIMEOUT_MS);
	}
	if (n < 0)
		return -1;
	got = (size_t) n;

	if (got < 4) {
		fprintf(stderr, "ERROR: capt6000: short reply (%zu bytes) to "
				"command %04X\n", got, cmd);
		return -1;
	}
	if ((buf[0] | (buf[1] << 8)) != cmd) {
		fprintf(stderr, "ERROR: capt6000: reply %02X%02X does not "
				"match command %04X\n", buf[1], buf[0], cmd);
		return -1;
	}

	want = decode_size(buf[2], buf[3], got);
	while (got < want && got < sizeof(buf)) {
		n = bulk_read(dev, buf + got, sizeof(buf) - got, IO_TIMEOUT_MS);
		if (n <= 0)
			break;
		got += (size_t) n;
	}
	if (got < want)
		fprintf(stderr, "DEBUG: capt6000: reply to %04X declared %u "
				"bytes, got %zu\n", cmd, want, got);

	if (reply) {
		size_t payload = got > 4 ? got - 4 : 0;
		size_t copy = reply_size && *reply_size < payload
			      ? *reply_size : payload;
		memcpy(reply, buf + 4, copy);
	}
	if (reply_size)
		*reply_size = got > 4 ? got - 4 : 0;
	return 0;
}

void capt_multi_begin(struct capt_multi *m, uint16_t cmd)
{
	m->buf[0] = (uint8_t) (cmd & 0xFF);
	m->buf[1] = (uint8_t) (cmd >> 8);
	m->size = 4;
}

int capt_multi_add(struct capt_multi *m, uint16_t cmd, const void *data,
		   size_t size)
{
	if (m->size + size + 4 > sizeof(m->buf))
		return -1;
	m->size += frame(m->buf + m->size, cmd, data, size);
	return 0;
}

int capt_multi_send(struct capt_dev *dev, struct capt_multi *m)
{
	m->buf[2] = (uint8_t) (m->size & 0xFF);
	m->buf[3] = (uint8_t) (m->size >> 8);
	return bulk_write(dev, m->buf, m->size);
}

/* ------------------------------------------------------------------ */
/* Status                                                              */
/* ------------------------------------------------------------------ */

static void decode_status(struct capt_status *s, const uint8_t *b, size_t size)
{
#define W(i)	((uint16_t) (b[i] | (b[(i) + 1] << 8)))
	memset(s, 0, sizeof(*s));
	if (size < 2)
		return;
	s->valid = 1;
	s->word[0] = W(0);
	if (size < 10)
		return;
	s->word[1] = W(8);
	if (size < 12)
		return;
	s->word[2] = W(10);
	if (size < 22)
		return;
	s->word[3] = W(12);
	s->page_decoding = W(14);
	s->page_printing = W(16);
	s->page_out = W(18);
	s->page_completed = W(20);
	if (size >= 26)
		s->word[4] = W(24);
	if (size >= 32)
		s->word[5] = W(30);
	if (size >= 36)
		s->page_received = W(34);
	if (size >= 40)
		s->word[6] = W(38);
#undef W
}

static int download_status(struct capt_dev *dev, uint16_t cmd,
			   struct capt_status *out)
{
	uint8_t buf[REPLY_BUF];
	size_t size = sizeof(buf);

	if (capt_sendrecv(dev, cmd, NULL, 0, buf, &size) < 0)
		return -1;
	if (size > sizeof(buf))
		size = sizeof(buf);
	decode_status(out, buf, size);

	if (status_trace) {
		static struct capt_status prev;
		static int have_prev;

		if (!have_prev || memcmp(&prev, out, sizeof(prev)) != 0) {
			fprintf(stderr, "DEBUG: capt6000: status %04X %04X "
					"%04X %04X %04X %04X %04X  pages "
					"dec=%u prn=%u out=%u done=%u rcv=%u\n",
				out->word[0], out->word[1], out->word[2],
				out->word[3], out->word[4], out->word[5],
				out->word[6], out->page_decoding,
				out->page_printing, out->page_out,
				out->page_completed, out->page_received);
			prev = *out;
			have_prev = 1;
		}
	}
	return 0;
}

int capt_get_status(struct capt_dev *dev, struct capt_status *out)
{
	return download_status(dev, CAPT_CHKSTATUS, out);
}

int capt_get_xstatus(struct capt_dev *dev, struct capt_status *out)
{
	return download_status(dev, CAPT_CHKXSTATUS, out);
}

int capt_wait_ready(struct capt_dev *dev, struct capt_status *out)
{
	struct capt_status local;
	unsigned tries = 0;

	if (!out)
		out = &local;
	for (;;) {
		if (capt_get_xstatus(dev, out) < 0)
			return -1;
		if (!capt_flag(out, CAPT_FL_BUSY))
			return 0;
		if (++tries > 600) {
			fprintf(stderr, "ERROR: capt6000: printer stayed busy "
					"for 10 minutes, giving up\n");
			return -1;
		}
		usleep(200000);
	}
}
