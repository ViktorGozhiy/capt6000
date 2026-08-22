/*
 * capt.h -- CAPT command codes, transport and protocol helpers.
 * Part of capt6000, a CUPS driver for the Canon LBP6000/LBP6018.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Copyright (C) 2026  Viktor Hozhyi <https://github.com/ViktorGozhiy/capt6000>
 *
 * The CAPT command codes and the status word bit assignments were
 * reverse engineered by the captdriver project:
 *   Copyright (C) 2013  Alexey Galakhov <agalakhov@gmail.com>
 *
 * This program is free software under the terms of the GNU General
 * Public License, version 3 or any later version; see the file
 * LICENSE.  There is NO WARRANTY, to the extent permitted by law.
 */

#ifndef CAPT6000_CAPT_H
#define CAPT6000_CAPT_H

#include <stddef.h>
#include <stdint.h>

/*
 * CAPT packet: little endian 16 bit opcode, little endian 16 bit total
 * size including the four byte header, then the payload.  Commands
 * that have a reply must have that reply read before anything else is
 * sent, or the printer wedges.
 */
enum capt_cmd {
	CAPT_NOP		= 0xA0A0,
	CAPT_CHKJOBSTAT		= 0xA0A1,
	CAPT_CHKXSTATUS		= 0xA0A8,

	CAPT_IEEE_IDENT		= 0xA1A0,	/* raw string reply */
	CAPT_IDENT		= 0xA1A1,

	CAPT_JOB_BEGIN		= 0xA2A0,
	CAPT_START_0		= 0xA3A2,

	CAPT_PRINT_DATA		= 0xC0A0,	/* no reply */
	CAPT_PRINT_DATA_END	= 0xC0A4,	/* no reply */

	CAPT_SET_PARM_PAGE	= 0xD0A0,	/* no reply */
	CAPT_SET_PARM_1		= 0xD0A1,	/* no reply */
	CAPT_SET_PARM_2		= 0xD0A2,	/* no reply */
	CAPT_SET_PARM_HISCOA	= 0xD0A4,	/* no reply */
	CAPT_SET_PARMS		= 0xD0A9,	/* container, no reply */

	CAPT_CHKSTATUS		= 0xE0A0,
	CAPT_START_2		= 0xE0A2,
	CAPT_START_1		= 0xE0A3,
	CAPT_START_3		= 0xE0A4,
	CAPT_UPLOAD_2		= 0xE0A5,
	CAPT_FIRE		= 0xE0A7,
	CAPT_JOB_END		= 0xE0A9,
	CAPT_LBP6000_SETUP	= 0xE0BA,

	CAPT_JOB_SETUP		= 0xE1A1,
	CAPT_GPIO		= 0xE1A2
};

/* Largest payload the printer will accept in one 0xC0A0 packet. */
#define CAPT_MAX_DATA_CHUNK	0xFF00u

/* ------------------------------------------------------------------ */
/* Transport                                                           */
/* ------------------------------------------------------------------ */

struct capt_dev;

/* Enumerate attached CAPT printers, calling cb for each. */
typedef void (*capt_enum_cb)(const char *make, const char *model,
			     const char *serial, const char *device_id,
			     void *user);
int capt_usb_enumerate(capt_enum_cb cb, void *user);

/*
 * Open a printer.  serial may be NULL to take the first one found.
 * On failure returns NULL and fills err (which may be NULL).
 */
struct capt_dev *capt_usb_open(const char *serial, char *err, size_t errlen);
void capt_close(struct capt_dev *dev);

/*
 * Port-reset the device.  The handle is invalid afterwards; reopen.
 * Returns 0 if the reset was accepted.
 */
int capt_usb_reset(struct capt_dev *dev);

/* IEEE 1284 device ID string, cached at open time. */
const char *capt_device_id(const struct capt_dev *dev);

/* Copy the MDL: field out of the device ID.  Returns 0 on success. */
int capt_device_model(const struct capt_dev *dev, char *out, size_t outlen);

/* Log every packet to stderr when enabled. */
void capt_set_trace(int on);

/* ------------------------------------------------------------------ */
/* Protocol                                                            */
/* ------------------------------------------------------------------ */

/* Send a command that has no reply.  Returns 0 on success. */
int capt_send(struct capt_dev *dev, uint16_t cmd,
	      const void *data, size_t size);

/*
 * Send a command and read its reply.  reply/reply_size may be NULL if
 * the caller does not care; *reply_size is set to the payload length.
 */
int capt_sendrecv(struct capt_dev *dev, uint16_t cmd,
		  const void *data, size_t size,
		  uint8_t *reply, size_t *reply_size);

/* Build a 0xD0A9 container out of several sub-commands. */
struct capt_multi {
	uint8_t buf[1024];
	size_t size;
};
void capt_multi_begin(struct capt_multi *m, uint16_t cmd);
int capt_multi_add(struct capt_multi *m, uint16_t cmd,
		   const void *data, size_t size);
int capt_multi_send(struct capt_dev *dev, struct capt_multi *m);

/* ------------------------------------------------------------------ */
/* Status                                                              */
/* ------------------------------------------------------------------ */

struct capt_status {
	uint16_t word[7];
	uint16_t page_decoding;
	uint16_t page_printing;
	uint16_t page_out;
	uint16_t page_completed;
	uint16_t page_received;
	int valid;
};

#define CAPT_FLAG(w, b)		(((unsigned) (w) << 16) | (1u << (b)))

enum capt_flag {
	CAPT_FL_JOBSTAT_CHANGED	= CAPT_FLAG(0, 9),
	CAPT_FL_XSTATUS_CHANGED	= CAPT_FLAG(0, 8),
	CAPT_FL_BUSY		= CAPT_FLAG(0, 7),
	CAPT_FL_UNINIT1		= CAPT_FLAG(0, 5),
	CAPT_FL_UNINIT2		= CAPT_FLAG(0, 4),
	CAPT_FL_BUFFER_FULL	= CAPT_FLAG(0, 2),
	CAPT_FL_NO_PAPER1	= CAPT_FLAG(0, 1),
	CAPT_FL_PROCESSING	= CAPT_FLAG(0, 0),

	CAPT_FL_NO_PAPER2	= CAPT_FLAG(1, 14),
	CAPT_FL_PROCESSING_DATA	= CAPT_FLAG(1, 7),
	CAPT_FL_BUTTON		= CAPT_FLAG(1, 5),
	CAPT_FL_PRINTING	= CAPT_FLAG(1, 2),
	CAPT_FL_POWERING_UP	= CAPT_FLAG(1, 0),

	CAPT_FL_BUTTON_PRESSED	= CAPT_FLAG(2, 8),
	/* Active high: set means the printer has no complaint.  It goes
	 * low while the engine wants attention and comes back once the
	 * condition is cleared, which is how a job resumes. */
	CAPT_FL_NO_ERROR	= CAPT_FLAG(2, 7),

	CAPT_FL_POWERING_UP2	= CAPT_FLAG(3, 12),

	CAPT_FL_BUTTON_ACTIVE	= CAPT_FLAG(4, 0)
};

static inline int capt_flag(const struct capt_status *s, enum capt_flag f)
{
	return (s->word[f >> 16] & (f & 0xFFFFu)) != 0;
}

int capt_get_status(struct capt_dev *dev, struct capt_status *out);
int capt_get_xstatus(struct capt_dev *dev, struct capt_status *out);
/* Poll the extended status until the printer stops reporting BUSY. */
int capt_wait_ready(struct capt_dev *dev, struct capt_status *out);

/* Log every status read, for diagnosing engine behaviour. */
void capt_set_status_trace(int on);

#endif /* CAPT6000_CAPT_H */
