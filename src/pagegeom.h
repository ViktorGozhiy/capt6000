/*
 * pagegeom.h -- CAPT page geometry.
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
 */

#ifndef CAPT6000_PAGEGEOM_H
#define CAPT6000_PAGEGEOM_H

#include <stdint.h>

struct page_geometry {
	const char *ppd_name;	/* CUPS PageSize keyword, NULL for custom */
	uint8_t size_id;	/* byte 4 of the 0xD0A0 payload */
	uint16_t paper_width;	/* physical sheet, pixels at 600 dpi */
	uint16_t paper_height;
	uint16_t bound_a;	/* imaging inset, pixels */
	uint16_t bound_b;	/* second imaging parameter, pixels */
	uint16_t line_size;	/* bytes per raster line */
	uint16_t num_lines;	/* raster lines per page */
};

/*
 * Look a page size up by its PPD keyword.  Returns NULL if the size is
 * not one of the ones Canon's own driver has a table entry for; use
 * page_geometry_custom() in that case.
 */
const struct page_geometry *page_geometry_find(const char *ppd_name);

/*
 * Derive the geometry of an arbitrary sheet from its size in points.
 * Follows the rules Canon's driver uses for custom sizes.
 */
void page_geometry_custom(struct page_geometry *out,
			  double width_pt, double height_pt);

#endif /* CAPT6000_PAGEGEOM_H */
