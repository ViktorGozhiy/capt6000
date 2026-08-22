/*
 * pagegeom.c -- CAPT page geometry.
 * Part of capt6000, a CUPS driver for the Canon LBP6000/LBP6018.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Copyright (C) 2026  Viktor Hozhyi <https://github.com/ViktorGozhiy/capt6000>
 *
 * The geometry table reproduces measurements of Canon's own driver
 * published by the captdriver project:
 *   Copyright (C) 2020  Moses Chong
 *
 * This program is free software under the terms of the GNU General
 * Public License, version 3 or any later version; see the file
 * LICENSE.  There is NO WARRANTY, to the extent permitted by law.
 *
 * The numbers below are the ones Canon's own Windows driver puts into
 * the 0xD0A0 page prologue, as tabulated by the captdriver project.
 * They are reproduced verbatim rather than recomputed because the
 * relationship between the sheet size and the imaging area is not
 * quite a formula: standard document sizes get two extra raster lines
 * that envelopes and custom sizes do not.
 *
 * For everything else the observed rules are:
 *      line_size = round_up_4(ceil((paper_width  - 2 * bound_a) / 8))
 *      num_lines =            paper_height - 2 * bound_a  (+2 if standard)
 */

#include "pagegeom.h"

#include <stddef.h>
#include <string.h>

static const struct page_geometry geometries[] = {
	/* name          id     W     H    A    B  line  lines */
	{ "A4",        0x02, 4960, 7014, 120,  96,  592, 6776 },
	{ "Letter",    0x0D, 5100, 6600, 120,  96,  608, 6362 },
	{ "Legal",     0x0C, 5100, 8400, 120,  96,  608, 8162 },
	{ "Executive", 0x0A, 4350, 6300, 120,  96,  516, 6062 },
	{ "A5",        0x03, 3496, 4960, 120,  96,  408, 4722 },
	{ "B5",        0x07, 4298, 6070, 120,  96,  508, 5832 },
	{ "Env10",     0x16, 2478, 5700, 236, 236,  252, 5228 },
	{ "EnvDL",     0x18, 2598, 5196, 236, 236,  268, 4724 },
	{ "EnvC5",     0x15, 3826, 5408, 236, 236,  420, 4936 },
	{ "EnvMonarch",0x17, 2328, 4500, 236, 236,  232, 4028 },
	{ "3x5",       0x40, 1800, 3000, 236, 236,  168, 2528 },
};

#define CUSTOM_SIZE_ID	0x13
#define CUSTOM_BOUND_A	118
#define CUSTOM_BOUND_B	94

const struct page_geometry *page_geometry_find(const char *ppd_name)
{
	size_t i;

	if (!ppd_name)
		return NULL;
	for (i = 0; i < sizeof(geometries) / sizeof(geometries[0]); ++i) {
		if (strcmp(geometries[i].ppd_name, ppd_name) == 0)
			return &geometries[i];
	}
	return NULL;
}

static unsigned round_up_4(unsigned v)
{
	return (v + 3u) & ~3u;
}

void page_geometry_custom(struct page_geometry *out,
			  double width_pt, double height_pt)
{
	unsigned w = (unsigned) (width_pt * 600.0 / 72.0 + 0.5);
	unsigned h = (unsigned) (height_pt * 600.0 / 72.0 + 0.5);
	unsigned imaging_w;

	if (w < 2 * CUSTOM_BOUND_A + 8)
		w = 2 * CUSTOM_BOUND_A + 8;
	if (h < 2 * CUSTOM_BOUND_A + 1)
		h = 2 * CUSTOM_BOUND_A + 1;

	imaging_w = w - 2 * CUSTOM_BOUND_A;

	out->ppd_name = NULL;
	out->size_id = CUSTOM_SIZE_ID;
	out->paper_width = (uint16_t) w;
	out->paper_height = (uint16_t) h;
	out->bound_a = CUSTOM_BOUND_A;
	out->bound_b = CUSTOM_BOUND_B;
	out->line_size = (uint16_t) round_up_4((imaging_w + 7u) / 8u);
	out->num_lines = (uint16_t) (h - 2 * CUSTOM_BOUND_A);
}
