/*
 * rastertolbp6000.c -- CUPS raster filter for Canon LBP6000/LBP6018.
 *
 * Part of capt6000, a CUPS driver for the Canon LBP6000/LBP6018.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Copyright (C) 2026  Viktor Hozhyi <https://github.com/ViktorGozhiy/capt6000>
 *
 * The layout of the page prologue, command 0xD0A0, comes from the
 * captdriver project:
 *   Copyright (C) 2013  Alexey Galakhov <agalakhov@gmail.com>
 *   Copyright (C) 2016  Alexei Gordeev <KP1533TM2@gmail.com>
 *
 * This program is free software under the terms of the GNU General
 * Public License, version 3 or any later version; see the file
 * LICENSE.  There is NO WARRANTY, to the extent permitted by law.
 *
 * Reads a CUPS raster stream and writes the capt6000 record stream
 * (see captstream.h) that the "capt" backend turns into a CAPT
 * conversation with the printer.
 *
 * Usage: rastertolbp6000 job user title copies options [file]
 */

#include "captstream.h"
#include "hiscoa.h"
#include "pagegeom.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cups/cups.h>
#include <cups/raster.h>

#define DEFAULT_BAND_LINES	160
#define DEFAULT_DARKNESS	7
#define MODEL_MAGIC_LO		0x30	/* bytes 2-3 of the page prologue */
#define MODEL_MAGIC_HI		0x2A

/* Media type codes as understood by CAPT 3 printers. */
enum media_type {
	MEDIA_PLAIN	= 0x01,
	MEDIA_PLAIN_L	= 0x08,
	MEDIA_HEAVY	= 0x04,
	MEDIA_HEAVY_H	= 0x05,
	MEDIA_TRANSP	= 0x24,
	MEDIA_LABEL	= 0x04,
	MEDIA_ENVELOPE	= 0x20
};

static volatile sig_atomic_t cancelled;

static void on_cancel(int sig)
{
	(void) sig;
	cancelled = 1;
}

static uint8_t fuser_mode(unsigned media)
{
	switch (media) {
	case MEDIA_PLAIN:	return 0x01;
	case MEDIA_PLAIN_L:	return 0x01;
	case MEDIA_HEAVY:	return 0x02;	/* also MEDIA_LABEL */
	case MEDIA_HEAVY_H:	return 0x02;
	case MEDIA_TRANSP:	return 0x13;
	case MEDIA_ENVELOPE:	return 0x1C;
	default:		return 0x01;
	}
}

static void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t) (v & 0xFF);
	p[1] = (uint8_t) ((v >> 8) & 0xFF);
	p[2] = (uint8_t) ((v >> 16) & 0xFF);
	p[3] = (uint8_t) ((v >> 24) & 0xFF);
}

static void put_le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t) (v & 0xFF);
	p[1] = (uint8_t) (v >> 8);
}

static int write_all(const void *buf, size_t size)
{
	const uint8_t *p = buf;

	while (size) {
		ssize_t n = write(STDOUT_FILENO, p, size);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		p += n;
		size -= (size_t) n;
	}
	return 0;
}

static int write_record(uint32_t type, const void *data, uint32_t length)
{
	uint8_t hdr[8];

	put_le32(hdr, type);
	put_le32(hdr + 4, length);
	if (write_all(hdr, sizeof(hdr)) < 0)
		return -1;
	if (length && write_all(data, length) < 0)
		return -1;
	return 0;
}

/* Build the 40 byte payload of command 0xD0A0. */
static void build_page_params(uint8_t out[40],
			      const struct page_geometry *geom,
			      unsigned media, unsigned media_adapt,
			      unsigned darkness, unsigned toner_save)
{
	memset(out, 0, 40);

	out[2] = MODEL_MAGIC_LO;
	out[3] = MODEL_MAGIC_HI;
	out[4] = geom->size_id;

	out[8] = (uint8_t) ((darkness & 0x0F) << 2);	/* black density */
	out[9] = 0x1C;					/* unused colour */
	out[10] = 0x1C;
	out[11] = 0x1C;

	out[12] = (uint8_t) media;
	out[13] = (uint8_t) media_adapt;
	out[14] = 0x04;
	out[16] = 0x01;
	out[17] = 0x01;
	out[18] = 0x02;					/* image refinement */
	out[19] = toner_save ? 0x01 : 0x00;

	put_le16(out + 22, geom->bound_a);
	put_le16(out + 24, geom->bound_b);
	put_le16(out + 26, geom->line_size);
	put_le16(out + 28, geom->num_lines);
	put_le16(out + 30, geom->paper_width);
	put_le16(out + 32, geom->paper_height);

	out[36] = fuser_mode(media);
}

struct band_buffers {
	uint8_t *line;		/* one raster line as CUPS delivers it */
	uint8_t *band;		/* band_lines x line_size, printer layout */
	uint8_t *comp;		/* compressed band */
	size_t comp_size;
	size_t line_alloc;
	size_t band_alloc;
};

static void buffers_free(struct band_buffers *b)
{
	free(b->line);
	free(b->band);
	free(b->comp);
	memset(b, 0, sizeof(*b));
}

static int buffers_alloc(struct band_buffers *b, size_t line_bytes,
			 size_t band_bytes)
{
	buffers_free(b);
	/* Worst case for Hi-SCoA is 12 bits per byte plus framing. */
	b->comp_size = band_bytes * 2 + 4096;
	b->line = calloc(1, line_bytes ? line_bytes : 1);
	b->band = calloc(1, band_bytes);
	b->comp = calloc(1, b->comp_size);
	b->line_alloc = line_bytes;
	b->band_alloc = band_bytes;
	if (!b->line || !b->band || !b->comp) {
		buffers_free(b);
		return -1;
	}
	return 0;
}

static int emit_band(struct band_buffers *b, unsigned line_size,
		     unsigned nlines, enum hiscoa_eob eob)
{
	size_t size;

	for (;;) {
		size = hiscoa_compress_band(b->comp, b->comp_size, b->band,
					    line_size, nlines, eob,
					    &hiscoa_default_params);
		if (size != HISCOA_OVERFLOW)
			break;
		/* Pathological input; give the compressor more room. */
		if (b->comp_size > 64u * 1024 * 1024) {
			fprintf(stderr, "ERROR: capt6000: band does not fit "
					"in %zu bytes\n", b->comp_size);
			return -1;
		}
		free(b->comp);
		b->comp_size *= 2;
		b->comp = malloc(b->comp_size);
		if (!b->comp) {
			fprintf(stderr, "ERROR: capt6000: out of memory\n");
			return -1;
		}
	}
	return write_record(CAPTREC_BAND, b->comp, (uint32_t) size);
}

/*
 * Work out where the top left corner of the raster lands in the
 * printer's imaging area, which starts bound_a pixels in from the top
 * left corner of the sheet.  The rasterizer says where the raster sits
 * on the sheet in cupsImagingBBox: Ghostscript renders exactly the
 * imageable area, but imagetoraster renders only the image and places
 * it by the bounding box.  The column is rounded to whole bytes; it is
 * negative when the raster starts outside the imaging area.
 */
static void raster_origin(const cups_page_header2_t *header,
			  const struct page_geometry *geom,
			  long *x_bytes, long *y_lines)
{
	const float *bbox = header->cupsImagingBBox;

	if (bbox[2] <= bbox[0] || bbox[3] <= bbox[1] ||
	    header->cupsPageSize[1] <= 0) {
		/* No position given: center horizontally, start at the top. */
		*x_bytes = ((long) geom->line_size -
			    (long) header->cupsBytesPerLine) / 2;
		*y_lines = 0;
		return;
	}
	*x_bytes = lround((bbox[0] * header->HWResolution[0] / 72.0 -
			   geom->bound_a) / 8.0);
	*y_lines = lround((header->cupsPageSize[1] - bbox[3]) *
			  header->HWResolution[1] / 72.0 - geom->bound_a);
}

static int read_raster_line(cups_raster_t *raster,
			    const cups_page_header2_t *header, uint8_t *line,
			    unsigned page_no, unsigned line_no)
{
	if (cupsRasterReadPixels(raster, line, header->cupsBytesPerLine)
	    != header->cupsBytesPerLine) {
		fprintf(stderr, "ERROR: capt6000: truncated raster on page %u "
				"line %u\n", page_no, line_no);
		return -1;
	}
	return 0;
}

static int process_page(cups_raster_t *raster, cups_page_header2_t *header,
			unsigned page_no, struct band_buffers *bufs)
{
	struct page_geometry custom;
	const struct page_geometry *geom;
	struct captstream_page rec;
	uint8_t page_params[40];
	unsigned band_lines = header->cupsRowCount;
	unsigned media, media_adapt, darkness, toner_save;
	unsigned line_size, num_lines;
	unsigned copy_bytes, dst_off, src_off;
	unsigned line;
	unsigned read_lines = 0;
	unsigned emitted = 0;
	long x_bytes, y_lines;

	geom = page_geometry_find(header->cupsPageSizeName);
	if (!geom) {
		page_geometry_custom(&custom, header->PageSize[0],
				     header->PageSize[1]);
		geom = &custom;
		fprintf(stderr, "DEBUG: capt6000: page size '%s' is not in the "
				"model table, treating as custom\n",
			header->cupsPageSizeName);
	}

	line_size = geom->line_size;
	num_lines = geom->num_lines;

	if (band_lines == 0 || band_lines > num_lines)
		band_lines = DEFAULT_BAND_LINES;
	if (band_lines > num_lines)
		band_lines = num_lines;

	media = header->cupsMediaType ? header->cupsMediaType : MEDIA_PLAIN;
	media_adapt = header->cupsInteger[3] ? header->cupsInteger[3] : 0x11;
	toner_save = header->cupsInteger[0];
	darkness = header->cupsInteger[1] ? header->cupsInteger[1]
					  : DEFAULT_DARKNESS;

	if (header->cupsBitsPerPixel != 1) {
		fprintf(stderr, "ERROR: capt6000: this driver needs 1 bit per "
				"pixel raster data, got %u\n",
			header->cupsBitsPerPixel);
		return -1;
	}

	fprintf(stderr, "DEBUG: capt6000: page %u: %s, %ux%u px sheet, "
			"%u bytes/line x %u lines, %u lines/band\n",
		page_no, geom->ppd_name ? geom->ppd_name : "Custom",
		geom->paper_width, geom->paper_height,
		line_size, num_lines, band_lines);

	raster_origin(header, geom, &x_bytes, &y_lines);

	if (header->cupsBytesPerLine != line_size ||
	    header->cupsHeight != num_lines || x_bytes != 0 || y_lines != 0)
		fprintf(stderr, "DEBUG: capt6000: raster is %u bytes/line x %u "
				"lines, printer wants %u x %u; placing it at "
				"byte %ld, line %ld\n",
			header->cupsBytesPerLine, header->cupsHeight,
			line_size, num_lines, x_bytes, y_lines);

	/* The part of each raster line that falls into the imaging area. */
	dst_off = x_bytes > 0 ? (unsigned) x_bytes : 0;
	src_off = x_bytes < 0 ? (unsigned) -x_bytes : 0;
	copy_bytes = 0;
	if (src_off < header->cupsBytesPerLine && dst_off < line_size) {
		copy_bytes = header->cupsBytesPerLine - src_off;
		if (copy_bytes > line_size - dst_off)
			copy_bytes = line_size - dst_off;
	}

	if (buffers_alloc(bufs, header->cupsBytesPerLine,
			  (size_t) line_size * band_lines) < 0) {
		fprintf(stderr, "ERROR: capt6000: out of memory\n");
		return -1;
	}

	build_page_params(page_params, geom, media, media_adapt, darkness,
			  toner_save);

	memset(&rec, 0, sizeof(rec));
	memcpy(rec.page_params, page_params, sizeof(page_params));
	hiscoa_format_params(rec.hiscoa_params, &hiscoa_default_params);
	put_le32((uint8_t *) &rec.line_size, line_size);
	put_le32((uint8_t *) &rec.num_lines, num_lines);
	put_le32((uint8_t *) &rec.band_lines, band_lines);

	if (write_record(CAPTREC_PAGE_BEGIN, &rec, sizeof(rec)) < 0)
		return -1;

	for (line = 0; line < num_lines && !cancelled; ) {
		unsigned n = num_lines - line;
		unsigned i;

		if (n > band_lines)
			n = band_lines;

		memset(bufs->band, 0, bufs->band_alloc);
		for (i = 0; i < n; ++i) {
			long src_line = (long) (line + i) - y_lines;

			if (src_line < 0 ||
			    src_line >= (long) header->cupsHeight)
				continue;	/* outside the raster: white */
			while ((long) read_lines <= src_line) {
				if (read_raster_line(raster, header,
						     bufs->line, page_no,
						     read_lines) < 0)
					return -1;
				++read_lines;
			}
			memcpy(bufs->band + (size_t) i * line_size + dst_off,
			       bufs->line + src_off, copy_bytes);
		}

		line += n;
		if (emit_band(bufs, line_size, n,
			      line >= num_lines ? HISCOA_EOB_PAGE
						: HISCOA_EOB_BAND) < 0)
			return -1;
		++emitted;
	}

	/* Drain any raster lines beyond the printable area. */
	for (; read_lines < header->cupsHeight; ++read_lines)
		cupsRasterReadPixels(raster, bufs->line,
				     header->cupsBytesPerLine);

	fprintf(stderr, "DEBUG: capt6000: page %u encoded into %u bands\n",
		page_no, emitted);

	return write_record(CAPTREC_PAGE_END, NULL, 0);
}

int main(int argc, char *argv[])
{
	cups_raster_t *raster;
	cups_page_header2_t header;
	struct band_buffers bufs;
	struct captstream_job job;
	unsigned page_no = 0;
	int fd = STDIN_FILENO;
	int rc = 0;

	if (argc < 6 || argc > 7) {
		fprintf(stderr,
			"Usage: %s job-id user title copies options [file]\n",
			argv[0]);
		return 1;
	}

	signal(SIGPIPE, SIG_IGN);
	signal(SIGTERM, on_cancel);

	if (argc == 7) {
		fd = open(argv[6], O_RDONLY);
		if (fd < 0) {
			fprintf(stderr, "ERROR: capt6000: cannot open %s: %s\n",
				argv[6], strerror(errno));
			return 1;
		}
	}

	memset(&bufs, 0, sizeof(bufs));

	if (write_all(CAPTSTREAM_MAGIC, 8) < 0) {
		fprintf(stderr, "ERROR: capt6000: cannot write to backend\n");
		return 1;
	}
	memset(&job, 0, sizeof(job));
	put_le32((uint8_t *) &job.version, CAPTSTREAM_VERSION);
	if (write_record(CAPTREC_JOB_BEGIN, &job, sizeof(job)) < 0)
		return 1;

	raster = cupsRasterOpen(fd, CUPS_RASTER_READ);
	if (!raster) {
		fprintf(stderr, "ERROR: capt6000: cannot read raster stream\n");
		return 1;
	}

	while (!cancelled && cupsRasterReadHeader2(raster, &header)) {
		++page_no;
		fprintf(stderr, "PAGE: %u 1\n", page_no);
		if (process_page(raster, &header, page_no, &bufs) < 0) {
			rc = 1;
			break;
		}
	}

	cupsRasterClose(raster);
	buffers_free(&bufs);
	if (argc == 7)
		close(fd);

	if (rc == 0 && page_no == 0) {
		fprintf(stderr, "ERROR: capt6000: the job contains no pages\n");
		rc = 1;
	}

	if (write_record(CAPTREC_JOB_END, NULL, 0) < 0)
		rc = 1;

	fprintf(stderr, "DEBUG: capt6000: filter finished, %u page(s), rc=%d\n",
		page_no, rc);
	return rc;
}
