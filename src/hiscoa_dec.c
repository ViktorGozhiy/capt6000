/*
 * hiscoa_dec.c -- Hi-SCoA decompressor.
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
 * The printer contains the only decoder that really matters, so this
 * one exists purely so the driver can check its own work: the test
 * suite round-trips every band and the captdump tool renders a job
 * back into an image.
 */

#include "hiscoa.h"

#include <string.h>

struct dec {
	const uint8_t *in;
	size_t in_size;
	size_t bit;
	int error;

	uint8_t *out;
	size_t out_size;
	size_t pos;

	unsigned line_size;
	unsigned nstash;
	uint8_t stash[16];
	unsigned origin[6];
};

static unsigned get_bits(struct dec *d, unsigned count)
{
	unsigned val = 0;

	while (count--) {
		size_t byte = d->bit / 8;
		unsigned shift;

		if (byte >= d->in_size) {
			d->error = 1;
			return 0;
		}
		shift = 7 - (unsigned) (d->bit % 8);
		val = (val << 1) | (((d->in[byte] ^ 0x43) >> shift) & 1);
		d->bit += 1;
	}
	return val;
}

/* Count 1-bits up to and including the terminating 0, at most max. */
static unsigned get_unary(struct dec *d, unsigned max)
{
	unsigned n = 0;

	while (n < max && get_bits(d, 1) == 1)
		++n;
	return n;
}

static unsigned get_length(struct dec *d)
{
	unsigned order = get_unary(d, 6);
	unsigned nbits, n;

	if (order == 6)
		return 0;
	if (order == 0)
		return get_bits(d, 1) == 0 ? 1 : (get_bits(d, 1) == 1 ? 2 : 3);

	nbits = order + 1;
	n = get_bits(d, nbits);
	return (1u << (order + 2)) - 1 - n;
}

static unsigned get_prefix(struct dec *d)
{
	unsigned order = get_bits(d, 2);
	unsigned n = order ? get_bits(d, order) : 0;

	return 128u * ((1u << (order + 1)) - 1 - n);
}

static void put_byte(struct dec *d, uint8_t byte)
{
	if (d->pos >= d->out_size) {
		d->error = 1;
		return;
	}
	d->out[d->pos++] = byte;
}

static void copy_from(struct dec *d, unsigned dist, unsigned count)
{
	if (dist == 0 || dist > d->pos) {
		d->error = 1;
		return;
	}
	while (count-- && !d->error)
		put_byte(d, d->out[d->pos - dist]);
}

static void stash_push(struct dec *d, uint8_t byte)
{
	if (d->nstash < 16)
		d->nstash += 1;
	memmove(d->stash + 1, d->stash, d->nstash - 1);
	d->stash[0] = byte;
}

static void swap_origin(struct dec *d, unsigned a, unsigned b)
{
	unsigned t = d->origin[a];
	d->origin[a] = d->origin[b];
	d->origin[b] = t;
}

int hiscoa_decompress_band(const uint8_t *in, size_t in_size,
			   uint8_t *out, size_t out_size, size_t *out_len,
			   unsigned line_size, const struct hiscoa_params *p,
			   enum hiscoa_eob *eob, size_t *consumed)
{
	struct dec d;
	unsigned pending_prefix = 0;
	int done = 0;

	if (line_size == 0)
		return -1;

	memset(&d, 0, sizeof(d));
	d.in = in;
	d.in_size = in_size;
	d.out = out;
	d.out_size = out_size;
	d.line_size = line_size;

	d.origin[0] = (unsigned) ((int) line_size + p->origin_0);
	d.origin[1] = 0;
	d.origin[2] = (unsigned) ((int) line_size + p->origin_2);
	d.origin[3] = (unsigned) p->origin_3;
	d.origin[4] = (unsigned) p->origin_4;
	d.origin[5] = (unsigned) p->origin_5;

	while (!done && !d.error) {
		unsigned code = get_unary(&d, 8);
		unsigned len;

		switch (code) {
		case 0:					/* LONGREP0 */
		case 3:					/* LONGREP3 */
		case 4:					/* LONGREP4 */
			len = pending_prefix + get_length(&d);
			pending_prefix = 0;
			copy_from(&d, d.origin[code], len);
			break;

		case 1:					/* REPBYTE */
			{
				unsigned v = get_bits(&d, 4);
				unsigned i = 15 - v;
				uint8_t byte;

				if (i >= d.nstash) {
					d.error = 1;
					break;
				}
				byte = d.stash[i];
				memmove(d.stash + 1, d.stash, i);
				d.stash[0] = byte;
				put_byte(&d, byte);
			}
			break;

		case 2:					/* BYTE or LONGREP2 */
			if (get_bits(&d, 1)) {
				uint8_t byte = (uint8_t) get_bits(&d, 8);
				stash_push(&d, byte);
				put_byte(&d, byte);
			} else {
				len = pending_prefix + get_length(&d);
				pending_prefix = 0;
				copy_from(&d, d.origin[2], len);
				swap_origin(&d, 2, 0);
			}
			break;

		case 5:					/* LONGREP5 */
			len = pending_prefix + get_length(&d);
			pending_prefix = 0;
			copy_from(&d, d.origin[5], len);
			swap_origin(&d, 5, 3);
			break;

		case 6:					/* ZEROBYTE or PREFIX */
			if (get_bits(&d, 1)) {
				stash_push(&d, 0);
				put_byte(&d, 0);
			} else {
				pending_prefix = get_prefix(&d);
			}
			break;

		case 7:					/* END */
			if (eob)
				*eob = (enum hiscoa_eob) get_bits(&d, 2);
			else
				(void) get_bits(&d, 2);
			done = 1;
			break;

		default:				/* 8 ones: padding */
			d.error = 1;
			break;
		}
	}

	if (d.error)
		return -1;
	if (out_len)
		*out_len = d.pos;
	if (consumed)
		*consumed = (d.bit + 31) / 32 * 4;
	return 0;
}
