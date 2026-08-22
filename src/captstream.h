/*
 * captstream.h -- the record stream that the filter hands to the backend.
 *
 * Part of capt6000, a CUPS driver for the Canon LBP6000/LBP6018.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Copyright (C) 2026  Viktor Hozhyi <https://github.com/ViktorGozhiy/capt6000>
 *
 * This program is free software under the terms of the GNU General
 * Public License, version 3 or any later version; see the file
 * LICENSE.  There is NO WARRANTY, to the extent permitted by law.
 *
 * CAPT is a bidirectional, flow-controlled protocol: the host may only
 * send the next page once the printer says it has room, and it has to
 * poll page counters to know when a sheet has actually come out.  That
 * conversation has to happen where the device is open, which in CUPS
 * means the backend.
 *
 * So the split is: the filter does all the CPU work (raster -> Hi-SCoA
 * bands, page parameters) and emits this simple record stream; the
 * backend does the conversation and owns the USB device.  The stream
 * is also a convenient thing to capture and inspect - see captdump.
 */

#ifndef CAPT6000_CAPTSTREAM_H
#define CAPT6000_CAPTSTREAM_H

#include <stdint.h>

#define CAPTSTREAM_MAGIC	"CAPT6000"	/* 8 bytes, no NUL */
#define CAPTSTREAM_VERSION	1u

/* Every record is a little endian 8 byte header plus payload. */
struct captstream_record {
	uint32_t type;
	uint32_t length;
	/* uint8_t data[length]; */
};

enum captstream_type {
	CAPTREC_JOB_BEGIN	= 1,	/* struct captstream_job */
	CAPTREC_PAGE_BEGIN	= 2,	/* struct captstream_page */
	CAPTREC_BAND		= 3,	/* Hi-SCoA compressed band */
	CAPTREC_PAGE_END	= 4,	/* empty */
	CAPTREC_JOB_END		= 5	/* empty */
};

struct captstream_job {
	uint32_t version;
	uint32_t npages;		/* 0 when not known in advance */
};

struct captstream_page {
	uint8_t page_params[40];	/* payload of command 0xD0A0 */
	uint8_t hiscoa_params[8];	/* payload of command 0xD0A4 */
	uint32_t line_size;		/* bytes per line, informational */
	uint32_t num_lines;		/* lines per page, informational */
	uint32_t band_lines;		/* lines per band, informational */
};

#endif /* CAPT6000_CAPTSTREAM_H */
