# capt6000 -- Canon LBP6000/LBP6018 CAPT driver for CUPS
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Copyright (C) 2026  Viktor Hozhyi <https://github.com/ViktorGozhiy/capt6000>
#
# This program is free software under the terms of the GNU General
# Public License, version 3 or any later version; see the file LICENSE.
# There is NO WARRANTY, to the extent permitted by law.

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -Wall -Wextra -Wno-unused-parameter -std=gnu99
LDFLAGS ?=

PKG_CONFIG ?= pkg-config

CUPS_CFLAGS := $(shell $(PKG_CONFIG) --cflags cups 2>/dev/null)
CUPS_LIBS   := $(shell $(PKG_CONFIG) --libs cups 2>/dev/null || echo -lcups)
USB_CFLAGS  := $(shell $(PKG_CONFIG) --cflags libusb-1.0)
USB_LIBS    := $(shell $(PKG_CONFIG) --libs libusb-1.0)

PREFIX ?= /usr/local
CUPS_SERVERBIN ?= $(shell cups-config --serverbin 2>/dev/null || echo /usr/lib/cups)
CUPS_DATADIR   ?= $(shell cups-config --datadir 2>/dev/null || echo /usr/share/cups)
PPD_DIR        ?= $(CUPS_DATADIR)/model/capt6000

BUILD := build

FILTER_OBJS  := $(BUILD)/rastertolbp6000.o $(BUILD)/hiscoa_enc.o \
                $(BUILD)/pagegeom.o
BACKEND_OBJS := $(BUILD)/backend_capt.o $(BUILD)/capt_usb.o
DUMP_OBJS    := $(BUILD)/captdump.o $(BUILD)/hiscoa_dec.o $(BUILD)/hiscoa_enc.o
STAT_OBJS    := $(BUILD)/captstat.o $(BUILD)/capt_usb.o
WIRE_OBJS    := $(BUILD)/captwire.o
TEST_OBJS    := $(BUILD)/test_hiscoa.o $(BUILD)/hiscoa_enc.o $(BUILD)/hiscoa_dec.o

TARGETS := $(BUILD)/rastertolbp6000 $(BUILD)/capt $(BUILD)/captdump \
           $(BUILD)/captstat $(BUILD)/captwire

.PHONY: all clean install uninstall check test

all: $(TARGETS)

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/rastertolbp6000.o: src/rastertolbp6000.c | $(BUILD)
	$(CC) $(CFLAGS) $(CUPS_CFLAGS) -c -o $@ $<

$(BUILD)/backend_capt.o: src/backend_capt.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/capt_usb.o: src/capt_usb.c | $(BUILD)
	$(CC) $(CFLAGS) $(USB_CFLAGS) -c -o $@ $<

$(BUILD)/captdump.o: tools/captdump.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/captstat.o: tools/captstat.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/captwire.o: tools/captwire.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/test_hiscoa.o: tests/test_hiscoa.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/rastertolbp6000: $(FILTER_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(CUPS_LIBS)

$(BUILD)/capt: $(BACKEND_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(USB_LIBS)

$(BUILD)/captdump: $(DUMP_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD)/captstat: $(STAT_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(USB_LIBS)

$(BUILD)/captwire: $(WIRE_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD)/test_hiscoa: $(TEST_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

check test: $(BUILD)/test_hiscoa
	$(BUILD)/test_hiscoa
	@if command -v cupstestppd >/dev/null 2>&1; then \
		cupstestppd -q -W filters ppd/Canon-LBP6000.ppd && \
		echo "ok   PPD passes cupstestppd"; \
	fi

install: all
	install -d $(DESTDIR)$(CUPS_SERVERBIN)/filter
	install -d $(DESTDIR)$(CUPS_SERVERBIN)/backend
	install -d $(DESTDIR)$(PPD_DIR)
	install -m 755 $(BUILD)/rastertolbp6000 $(DESTDIR)$(CUPS_SERVERBIN)/filter/
	install -m 700 $(BUILD)/capt $(DESTDIR)$(CUPS_SERVERBIN)/backend/
	install -m 644 ppd/Canon-LBP6000.ppd $(DESTDIR)$(PPD_DIR)/
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(BUILD)/captstat $(DESTDIR)$(PREFIX)/bin/
	install -m 755 $(BUILD)/captdump $(DESTDIR)$(PREFIX)/bin/
	install -m 755 $(BUILD)/captwire $(DESTDIR)$(PREFIX)/bin/

uninstall:
	rm -f $(DESTDIR)$(CUPS_SERVERBIN)/filter/rastertolbp6000
	rm -f $(DESTDIR)$(CUPS_SERVERBIN)/backend/capt
	rm -f $(DESTDIR)$(PPD_DIR)/Canon-LBP6000.ppd
	rm -f $(DESTDIR)$(PREFIX)/bin/captstat $(DESTDIR)$(PREFIX)/bin/captdump

clean:
	rm -rf $(BUILD)
