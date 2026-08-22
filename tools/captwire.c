/*
 * captwire.c -- decode a raw CAPT wire capture into readable commands.
 * Part of capt6000, a CUPS driver for the Canon LBP6000/LBP6018.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Copyright (C) 2026  Viktor Hozhyi <https://github.com/ViktorGozhiy/capt6000>
 *
 * This program is free software under the terms of the GNU General
 * Public License, version 3 or any later version; see the file
 * LICENSE.  There is NO WARRANTY, to the extent permitted by law.
 */

#include "../src/capt.h"
#include "../src/hiscoa.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *cmd_name(uint16_t c)
{
	switch (c) {
	case CAPT_NOP:			return "NOP";
	case CAPT_CHKJOBSTAT:		return "CHKJOBSTAT";
	case CAPT_CHKXSTATUS:		return "CHKXSTATUS";
	case CAPT_IEEE_IDENT:		return "IEEE_IDENT";
	case CAPT_IDENT:		return "IDENT";
	case CAPT_JOB_BEGIN:		return "JOB_BEGIN";
	case CAPT_START_0:		return "START_0";
	case CAPT_PRINT_DATA:		return "PRINT_DATA";
	case CAPT_PRINT_DATA_END:	return "PRINT_DATA_END";
	case CAPT_SET_PARM_PAGE:	return "SET_PARM_PAGE";
	case CAPT_SET_PARM_1:		return "SET_PARM_1";
	case CAPT_SET_PARM_2:		return "SET_PARM_2";
	case CAPT_SET_PARM_HISCOA:	return "SET_PARM_HISCOA";
	case CAPT_SET_PARMS:		return "SET_PARMS";
	case CAPT_CHKSTATUS:		return "CHKSTATUS";
	case CAPT_START_1:		return "START_1";
	case CAPT_START_2:		return "START_2";
	case CAPT_START_3:		return "START_3";
	case CAPT_UPLOAD_2:		return "UPLOAD_2";
	case CAPT_FIRE:			return "FIRE";
	case CAPT_JOB_END:		return "JOB_END";
	case CAPT_LBP6000_SETUP:	return "LBP6000_SETUP";
	case CAPT_JOB_SETUP:		return "JOB_SETUP";
	case CAPT_GPIO:			return "GPIO";
	default:			return "?";
	}
}

int main(int argc, char *argv[])
{
	FILE *f;
	uint8_t *d;
	long size;
	long pos = 0;
	unsigned long data_bytes = 0, data_packets = 0;
	int verbose = 0;
	const char *path = NULL;
	int i;

	for (i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "-v") == 0)
			verbose = 1;
		else
			path = argv[i];
	}
	if (!path) {
		fprintf(stderr, "usage: captwire [-v] capture\n");
		return 2;
	}
	f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno));
		return 1;
	}
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	d = malloc((size_t) size);
	if (fread(d, 1, (size_t) size, f) != (size_t) size) {
		fprintf(stderr, "short read\n");
		return 1;
	}
	fclose(f);

	printf("CAPT wire capture: %s (%ld bytes)\n\n", path, size);

	while (pos + 4 <= size) {
		uint16_t cmd = (uint16_t) (d[pos] | (d[pos + 1] << 8));
		uint16_t len = (uint16_t) (d[pos + 2] | (d[pos + 3] << 8));
		long body = pos + 4;

		if (len < 4 || pos + len > size) {
			printf("%08lx  MALFORMED packet %04X len %u\n",
			       pos, cmd, len);
			break;
		}

		if (cmd == CAPT_PRINT_DATA) {
			++data_packets;
			data_bytes += len - 4u;
			if (verbose)
				printf("%08lx  PRINT_DATA        %u bytes\n",
				       pos, len - 4);
			pos += len;
			continue;
		}

		printf("%08lx  %-17s %u bytes", pos, cmd_name(cmd), len - 4);
		if (cmd == CAPT_SET_PARMS) {
			long sub = body;
			printf("  {");
			while (sub + 4 <= pos + len) {
				uint16_t sc = (uint16_t) (d[sub] |
							  (d[sub + 1] << 8));
				uint16_t sl = (uint16_t) (d[sub + 2] |
							  (d[sub + 3] << 8));
				if (sl < 4)
					break;
				printf(" %s(%u)", cmd_name(sc), sl - 4);
				sub += sl;
			}
			printf(" }");
		} else if (len > 4 && len <= 24) {
			int k;
			printf("  ");
			for (k = 0; k < len - 4; ++k)
				printf("%02X ", d[body + k]);
		}
		printf("\n");
		pos += len;
	}

	printf("\n%lu PRINT_DATA packets, %lu bytes of page data\n",
	       data_packets, data_bytes);
	free(d);
	return 0;
}
