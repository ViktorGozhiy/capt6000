/*
 * captstat.c -- query a CAPT printer's status without printing anything.
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void show(const char *label, int value)
{
	printf("  %-22s %s\n", label, value ? "yes" : "no");
}

int main(int argc, char *argv[])
{
	struct capt_dev *dev;
	struct capt_status st;
	char err[256] = "";
	char model[128] = "";
	int i;
	int watch = 0;
	int reset = 0;

	for (i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "-t") == 0)
			capt_set_trace(1);
		else if (strcmp(argv[i], "-w") == 0)
			watch = 1;
		else if (strcmp(argv[i], "-r") == 0)
			reset = 1;
	}

	if (reset) {
		dev = capt_usb_open(NULL, err, sizeof(err));
		if (!dev) {
			fprintf(stderr, "captstat: %s\n", err);
			return 1;
		}
		capt_usb_reset(dev);
		capt_close(dev);
		sleep(3);
		printf("device reset\n");
	}

	dev = capt_usb_open(NULL, err, sizeof(err));
	if (!dev) {
		fprintf(stderr, "captstat: %s\n", err);
		return 1;
	}

	printf("device id : %s\n", capt_device_id(dev));
	if (capt_device_model(dev, model, sizeof(model)) == 0)
		printf("model     : %s\n", model);

	do {
		if (capt_get_xstatus(dev, &st) < 0) {
			capt_close(dev);
			return 1;
		}
		printf("\nstatus words: %04X %04X %04X %04X %04X %04X %04X\n",
		       st.word[0], st.word[1], st.word[2], st.word[3],
		       st.word[4], st.word[5], st.word[6]);
		show("busy", capt_flag(&st, CAPT_FL_BUSY));
		show("engine uninitialised",
		     capt_flag(&st, CAPT_FL_UNINIT1) ||
		     capt_flag(&st, CAPT_FL_UNINIT2));
		show("buffer full", capt_flag(&st, CAPT_FL_BUFFER_FULL));
		show("processing a job", capt_flag(&st, CAPT_FL_PROCESSING));
		show("printing", capt_flag(&st, CAPT_FL_PRINTING));
		show("powering up", capt_flag(&st, CAPT_FL_POWERING_UP) ||
				    capt_flag(&st, CAPT_FL_POWERING_UP2));
		show("out of paper", capt_flag(&st, CAPT_FL_NO_PAPER1) ||
				     capt_flag(&st, CAPT_FL_NO_PAPER2));
		show("engine ok (no error)", capt_flag(&st, CAPT_FL_NO_ERROR));
		show("button active", capt_flag(&st, CAPT_FL_BUTTON_ACTIVE));
		show("button pressed", capt_flag(&st, CAPT_FL_BUTTON));
		printf("  pages decoding/printing/out/completed/received:"
		       " %u/%u/%u/%u/%u\n",
		       st.page_decoding, st.page_printing, st.page_out,
		       st.page_completed, st.page_received);
		if (watch)
			sleep(1);
	} while (watch);

	capt_close(dev);
	return 0;
}
