/*
 * hiscoa_enc.c -- Hi-SCoA compressor.
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
 * The stream is a sequence of bit-coded commands, MSB first, padded
 * with 1-bits to a 32 bit boundary and finally obfuscated by XORing
 * every byte with 0x43.
 *
 *   0        LONGREP0        copy from pos - origin[0]
 *   10       REPBYTE         re-emit a byte from the 16 entry stash
 *   1101     BYTE            literal byte follows
 *   1100     LONGREP2        copy from pos - origin[2], then swap o2/o0
 *   1110     LONGREP3        copy from pos - origin[3]
 *   11110    LONGREP4        copy from pos - origin[4]
 *   111110   LONGREP5        copy from pos - origin[5], then swap o5/o3
 *   11111101 ZEROBYTE        literal 0x00
 *   11111100 PREFIX          adds a multiple of 128 to the next length
 *   11111110 END             + 2 bit end-of-band type
 *   11111111 (padding)
 */

#include "hiscoa.h"

#include <string.h>

const struct hiscoa_params hiscoa_default_params = {
	.origin_0 = 0,
	.origin_2 = -7,
	.origin_3 = 1,
	.origin_4 = 0,
	.origin_5 = 4,
};

/* A match may not be longer than this: the length is coded as a
 * PREFIX (a multiple of 128, at most 512) plus a 7 bit remainder. */
#define MAX_MATCH	(512 + 127)

struct enc {
	const uint8_t *in;
	size_t in_size;
	size_t pos;

	uint8_t *out;
	size_t out_bits;	/* capacity, in bits */
	size_t bit;		/* write cursor, in bits */
	int overflow;

	unsigned line_size;

	unsigned nstash;
	uint8_t stash[16];

	unsigned origin[6];
};

static void put_bits(struct enc *e, uint32_t bits, unsigned count)
{
	if (e->bit + count > e->out_bits) {
		e->overflow = 1;
		return;
	}
	while (count) {
		unsigned byte = (unsigned) (e->bit / 8);
		unsigned used = (unsigned) (e->bit % 8);
		unsigned free_bits = 8 - used;
		unsigned n = count < free_bits ? count : free_bits;
		unsigned shift = free_bits - n;
		uint8_t val = (uint8_t) ((bits >> (count - n)) << shift);

		/* Bytes are filled strictly left to right, so the first
		 * touch of a byte initialises it; never read back. */
		if (used == 0)
			e->out[byte] = 0;
		e->out[byte] |= (uint8_t) (val & ((0xFFu >> (8 - n)) << shift));
		e->bit += n;
		count -= n;
	}
}

/* Number of significant bits in val (0 -> 0, 1 -> 1, 4 -> 3). */
static unsigned bit_width(unsigned val)
{
	unsigned n = 0;
	while (val) {
		++n;
		val >>= 1;
	}
	return n;
}

/* Length of the run at pos that repeats the run at pos - dist. */
static unsigned match_len(const struct enc *e, unsigned dist)
{
	size_t p = e->pos;
	size_t limit = e->pos + MAX_MATCH;

	if (dist == 0 || p < dist)
		return 0;
	if (limit > e->in_size)
		limit = e->in_size;

	while (p < limit && e->in[p] == e->in[p - dist]) {
		++p;
		/* Canon's compressor never lets a match cross a scan
		 * line; keep that property, decoders may rely on it. */
		if (p % e->line_size == 0)
			break;
	}
	return (unsigned) (p - e->pos);
}

/* Elias-style length code used by the LONGREP commands. */
static void put_length(struct enc *e, unsigned len)
{
	unsigned nbits;

	switch (len) {
	case 0:
		put_bits(e, 0x3F, 6);		/* 111111 */
		return;
	case 1:
		put_bits(e, 0x0, 2);		/* 0 0 */
		return;
	case 2:
		put_bits(e, 0x3, 3);		/* 0 11 */
		return;
	case 3:
		put_bits(e, 0x2, 3);		/* 0 10 */
		return;
	default:
		break;
	}
	nbits = bit_width(len) - 1;
	put_bits(e, 0xFFFFFFFEu, nbits);	/* (nbits-1) ones and a zero */
	put_bits(e, ~len, nbits);
}

static void swap_origin(struct enc *e, unsigned a, unsigned b)
{
	unsigned t = e->origin[a];
	e->origin[a] = e->origin[b];
	e->origin[b] = t;
}

static int emit_longrep(struct enc *e)
{
	unsigned best_cmd = 0;
	unsigned best_len = 0;
	unsigned cmd;
	unsigned len;

	for (cmd = 0; cmd < 6; ++cmd) {
		unsigned l = match_len(e, e->origin[cmd]);
		if (l > best_len) {
			best_len = l;
			best_cmd = cmd;
		}
	}
	if (best_len < 2)
		return 0;

	len = best_len;
	if (len > 127) {
		unsigned units = len / 128;
		unsigned nbits = bit_width(units) - 1;
		put_bits(e, 0xFC, 8);		/* PREFIX */
		put_bits(e, nbits, 2);
		put_bits(e, ~units, nbits);
		len %= 128;
	}

	put_bits(e, 0xFFFFFFFEu, best_cmd + 1);
	if (best_cmd == 2)
		put_bits(e, 0x0, 1);		/* 1100, not 1101 (=BYTE) */
	put_length(e, len);

	if (best_cmd == 2)
		swap_origin(e, 2, 0);
	else if (best_cmd == 5)
		swap_origin(e, 5, 3);

	e->pos += best_len;
	return 1;
}

static int emit_repbyte(struct enc *e)
{
	uint8_t byte = e->in[e->pos];
	unsigned i;

	for (i = 0; i < e->nstash; ++i) {
		if (e->stash[i] != byte)
			continue;
		memmove(e->stash + 1, e->stash, i);
		e->stash[0] = byte;
		put_bits(e, 0x20 | ((15 - i) & 0xF), 6);
		e->pos += 1;
		return 1;
	}
	return 0;
}

static void emit_literal(struct enc *e)
{
	uint8_t byte = e->in[e->pos];

	if (e->nstash < 16)
		e->nstash += 1;
	memmove(e->stash + 1, e->stash, e->nstash - 1);
	e->stash[0] = byte;

	if (byte == 0)
		put_bits(e, 0xFD, 8);		/* ZEROBYTE */
	else
		put_bits(e, 0xD00 | byte, 12);	/* BYTE + literal */
	e->pos += 1;
}

void hiscoa_format_params(uint8_t out[8], const struct hiscoa_params *p)
{
	uint16_t o4 = (uint16_t) (int16_t) p->origin_4;

	out[0] = (uint8_t) (int8_t) p->origin_3;
	out[1] = (uint8_t) (int8_t) p->origin_5;
	out[2] = 0x01;
	out[3] = 0x01;			/* bits per pixel selector */
	out[4] = (uint8_t) (int8_t) p->origin_0;
	out[5] = (uint8_t) (int8_t) p->origin_2;
	out[6] = (uint8_t) (o4 & 0xFF);
	out[7] = (uint8_t) (o4 >> 8);
}

size_t hiscoa_compress_band(uint8_t *out, size_t out_size,
			    const uint8_t *band, unsigned line_size,
			    unsigned nlines, enum hiscoa_eob eob,
			    const struct hiscoa_params *p)
{
	struct enc e;
	size_t i;
	unsigned pad;

	if (line_size == 0 || nlines == 0)
		return HISCOA_OVERFLOW;

	memset(&e, 0, sizeof(e));
	e.in = band;
	e.in_size = (size_t) line_size * nlines;
	e.out = out;
	e.out_bits = out_size * 8;
	e.line_size = line_size;

	e.origin[0] = (unsigned) ((int) line_size + p->origin_0);
	e.origin[1] = 0;		/* opcode 10 is REPBYTE, never a copy */
	e.origin[2] = (unsigned) ((int) line_size + p->origin_2);
	e.origin[3] = (unsigned) p->origin_3;
	e.origin[4] = (unsigned) p->origin_4;
	e.origin[5] = (unsigned) p->origin_5;

	while (e.pos < e.in_size && !e.overflow) {
		if (emit_longrep(&e))
			continue;
		if (emit_repbyte(&e))
			continue;
		emit_literal(&e);
	}

	put_bits(&e, 0xFE, 8);			/* END */
	put_bits(&e, (uint32_t) eob, 2);

	pad = (unsigned) (e.bit % 32);
	if (pad)
		put_bits(&e, 0xFFFFFFFFu, 32 - pad);

	if (e.overflow)
		return HISCOA_OVERFLOW;

	for (i = 0; i < e.bit / 8; ++i)
		out[i] ^= 0x43;

	return e.bit / 8;
}
