/*
 * test_filter.c -- checks where rastertolbp6000 puts a raster on the page.
 * Part of capt6000, a CUPS driver for the Canon LBP6000/LBP6018.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Copyright (C) 2026  Viktor Hozhyi <https://github.com/ViktorGozhiy/capt6000>
 *
 * This program is free software under the terms of the GNU General
 * Public License, version 3 or any later version; see the file
 * LICENSE.  There is NO WARRANTY, to the extent permitted by law.
 *
 * Each case writes a one page CUPS raster holding a black rectangle,
 * runs the built filter on it, decodes the result with captdump and
 * finds the rectangle on the decoded page.  The page headers are the
 * ones the CUPS rasterizers actually produce for an A4 page.
 *
 * Run from the top of the source tree, after "make".
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cups/raster.h>

#define WORKDIR		"build/test_filter.d"
#define FILTER		"build/rastertolbp6000"
#define CAPTDUMP	"build/captdump"

static unsigned failures;
static unsigned checks;

struct rect {
	long left, top, right, bottom;	/* right and bottom exclusive */
};

struct placement_case {
	const char *name;
	unsigned width, height;		/* raster, pixels */
	float imaging_bbox[4];		/* cupsImagingBBox, points */
	struct rect black;		/* black area, raster pixels */
	struct rect expected;		/* on the decoded page */
	long x_slack, y_slack;		/* allowed error, pixels */
};

static void set_bit(unsigned char *line, unsigned x)
{
	line[x / 8] |= (unsigned char) (0x80 >> (x % 8));
}

static int write_raster(const char *path, const struct placement_case *c)
{
	cups_page_header2_t h;
	cups_raster_t *r;
	unsigned char *line;
	unsigned y, x;
	int fd;

	memset(&h, 0, sizeof(h));
	strcpy(h.cupsPageSizeName, "A4");
	h.HWResolution[0] = h.HWResolution[1] = 600;
	h.PageSize[0] = 595;
	h.PageSize[1] = 842;
	h.cupsPageSize[0] = 595.28f;
	h.cupsPageSize[1] = 841.89f;
	memcpy(h.cupsImagingBBox, c->imaging_bbox, sizeof(h.cupsImagingBBox));
	for (x = 0; x < 4; ++x)
		h.ImagingBoundingBox[x] = (unsigned) c->imaging_bbox[x];
	h.cupsWidth = c->width;
	h.cupsHeight = c->height;
	h.cupsBitsPerColor = 1;
	h.cupsBitsPerPixel = 1;
	h.cupsBytesPerLine = (c->width + 7) / 8;
	h.cupsColorSpace = CUPS_CSPACE_K;
	h.cupsColorOrder = CUPS_ORDER_CHUNKED;
	h.cupsRowCount = 160;
	h.NumCopies = 1;

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return -1;
	r = cupsRasterOpen(fd, CUPS_RASTER_WRITE_COMPRESSED);
	line = calloc(1, h.cupsBytesPerLine);
	if (!r || !line || !cupsRasterWriteHeader2(r, &h)) {
		free(line);
		close(fd);
		return -1;
	}
	for (y = 0; y < c->height; ++y) {
		memset(line, 0, h.cupsBytesPerLine);
		if (y >= (unsigned) c->black.top &&
		    y < (unsigned) c->black.bottom)
			for (x = (unsigned) c->black.left;
			     x < (unsigned) c->black.right; ++x)
				set_bit(line, x);
		cupsRasterWritePixels(r, line, h.cupsBytesPerLine);
	}
	cupsRasterClose(r);
	free(line);
	close(fd);
	return 0;
}

/* Finds the bounding box of the black pixels in a P4 PBM file. */
static int black_area(const char *path, struct rect *out)
{
	FILE *f = fopen(path, "rb");
	unsigned w, h, bpl, x, y;
	unsigned char *line;

	if (!f || fscanf(f, "P4 %u %u", &w, &h) != 2 || fgetc(f) == EOF) {
		if (f)
			fclose(f);
		return -1;
	}
	bpl = (w + 7) / 8;
	line = malloc(bpl);
	out->left = out->top = -1;
	out->right = out->bottom = -1;
	for (y = 0; y < h; ++y) {
		if (fread(line, 1, bpl, f) != bpl)
			break;
		for (x = 0; x < w; ++x) {
			if (!(line[x / 8] & (0x80 >> (x % 8))))
				continue;
			if (out->top < 0)
				out->top = y;
			out->bottom = y + 1;
			if (out->left < 0 || (long) x < out->left)
				out->left = x;
			if ((long) x + 1 > out->right)
				out->right = x + 1;
		}
	}
	free(line);
	fclose(f);
	return 0;
}

static int near(long got, long want, long slack)
{
	return got >= want - slack && got <= want + slack;
}

static void run_case(const struct placement_case *c)
{
	char cmd[512];
	struct rect got;

	++checks;
	if (write_raster(WORKDIR "/page.ras", c) < 0) {
		printf("FAIL %-28s cannot write the test raster\n", c->name);
		++failures;
		return;
	}
	snprintf(cmd, sizeof(cmd),
		 "rm -f " WORKDIR "/page-001.pbm && "
		 FILTER " 1 test test 1 '' " WORKDIR "/page.ras"
		 " >" WORKDIR "/page.capt 2>" WORKDIR "/filter.log && "
		 CAPTDUMP " -o " WORKDIR " " WORKDIR "/page.capt"
		 " >" WORKDIR "/captdump.log");
	if (system(cmd) != 0 ||
	    black_area(WORKDIR "/page-001.pbm", &got) < 0) {
		printf("FAIL %-28s filter or captdump failed, see " WORKDIR
		       "\n", c->name);
		++failures;
		return;
	}
	if (near(got.left, c->expected.left, c->x_slack) &&
	    near(got.right, c->expected.right, c->x_slack) &&
	    near(got.top, c->expected.top, c->y_slack) &&
	    near(got.bottom, c->expected.bottom, c->y_slack)) {
		printf("ok   %-28s black at x %ld..%ld, y %ld..%ld\n", c->name,
		       got.left, got.right, got.top, got.bottom);
		return;
	}
	printf("FAIL %-28s black at x %ld..%ld, y %ld..%ld; "
	       "expected x %ld..%ld, y %ld..%ld\n", c->name,
	       got.left, got.right, got.top, got.bottom,
	       c->expected.left, c->expected.right,
	       c->expected.top, c->expected.bottom);
	++failures;
}

/*
 * The decoded page is the printer's imaging window: 4736 x 6776 pixels
 * for A4, whose top left corner lies 120 pixels (14.4 pt) in from the
 * top left corner of the sheet.  A raster pixel at page position
 * (x pt, y pt from the bottom) lands at window pixel
 * (x * 600/72 - 120, (841.89 - y) * 600/72 - 120).  Horizontal placement
 * is done in whole bytes, hence the 8 pixel slack.
 */
static const struct placement_case cases[] = {
	{
		/* Ghostscript: a raster covering exactly the imageable area. */
		"full imageable area",
		4736, 6776, { 14.40f, 14.40f, 582.66f, 827.49f },
		{ 0, 0, 4736, 6776 },
		{ 0, 0, 4736, 6776 }, 0, 0
	},
	{
		/* imagetoraster: an unscaled image, centered on the sheet. */
		"image centered on sheet",
		3000, 4800, { 117.64f, 132.94f, 477.64f, 708.94f },
		{ 0, 0, 3000, 4800 },
		{ 860, 988, 3860, 5788 }, 8, 1
	},
	{
		/* A raster covering the whole sheet, past the imaging area. */
		"raster larger than window",
		4961, 7016, { 0.0f, 0.0f, 595.28f, 841.89f },
		{ 400, 400, 1200, 1200 },
		{ 280, 280, 1080, 1080 }, 8, 1
	},
	{
		/* A rasterizer that does not say where the raster goes. */
		"no imaging bounding box",
		3000, 4800, { 0.0f, 0.0f, 0.0f, 0.0f },
		{ 0, 0, 3000, 4800 },
		{ 864, 0, 3864, 4800 }, 0, 0
	},
};

int main(void)
{
	size_t i;

	if (system("mkdir -p " WORKDIR) != 0) {
		printf("FAIL cannot create " WORKDIR "\n");
		return 1;
	}
	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
		run_case(&cases[i]);

	printf("%u of %u filter placement checks passed\n",
	       checks - failures, checks);
	return failures ? 1 : 0;
}
