/*
 * test_hiscoa.c -- round-trip and edge case tests for the Hi-SCoA codec.
 * Part of capt6000, a CUPS driver for the Canon LBP6000/LBP6018.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Copyright (C) 2026  Viktor Hozhyi <https://github.com/ViktorGozhiy/capt6000>
 *
 * This program is free software under the terms of the GNU General
 * Public License, version 3 or any later version; see the file
 * LICENSE.  There is NO WARRANTY, to the extent permitted by law.
 */

#include "../src/hiscoa.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures;
static unsigned checks;

static uint32_t rnd_state = 0x12345678u;

static uint32_t rnd(void)
{
	rnd_state ^= rnd_state << 13;
	rnd_state ^= rnd_state >> 17;
	rnd_state ^= rnd_state << 5;
	return rnd_state;
}

static void roundtrip(const char *name, const uint8_t *band,
		      unsigned line_size, unsigned nlines, enum hiscoa_eob eob)
{
	size_t raw = (size_t) line_size * nlines;
	size_t cap = raw * 2 + 4096;
	uint8_t *comp = malloc(cap);
	uint8_t *back = malloc(raw + 64);
	size_t csize, dsize = 0, consumed = 0;
	enum hiscoa_eob got_eob = HISCOA_EOB_BAND;

	++checks;
	csize = hiscoa_compress_band(comp, cap, band, line_size, nlines, eob,
				     &hiscoa_default_params);
	if (csize == HISCOA_OVERFLOW) {
		printf("FAIL %-28s compressor reported overflow\n", name);
		++failures;
		goto out;
	}
	if (csize % 4) {
		printf("FAIL %-28s output %zu bytes, not 32 bit aligned\n",
		       name, csize);
		++failures;
		goto out;
	}
	if (hiscoa_decompress_band(comp, csize, back, raw + 64, &dsize,
				   line_size, &hiscoa_default_params,
				   &got_eob, &consumed) != 0) {
		printf("FAIL %-28s stream does not decode\n", name);
		++failures;
		goto out;
	}
	if (dsize != raw) {
		printf("FAIL %-28s decoded %zu bytes, expected %zu\n",
		       name, dsize, raw);
		++failures;
		goto out;
	}
	if (memcmp(band, back, raw) != 0) {
		size_t i;
		for (i = 0; i < raw && band[i] == back[i]; ++i)
			;
		printf("FAIL %-28s payload differs at byte %zu (%02X != %02X)\n",
		       name, i, band[i], back[i]);
		++failures;
		goto out;
	}
	if (got_eob != eob) {
		printf("FAIL %-28s eob %d, expected %d\n",
		       name, (int) got_eob, (int) eob);
		++failures;
		goto out;
	}
	if (consumed != csize) {
		printf("FAIL %-28s decoder consumed %zu of %zu bytes\n",
		       name, consumed, csize);
		++failures;
		goto out;
	}
	printf("ok   %-28s %6zu -> %6zu bytes (%.1f%%)\n",
	       name, raw, csize, 100.0 * (double) csize / (double) raw);
out:
	free(comp);
	free(back);
}

/* A page-like pattern: text-ish blobs on a white background. */
static void make_textish(uint8_t *buf, unsigned line_size, unsigned nlines)
{
	unsigned y, x;

	memset(buf, 0, (size_t) line_size * nlines);
	for (y = 0; y < nlines; ++y) {
		if ((y / 24) % 3 == 2)
			continue;			/* line spacing */
		for (x = 8; x + 8 < line_size; ++x) {
			if ((x / 3 + y / 7) % 5 == 0)
				buf[(size_t) y * line_size + x] =
					(uint8_t) (0xE0 >> (x % 3));
		}
	}
}

int main(void)
{
	static const unsigned line_size = 592;
	static const unsigned nlines = 160;
	size_t raw = (size_t) line_size * nlines;
	uint8_t *buf = malloc(raw);
	uint8_t small[64];
	unsigned i;

	/* All white: the common case, must compress enormously. */
	memset(buf, 0x00, raw);
	roundtrip("blank band", buf, line_size, nlines, HISCOA_EOB_BAND);

	/* All black. */
	memset(buf, 0xFF, raw);
	roundtrip("solid black band", buf, line_size, nlines, HISCOA_EOB_BAND);

	/* Vertical stripes: perfect line-to-line repetition. */
	for (i = 0; i < raw; ++i)
		buf[i] = (uint8_t) ((i % line_size) % 2 ? 0xAA : 0x55);
	roundtrip("vertical stripes", buf, line_size, nlines, HISCOA_EOB_BAND);

	/* Horizontal stripes: no line-to-line repetition. */
	for (i = 0; i < raw; ++i)
		buf[i] = (uint8_t) ((i / line_size) % 2 ? 0xFF : 0x00);
	roundtrip("horizontal stripes", buf, line_size, nlines, HISCOA_EOB_BAND);

	make_textish(buf, line_size, nlines);
	roundtrip("text-like page", buf, line_size, nlines, HISCOA_EOB_PAGE);

	/* Incompressible data: the worst case for the format. */
	for (i = 0; i < raw; ++i)
		buf[i] = (uint8_t) rnd();
	roundtrip("random noise", buf, line_size, nlines, HISCOA_EOB_BAND);

	/* Degenerate geometries. */
	for (i = 0; i < sizeof(small); ++i)
		small[i] = (uint8_t) (i * 7);
	roundtrip("single line", small, sizeof(small), 1, HISCOA_EOB_PAGE);
	roundtrip("one byte lines", small, 1, sizeof(small), HISCOA_EOB_BAND);

	/* Long runs, to exercise PREFIX coded match lengths. */
	memset(buf, 0, raw);
	memset(buf + 1000, 0x5A, 2000);
	roundtrip("long run", buf, line_size, nlines, HISCOA_EOB_BAND);

	/* Random bands of random shapes. */
	for (i = 0; i < 40; ++i) {
		unsigned ls = 1 + rnd() % 700;
		unsigned nl = 1 + rnd() % 40;
		unsigned j;
		char name[64];

		for (j = 0; j < ls * nl; ++j) {
			uint32_t r = rnd();
			/* biased towards runs of zeros, like real pages */
			buf[j] = (r % 8) ? 0x00 : (uint8_t) (r >> 8);
		}
		snprintf(name, sizeof(name), "fuzz %ux%u", ls, nl);
		roundtrip(name, buf, ls, nl, HISCOA_EOB_BAND);
	}

	/* The compressor must report, not silently truncate, an overflow. */
	++checks;
	{
		uint8_t tiny[8];
		size_t r;
		for (i = 0; i < raw; ++i)
			buf[i] = (uint8_t) rnd();
		r = hiscoa_compress_band(tiny, sizeof(tiny), buf, line_size,
					 nlines, HISCOA_EOB_BAND,
					 &hiscoa_default_params);
		if (r != HISCOA_OVERFLOW) {
			printf("FAIL %-28s overflow not detected (got %zu)\n",
			       "overflow detection", r);
			++failures;
		} else {
			printf("ok   %-28s overflow reported\n",
			       "overflow detection");
		}
	}

	/* A truncated stream must be rejected, not decoded into garbage. */
	++checks;
	{
		uint8_t comp[8192];
		uint8_t back[4096];
		size_t csize;

		memset(buf, 0x33, 1024);
		csize = hiscoa_compress_band(comp, sizeof(comp), buf, 64, 16,
					     HISCOA_EOB_BAND,
					     &hiscoa_default_params);
		if (csize == HISCOA_OVERFLOW ||
		    hiscoa_decompress_band(comp, csize - 4, back, sizeof(back),
					   NULL, 64, &hiscoa_default_params,
					   NULL, NULL) == 0) {
			printf("FAIL %-28s truncated stream accepted\n",
			       "truncation detection");
			++failures;
		} else {
			printf("ok   %-28s truncated stream rejected\n",
			       "truncation detection");
		}
	}

	free(buf);
	printf("\n%u checks, %u failures\n", checks, failures);
	return failures ? 1 : 0;
}
