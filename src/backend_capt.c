/*
 * backend_capt.c -- CUPS backend for Canon CAPT printers over USB.
 *
 * Part of capt6000, a CUPS driver for the Canon LBP6000/LBP6018.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Copyright (C) 2026  Viktor Hozhyi <https://github.com/ViktorGozhiy/capt6000>
 *
 * The job and page command sequence, the magic payloads and the layout
 * of command 0xE1A1 come from the captdriver project:
 *   Copyright (C) 2013  Alexey Galakhov <agalakhov@gmail.com>
 *   Copyright (C) 2016  Alexei Gordeev <KP1533TM2@gmail.com>
 *
 * This program is free software under the terms of the GNU General
 * Public License, version 3 or any later version; see the file
 * LICENSE.  There is NO WARRANTY, to the extent permitted by law.
 *
 * Consumes the record stream produced by rastertolbp6000 and carries
 * out the CAPT conversation with the printer.  Also does device
 * discovery when CUPS runs it with no arguments.
 *
 * Device URI:  capt://Canon/LBP6000?serial=XXXXXXXX
 */

#include "capt.h"
#include "captstream.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* CUPS backend exit codes. */
#define BACKEND_OK		0
#define BACKEND_FAILED		1
#define BACKEND_STOP		4
#define BACKEND_CANCEL		5
#define BACKEND_RETRY		6

#define STATUS_POLL_US		250000
#define PAGE_TIMEOUT_S		300
#define OPERATOR_TIMEOUT_S	3600

static volatile sig_atomic_t cancel_requested;
static struct capt_dev *device;
static uint16_t job_id;
static int job_open;
static uint16_t last_page_done;
/* Set once the printer stops answering: from then on we must not push
 * anything else at it.  Talking to a wedged CAPT engine is what turns a
 * recoverable error into one that needs the power switch. */
static int device_dead;

static void on_cancel(int sig)
{
	(void) sig;
	cancel_requested = 1;
}

/* ------------------------------------------------------------------ */
/* Discovery                                                           */
/* ------------------------------------------------------------------ */

static void uri_escape(const char *in, char *out, size_t outlen)
{
	static const char safe[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
				   "abcdefghijklmnopqrstuvwxyz"
				   "0123456789-._~";
	size_t o = 0;

	for (; *in && o + 4 < outlen; ++in) {
		if (strchr(safe, *in)) {
			out[o++] = *in;
		} else {
			snprintf(out + o, outlen - o, "%%%02X",
				 (unsigned char) *in);
			o += 3;
		}
	}
	out[o] = '\0';
}

static void report_device(const char *make, const char *model,
			  const char *serial, const char *device_id,
			  void *user)
{
	char emake[256], emodel[256], eserial[256];

	(void) user;
	uri_escape(make, emake, sizeof(emake));
	uri_escape(model, emodel, sizeof(emodel));
	uri_escape(serial, eserial, sizeof(eserial));

	printf("direct capt://%s/%s?serial=%s \"%s %s\" \"%s %s USB\" \"%s\"\n",
	       emake, emodel, eserial, make, model, make, model, device_id);
}

static int list_devices(void)
{
	if (capt_usb_enumerate(report_device, NULL) < 0) {
		fprintf(stderr, "DEBUG: capt6000: USB enumeration failed\n");
		return BACKEND_OK;	/* discovery must never fail hard */
	}
	return BACKEND_OK;
}

/* ------------------------------------------------------------------ */
/* Record stream reader                                                */
/* ------------------------------------------------------------------ */

struct reader {
	int fd;
	uint8_t *buf;
	size_t cap;
};

static int read_exact(int fd, void *buf, size_t size)
{
	uint8_t *p = buf;

	while (size) {
		ssize_t n = read(fd, p, size);
		if (n == 0)
			return 1;		/* clean EOF */
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		p += n;
		size -= (size_t) n;
	}
	return 0;
}

static uint32_t get_le32(const uint8_t *p)
{
	return (uint32_t) p[0] | ((uint32_t) p[1] << 8) |
	       ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

/* Returns 1 on end of stream, 0 on success, -1 on error. */
static int reader_next(struct reader *r, uint32_t *type, uint32_t *length)
{
	uint8_t hdr[8];
	int rc = read_exact(r->fd, hdr, sizeof(hdr));

	if (rc)
		return rc;
	*type = get_le32(hdr);
	*length = get_le32(hdr + 4);
	if (*length > 32u * 1024 * 1024) {
		fprintf(stderr, "ERROR: capt6000: absurd record length %u\n",
			*length);
		return -1;
	}
	if (*length > r->cap) {
		size_t cap = *length + 4096;
		uint8_t *nb = realloc(r->buf, cap);
		if (!nb) {
			fprintf(stderr, "ERROR: capt6000: out of memory\n");
			return -1;
		}
		r->buf = nb;
		r->cap = cap;
	}
	if (*length && read_exact(r->fd, r->buf, *length) != 0) {
		fprintf(stderr, "ERROR: capt6000: truncated record\n");
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* CAPT job sequencing                                                 */
/* ------------------------------------------------------------------ */

static const uint8_t magic_job_begin[] = {
	0x00, 0x00, 0x1E, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const uint8_t magic_upload[] = {
	0xEE, 0xDB, 0xEA, 0xAD, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* CAPT 3 GPIO block; the first byte drives the status LED. */
static const uint8_t gpio_idle[] = {
	0x13, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const uint8_t gpio_blink[] = {
	0x31, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const uint8_t lbp6000_setup[] = { 0x01, 0x00 };

enum job_phase {
	JOB_PHASE_START		= 1,
	JOB_PHASE_PAGE_SENT	= 2,
	JOB_PHASE_FINISH	= 4,
	JOB_PHASE_PAGE_FIRED	= 6
};

/* Command 0xE1A1: tell the printer where we are in the job. */
static int send_job_phase(struct capt_dev *dev, enum job_phase phase,
			  uint16_t page)
{
	uint8_t buf[72];
	time_t now = time(NULL);
	struct tm tm;

	localtime_r(&now, &tm);
	memset(buf, 0, sizeof(buf));

	buf[4] = (uint8_t) (page & 0xFF);
	buf[5] = (uint8_t) (page >> 8);
	/* host, user and document name lengths: we send none */
	buf[16] = (uint8_t) phase;
	buf[17] = 0x01;
	buf[18] = (uint8_t) (job_id & 0xFF);
	buf[19] = (uint8_t) (job_id >> 8);
	buf[20] = 0xC4;			/* -60,  minutes west of UTC */
	buf[21] = 0xFF;
	buf[22] = 0x88;			/* -120, ditto with DST */
	buf[23] = 0xFF;
	buf[24] = (uint8_t) (tm.tm_year & 0xFF);
	buf[25] = (uint8_t) ((tm.tm_year >> 8) & 0xFF);
	buf[26] = (uint8_t) tm.tm_mon;
	buf[27] = (uint8_t) tm.tm_mday;
	buf[28] = (uint8_t) tm.tm_hour;
	buf[29] = (uint8_t) tm.tm_min;
	buf[30] = (uint8_t) tm.tm_sec;
	buf[31] = 0x01;

	return capt_sendrecv(dev, CAPT_JOB_SETUP, buf, sizeof(buf), NULL, NULL);
}

static int out_of_paper(const struct capt_status *s)
{
	return capt_flag(s, CAPT_FL_NO_PAPER1) ||
	       capt_flag(s, CAPT_FL_NO_PAPER2);
}

static void report_problems(const struct capt_status *s)
{
	/* -1 means "not reported yet": the first status of a job always
	 * publishes the condition, so a stale media-empty-error left on
	 * the queue by an earlier job gets cleared. */
	static int paper_out = -1;
	int now = out_of_paper(s);

	if (now && paper_out != 1) {
		fputs("STATE: +media-empty-error\n", stderr);
		fputs("ERROR: capt6000: the printer is out of paper\n", stderr);
	} else if (!now && paper_out != 0) {
		fputs("STATE: -media-empty-error\n", stderr);
	}
	paper_out = now;
}

static int job_prologue(struct capt_dev *dev)
{
	struct capt_status st;
	uint8_t reply[64];
	size_t size = sizeof(reply);

	if (capt_sendrecv(dev, CAPT_IDENT, NULL, 0, NULL, NULL) < 0)
		return -1;
	usleep(500000);
	if (capt_get_xstatus(dev, &st) < 0)
		return -1;
	report_problems(&st);

	if (capt_sendrecv(dev, CAPT_START_0, NULL, 0, NULL, NULL) < 0)
		return -1;
	if (capt_sendrecv(dev, CAPT_JOB_BEGIN, magic_job_begin,
			  sizeof(magic_job_begin), reply, &size) < 0)
		return -1;
	job_id = size >= 4 ? (uint16_t) (reply[2] | (reply[3] << 8)) : 0;
	job_open = 1;
	fprintf(stderr, "DEBUG: capt6000: printer assigned job id %u\n",
		job_id);

	if (capt_sendrecv(dev, CAPT_GPIO, gpio_idle, sizeof(gpio_idle),
			  NULL, NULL) < 0)
		return -1;
	if (capt_wait_ready(dev, NULL) < 0)
		return -1;

	/* CAPT 3 devices in the LBP6000 family need this before the
	 * job setup is accepted. */
	if (capt_sendrecv(dev, CAPT_LBP6000_SETUP, lbp6000_setup,
			  sizeof(lbp6000_setup), NULL, NULL) < 0)
		return -1;
	if (capt_wait_ready(dev, NULL) < 0)
		return -1;

	if (send_job_phase(dev, JOB_PHASE_START, 0) < 0)
		return -1;
	return capt_wait_ready(dev, NULL);
}

/*
 * The engine wants attention - almost always a sheet that did not get
 * picked up.  Blink the panel light, tell CUPS, and wait for the
 * condition to clear.  CAPT signals recovery by raising the no-error
 * bit again, which is what the operator loading paper (or pressing the
 * button) produces.  Refilling a tray happens on human time, so this
 * deliberately has a very long fuse; the job is not lost meanwhile.
 */
static int wait_for_operator(struct capt_dev *dev, struct capt_status *st)
{
	unsigned waits = 0;
	int blinked = 0;

	for (;;) {
		if (capt_get_xstatus(dev, st) < 0) {
			device_dead = 1;
			return -1;
		}
		report_problems(st);

		if (!out_of_paper(st) && capt_flag(st, CAPT_FL_NO_ERROR)) {
			if (blinked) {
				capt_sendrecv(dev, CAPT_GPIO, gpio_idle,
					      sizeof(gpio_idle), NULL, NULL);
				fprintf(stderr, "DEBUG: capt6000: the printer "
						"is happy again, resuming\n");
			}
			return 0;
		}
		if (cancel_requested)
			return -1;
		if (!blinked) {
			fprintf(stderr, "DEBUG: capt6000: waiting for the "
					"operator\n");
			capt_sendrecv(dev, CAPT_GPIO, gpio_blink,
				      sizeof(gpio_blink), NULL, NULL);
			blinked = 1;
		}
		if (++waits > 4 * OPERATOR_TIMEOUT_S) {
			fputs("ERROR: capt6000: the printer still needs "
			      "attention after an hour; giving up on this "
			      "job\n", stderr);
			return -1;
		}
		usleep(STATUS_POLL_US);
	}
}

static int printer_wake(struct capt_dev *dev, struct capt_status *st)
{
	if (!capt_flag(st, CAPT_FL_UNINIT1) && !capt_flag(st, CAPT_FL_UNINIT2))
		return 0;

	fprintf(stderr, "DEBUG: capt6000: engine not initialised, waking it\n");
	if (capt_sendrecv(dev, CAPT_START_1, NULL, 0, NULL, NULL) < 0 ||
	    capt_sendrecv(dev, CAPT_START_2, NULL, 0, NULL, NULL) < 0 ||
	    capt_sendrecv(dev, CAPT_START_3, NULL, 0, NULL, NULL) < 0)
		return -1;
	if (capt_wait_ready(dev, st) < 0)
		return -1;
	if (capt_sendrecv(dev, CAPT_UPLOAD_2, magic_upload,
			  sizeof(magic_upload), NULL, NULL) < 0)
		return -1;
	return capt_wait_ready(dev, st);
}

static int page_prologue(struct capt_dev *dev,
			 const struct captstream_page *page)
{
	struct capt_multi multi;
	struct capt_status st;
	unsigned waits = 0;

	if (capt_get_xstatus(dev, &st) < 0) {
		device_dead = 1;
		return -1;
	}
	if (printer_wake(dev, &st) < 0) {
		device_dead = 1;
		return -1;
	}

	if (out_of_paper(&st) || !capt_flag(&st, CAPT_FL_NO_ERROR)) {
		if (wait_for_operator(dev, &st) < 0)
			return -1;
	}

	while (capt_flag(&st, CAPT_FL_BUFFER_FULL)) {
		if (cancel_requested)
			return -1;
		if (++waits > 4 * 120) {
			fprintf(stderr, "ERROR: capt6000: printer buffer "
					"stayed full for two minutes\n");
			return -1;
		}
		usleep(STATUS_POLL_US);
		if (capt_get_xstatus(dev, &st) < 0)
			return -1;
		report_problems(&st);
	}

	capt_multi_begin(&multi, CAPT_SET_PARMS);
	if (capt_multi_add(&multi, CAPT_SET_PARM_PAGE, page->page_params,
			   sizeof(page->page_params)) < 0 ||
	    capt_multi_add(&multi, CAPT_SET_PARM_HISCOA, page->hiscoa_params,
			   sizeof(page->hiscoa_params)) < 0 ||
	    capt_multi_add(&multi, CAPT_SET_PARM_1, NULL, 0) < 0 ||
	    capt_multi_add(&multi, CAPT_SET_PARM_2, NULL, 0) < 0)
		return -1;
	return capt_multi_send(dev, &multi);
}

static int send_band(struct capt_dev *dev, const uint8_t *data, size_t size,
		     unsigned *packets)
{
	while (size) {
		size_t chunk = size > CAPT_MAX_DATA_CHUNK
			       ? CAPT_MAX_DATA_CHUNK : size;

		/* Give the printer a chance to drain now and then; it
		 * has far less memory than a page of data. */
		if (++*packets % 16 == 0 && capt_wait_ready(dev, NULL) < 0)
			return -1;
		if (capt_send(dev, CAPT_PRINT_DATA, data, chunk) < 0)
			return -1;
		data += chunk;
		size -= chunk;
	}
	return 0;
}

static int page_epilogue(struct capt_dev *dev, unsigned page_no)
{
	struct capt_status st;
	unsigned waits;

	if (capt_send(dev, CAPT_PRINT_DATA_END, NULL, 0) < 0)
		return -1;

	/* Wait for the printer to acknowledge the whole page. */
	for (waits = 0;; ++waits) {
		if (capt_get_xstatus(dev, &st) < 0) {
			device_dead = 1;
			return -1;
		}
		report_problems(&st);
		if (st.page_received == st.page_decoding)
			break;
		if (cancel_requested)
			return -1;
		if (out_of_paper(&st)) {
			if (wait_for_operator(dev, &st) < 0)
				return -1;
			waits = 0;
		}
		if (waits > 4 * PAGE_TIMEOUT_S) {
			fprintf(stderr, "ERROR: capt6000: printer never "
					"finished receiving page %u\n", page_no);
			return -1;
		}
		usleep(STATUS_POLL_US);
	}

	if (send_job_phase(dev, JOB_PHASE_PAGE_SENT, st.page_decoding) < 0)
		return -1;
	if (capt_wait_ready(dev, NULL) < 0)
		return -1;

	{
		uint8_t pg[2] = {
			(uint8_t) (st.page_decoding & 0xFF),
			(uint8_t) (st.page_decoding >> 8)
		};
		if (capt_sendrecv(dev, CAPT_FIRE, pg, sizeof(pg),
				  NULL, NULL) < 0)
			return -1;
	}
	if (capt_wait_ready(dev, NULL) < 0)
		return -1;
	if (send_job_phase(dev, JOB_PHASE_PAGE_FIRED, st.page_decoding) < 0)
		return -1;

	/* And now wait for the sheet to physically come out. */
	for (waits = 0;; ++waits) {
		struct capt_status now;

		if (capt_get_xstatus(dev, &now) < 0) {
			device_dead = 1;
			return -1;
		}
		report_problems(&now);
		if (now.page_out == st.page_decoding) {
			last_page_done = now.page_out;
			fprintf(stderr, "DEBUG: capt6000: page %u delivered\n",
				page_no);
			return 0;
		}
		if (cancel_requested)
			return -1;
		/* The engine only raises these once it has actually
		 * tried and failed to pull a sheet, so do not react
		 * while it is still busy with the previous one. */
		if (!capt_flag(&now, CAPT_FL_PRINTING) &&
		    !capt_flag(&now, CAPT_FL_PROCESSING_DATA) &&
		    (out_of_paper(&now) || !capt_flag(&now, CAPT_FL_NO_ERROR))) {
			if (wait_for_operator(dev, &now) < 0)
				return -1;
			/* The page is still in the printer's memory and
			 * comes out by itself; just keep watching. */
			waits = 0;
			continue;
		}
		if (waits > 4 * PAGE_TIMEOUT_S) {
			fprintf(stderr, "ERROR: capt6000: page %u never came "
					"out of the printer\n", page_no);
			return -1;
		}
		usleep(STATUS_POLL_US);
	}
}

static void job_epilogue(struct capt_dev *dev)
{
	struct capt_status st;
	unsigned waits;
	uint8_t jb[2];

	if (!job_open)
		return;
	job_open = 0;

	if (device_dead) {
		fputs("ERROR: capt6000: the printer stopped responding "
		      "mid-job.  Switch it off, wait a few seconds and "
		      "switch it back on.\n", stderr);
		return;
	}

	for (waits = 0; waits < 4 * PAGE_TIMEOUT_S; ++waits) {
		if (capt_get_xstatus(dev, &st) < 0)
			break;
		if (st.page_completed == st.page_decoding)
			break;
		usleep(STATUS_POLL_US);
	}

	capt_sendrecv(dev, CAPT_GPIO, gpio_idle, sizeof(gpio_idle), NULL, NULL);
	send_job_phase(dev, JOB_PHASE_FINISH, last_page_done);

	jb[0] = (uint8_t) (job_id & 0xFF);
	jb[1] = (uint8_t) (job_id >> 8);
	capt_sendrecv(dev, CAPT_JOB_END, jb, sizeof(jb), NULL, NULL);
	fprintf(stderr, "DEBUG: capt6000: job %u closed\n", job_id);
}

/* ------------------------------------------------------------------ */

static int run_job(struct capt_dev *dev, int fd)
{
	struct reader r;
	struct captstream_page page;
	uint8_t magic[8];
	unsigned page_no = 0;
	unsigned packets = 0;
	int in_page = 0;
	int started = 0;
	int rc = BACKEND_OK;

	memset(&r, 0, sizeof(r));
	r.fd = fd;

	if (read_exact(fd, magic, sizeof(magic)) != 0 ||
	    memcmp(magic, CAPTSTREAM_MAGIC, 8) != 0) {
		fprintf(stderr, "ERROR: capt6000: the data sent to this "
				"backend is not a capt6000 stream; check that "
				"the queue uses the LBP6000 PPD\n");
		return BACKEND_STOP;
	}

	for (;;) {
		uint32_t type, length;
		int n = reader_next(&r, &type, &length);

		if (n < 0) {
			rc = BACKEND_FAILED;
			break;
		}
		if (n > 0)
			break;
		if (cancel_requested) {
			rc = BACKEND_CANCEL;
			break;
		}

		switch (type) {
		case CAPTREC_JOB_BEGIN:
			break;

		case CAPTREC_PAGE_BEGIN:
			if (length < sizeof(page)) {
				fprintf(stderr, "ERROR: capt6000: short page "
						"record\n");
				rc = BACKEND_FAILED;
				goto done;
			}
			memcpy(&page, r.buf, sizeof(page));
			if (!started) {
				fputs("STATE: -connecting-to-device\n", stderr);
				if (job_prologue(dev) < 0) {
					rc = BACKEND_FAILED;
					goto done;
				}
				started = 1;
			}
			++page_no;
			fprintf(stderr, "DEBUG: capt6000: starting page %u\n",
				page_no);
			if (page_prologue(dev, &page) < 0) {
				rc = BACKEND_FAILED;
				goto done;
			}
			packets = 0;
			in_page = 1;
			break;

		case CAPTREC_BAND:
			if (!in_page) {
				fprintf(stderr, "ERROR: capt6000: band outside "
						"of a page\n");
				rc = BACKEND_FAILED;
				goto done;
			}
			if (send_band(dev, r.buf, length, &packets) < 0) {
				rc = BACKEND_FAILED;
				goto done;
			}
			break;

		case CAPTREC_PAGE_END:
			if (!in_page)
				break;
			in_page = 0;
			if (page_epilogue(dev, page_no) < 0) {
				rc = BACKEND_FAILED;
				goto done;
			}
			fprintf(stderr, "PAGE: %u 1\n", page_no);
			break;

		case CAPTREC_JOB_END:
			goto done;

		default:
			fprintf(stderr, "DEBUG: capt6000: ignoring unknown "
					"record type %u\n", type);
			break;
		}
	}

done:
	if (started)
		job_epilogue(dev);
	free(r.buf);

	/* A dead printer will not be revived by another attempt, so ask
	 * CUPS to hold the queue instead of retrying the job at it. */
	if (device_dead)
		rc = BACKEND_STOP;

	if (rc == BACKEND_OK && page_no == 0)
		fprintf(stderr, "DEBUG: capt6000: nothing to print\n");
	return rc;
}

int main(int argc, char *argv[])
{
	const char *uri;
	const char *serial = NULL;
	char serial_buf[128] = "";
	char err[256] = "";
	char model[128] = "";
	int fd = STDIN_FILENO;
	int rc;

	setbuf(stderr, NULL);

	if (argc == 1)
		return list_devices();

	if (argc < 6 || argc > 7) {
		fprintf(stderr, "Usage: %s job-id user title copies options "
				"[file]\n", argv[0]);
		return BACKEND_FAILED;
	}

	signal(SIGPIPE, SIG_IGN);
	signal(SIGTERM, on_cancel);
	signal(SIGINT, on_cancel);

	if (getenv("CAPT_TRACE"))
		capt_set_trace(1);
	if (getenv("CAPT_TRACE") || getenv("CAPT_STATUS_TRACE"))
		capt_set_status_trace(1);

	uri = getenv("DEVICE_URI");
	if (uri) {
		const char *q = strstr(uri, "serial=");
		if (q) {
			size_t i = 0;
			q += 7;
			while (*q && *q != '&' && i + 1 < sizeof(serial_buf))
				serial_buf[i++] = *q++;
			serial_buf[i] = '\0';
			if (serial_buf[0])
				serial = serial_buf;
		}
	}

	if (argc == 7) {
		fd = open(argv[6], O_RDONLY);
		if (fd < 0) {
			fprintf(stderr, "ERROR: capt6000: cannot open %s: %s\n",
				argv[6], strerror(errno));
			return BACKEND_FAILED;
		}
	}

	fputs("STATE: +connecting-to-device\n", stderr);
	device = capt_usb_open(serial, err, sizeof(err));
	if (!device) {
		fprintf(stderr, "INFO: capt6000: %s; will retry\n", err);
		return BACKEND_RETRY;
	}

	if (capt_device_model(device, model, sizeof(model)) == 0)
		fprintf(stderr, "DEBUG: capt6000: connected to '%s'\n", model);
	fprintf(stderr, "DEBUG: capt6000: device id: %s\n",
		capt_device_id(device));

	/*
	 * The printer enumerates happily even when its firmware has
	 * stopped servicing the bulk pipes - which is what happens if
	 * something once fed it data it could not parse.  Check that
	 * it still answers CAPT before pushing a job at it, and give
	 * it one port reset to recover on its own.
	 */
	{
		struct capt_status probe;

		if (capt_get_xstatus(device, &probe) < 0) {
			fprintf(stderr, "INFO: capt6000: no answer to the "
					"status command, resetting the "
					"printer\n");
			capt_usb_reset(device);
			capt_close(device);
			sleep(3);
			device = capt_usb_open(serial, err, sizeof(err));
			if (!device) {
				fprintf(stderr, "INFO: capt6000: %s; will "
						"retry\n", err);
				return BACKEND_RETRY;
			}
			if (capt_get_xstatus(device, &probe) < 0) {
				fputs("STATE: +offline-report\n", stderr);
				fputs("ERROR: capt6000: the printer is not "
				      "responding to CAPT commands.  Switch "
				      "it off, wait a few seconds and switch "
				      "it back on.\n", stderr);
				capt_close(device);
				return BACKEND_STOP;
			}
		}
		fputs("STATE: -offline-report\n", stderr);
	}

	rc = run_job(device, fd);

	capt_close(device);
	device = NULL;
	if (argc == 7)
		close(fd);

	fprintf(stderr, "DEBUG: capt6000: backend exiting with %d\n", rc);
	return rc;
}
