/*
 * hiscoa.h -- Hi-SCoA (Canon CAPT) compression codec.
 *
 * Part of capt6000, a CUPS driver for the Canon LBP6000/LBP6018.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Copyright (C) 2026  Viktor Hozhyi <https://github.com/ViktorGozhiy/capt6000>
 *
 * The Hi-SCoA format - its command set, its coding rules and the
 * compression strategy followed here - was reverse engineered by the
 * captdriver project, and this code is derived from it:
 *   Copyright (C) 2013  Alexey Galakhov <agalakhov@gmail.com>
 *   <https://github.com/agalakhov/captdriver>
 *
 * This program is free software under the terms of the GNU General
 * Public License, version 3 or any later version; see the file
 * LICENSE.  There is NO WARRANTY, to the extent permitted by law.
 *
 * Hi-SCoA is the band oriented LZ77-like format used by CAPT 2.x/3
 * printers.
 */

#ifndef CAPT6000_HISCOA_H
#define CAPT6000_HISCOA_H

#include <stddef.h>
#include <stdint.h>

/* Origins of the five LZ77 back-reference registers.  origin_0 and
 * origin_2 are relative to the line size, the rest are absolute. */
struct hiscoa_params {
	int origin_0;
	int origin_2;
	int origin_3;
	int origin_4;
	int origin_5;
};

extern const struct hiscoa_params hiscoa_default_params;

/* End-of-band marker flavours. */
enum hiscoa_eob {
	HISCOA_EOB_BAND = 0,	/* more bands follow */
	HISCOA_EOB_PAGE = 1	/* last band of the page */
};

/* Serialise params into the 8 byte payload of command 0xD0A4. */
void hiscoa_format_params(uint8_t out[8], const struct hiscoa_params *p);

/*
 * Compress one band.  Returns the number of bytes written, or
 * HISCOA_OVERFLOW if out_size was too small.  The result is always a
 * multiple of four bytes.
 */
#define HISCOA_OVERFLOW ((size_t) -1)

size_t hiscoa_compress_band(uint8_t *out, size_t out_size,
			    const uint8_t *band, unsigned line_size,
			    unsigned nlines, enum hiscoa_eob eob,
			    const struct hiscoa_params *p);

/*
 * Decompress one band.  Used by the test-suite and by the captdump
 * tool to verify that what the driver produces is what it means.
 * Returns 0 on success, -1 on a malformed stream.
 */
int hiscoa_decompress_band(const uint8_t *in, size_t in_size,
			   uint8_t *out, size_t out_size, size_t *out_len,
			   unsigned line_size, const struct hiscoa_params *p,
			   enum hiscoa_eob *eob, size_t *consumed);

#endif /* CAPT6000_HISCOA_H */
