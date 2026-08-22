/*
 * captdump.c -- inspect a capt6000 record stream.
 * Part of capt6000, a CUPS driver for the Canon LBP6000/LBP6018.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Copyright (C) 2026  Viktor Hozhyi <https://github.com/ViktorGozhiy/capt6000>
 *
 * This program is free software under the terms of the GNU General
 * Public License, version 3 or any later version; see the file
 * LICENSE.  There is NO WARRANTY, to the extent permitted by law.
 *
 * Decodes the stream the filter produces: prints the page parameters
 * in human readable form and, with -o, decompresses every page back
 * into a PBM image so the output can actually be looked at.
 */

#include "../src/captstream.h"
#include "../src/hiscoa.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t get_le32(const uint8_t *p)
{
	return (uint32_t) p[0] | ((uint32_t) p[1] << 8) |
	       ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static uint16_t get_le16(const uint8_t *p)
{
	return (uint16_t) (p[0] | (p[1] << 8));
}

static const char *media_name(unsigned m)
{
	switch (m) {
	case 0x01: return "Plain";
	case 0x04: return "Heavy/Label";
	case 0x05: return "Heavy H";
	case 0x08: return "Plain L";
	case 0x20: return "Envelope";
	case 0x24: return "Transparency";
	default:   return "?";
	}
}

static void print_page_params(const uint8_t *p)
{
	printf("    page prologue (0xD0A0):\n");
	printf("      model magic     0x%02X%02X\n", p[3], p[2]);
	printf("      paper size id   0x%02X\n", p[4]);
	printf("      toner density   %u\n", p[8] >> 2);
	printf("      media type      0x%02X (%s)\n", p[12], media_name(p[12]));
	printf("      media adapt     0x%02X\n", p[13]);
	printf("      image refine    0x%02X\n", p[18]);
	printf("      toner save      %u\n", p[19]);
	printf("      bound a / b     %u / %u px\n",
	       get_le16(p + 22), get_le16(p + 24));
	printf("      image           %u bytes/line x %u lines\n",
	       get_le16(p + 26), get_le16(p + 28));
	printf("      sheet           %u x %u px @600dpi (%.1f x %.1f mm)\n",
	       get_le16(p + 30), get_le16(p + 32),
	       get_le16(p + 30) * 25.4 / 600.0,
	       get_le16(p + 32) * 25.4 / 600.0);
	printf("      fuser mode      0x%02X\n", p[36]);
}

static void write_pbm(const char *dir, unsigned page, const uint8_t *bits,
		      unsigned line_size, unsigned lines)
{
	char path[512];
	FILE *f;

	snprintf(path, sizeof(path), "%s/page-%03u.pbm", dir, page);
	f = fopen(path, "wb");
	if (!f) {
		fprintf(stderr, "cannot write %s: %s\n", path, strerror(errno));
		return;
	}
	fprintf(f, "P4\n%u %u\n", line_size * 8, lines);
	fwrite(bits, 1, (size_t) line_size * lines, f);
	fclose(f);
	printf("    wrote %s (%u x %u)\n", path, line_size * 8, lines);
}

int main(int argc, char *argv[])
{
	const char *outdir = NULL;
	const char *path = NULL;
	FILE *in;
	uint8_t magic[8];
	uint8_t *buf = NULL;
	size_t cap = 0;
	struct captstream_page page;
	uint8_t *canvas = NULL;
	size_t canvas_pos = 0, canvas_size = 0;
	unsigned page_no = 0, band_no = 0;
	unsigned long total_bands = 0, total_bytes = 0;
	int i;

	for (i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
			outdir = argv[++i];
		else
			path = argv[i];
	}
	if (!path) {
		fprintf(stderr, "usage: captdump [-o outdir] stream\n");
		return 2;
	}

	in = fopen(path, "rb");
	if (!in) {
		fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno));
		return 1;
	}
	if (fread(magic, 1, 8, in) != 8 ||
	    memcmp(magic, CAPTSTREAM_MAGIC, 8) != 0) {
		fprintf(stderr, "%s is not a capt6000 stream\n", path);
		return 1;
	}
	printf("capt6000 stream: %s\n", path);

	for (;;) {
		uint8_t hdr[8];
		uint32_t type, length;

		if (fread(hdr, 1, 8, in) != 8)
			break;
		type = get_le32(hdr);
		length = get_le32(hdr + 4);
		if (length > cap) {
			buf = realloc(buf, length + 1);
			cap = length;
		}
		if (length && fread(buf, 1, length, in) != length) {
			fprintf(stderr, "truncated record\n");
			return 1;
		}

		switch (type) {
		case CAPTREC_JOB_BEGIN:
			printf("  JOB_BEGIN version %u\n",
			       length >= 4 ? get_le32(buf) : 0);
			break;

		case CAPTREC_PAGE_BEGIN:
			memcpy(&page, buf, sizeof(page));
			++page_no;
			band_no = 0;
			printf("  PAGE %u\n", page_no);
			print_page_params(page.page_params);
			printf("    hiscoa params   %02X %02X %02X %02X "
			       "%02X %02X %02X %02X\n",
			       page.hiscoa_params[0], page.hiscoa_params[1],
			       page.hiscoa_params[2], page.hiscoa_params[3],
			       page.hiscoa_params[4], page.hiscoa_params[5],
			       page.hiscoa_params[6], page.hiscoa_params[7]);
			printf("    band height     %u lines\n",
			       get_le32((uint8_t *) &page.band_lines));
			canvas_size = (size_t) get_le32((uint8_t *) &page.line_size) *
				      get_le32((uint8_t *) &page.num_lines);
			free(canvas);
			canvas = calloc(1, canvas_size + 4096);
			canvas_pos = 0;
			break;

		case CAPTREC_BAND:
			{
				size_t out_len = 0;
				enum hiscoa_eob eob;
				unsigned ls = get_le32((uint8_t *) &page.line_size);

				++band_no;
				++total_bands;
				total_bytes += length;
				if (!canvas)
					break;
				if (hiscoa_decompress_band(buf, length,
						canvas + canvas_pos,
						canvas_size + 4096 - canvas_pos,
						&out_len, ls,
						&hiscoa_default_params,
						&eob, NULL) != 0) {
					printf("    band %u: DECODE FAILED "
					       "(%u bytes)\n", band_no, length);
					break;
				}
				canvas_pos += out_len;
				printf("    band %2u: %6u bytes -> %6zu "
				       "raster bytes, eob=%d\n",
				       band_no, length, out_len, (int) eob);
			}
			break;

		case CAPTREC_PAGE_END:
			printf("    page total: %zu raster bytes decoded of "
			       "%zu expected\n", canvas_pos, canvas_size);
			if (outdir && canvas)
				write_pbm(outdir, page_no, canvas,
					  get_le32((uint8_t *) &page.line_size),
					  get_le32((uint8_t *) &page.num_lines));
			break;

		case CAPTREC_JOB_END:
			printf("  JOB_END\n");
			break;

		default:
			printf("  unknown record %u (%u bytes)\n", type, length);
			break;
		}
	}

	printf("\n%u page(s), %lu band(s), %lu compressed bytes\n",
	       page_no, total_bands, total_bytes);
	free(canvas);
	free(buf);
	fclose(in);
	return 0;
}
