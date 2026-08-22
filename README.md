# capt6000 — Canon LBP6000 / LBP6018 driver for CUPS

A free driver for Canon laser printers that only understand the
proprietary **CAPT** protocol (Canon Advanced Printing Technology).
Written for the LBP6000/LBP6018 — that is CAPT version 3 with Hi-SCoA
compression.

The printer identifies itself like this:

    MFG:Canon;MDL:LBP6000/LBP6018;CMD:CAPT;VER:3.0;CLS:PRINTER;DES:Canon LBP6000/LBP6018

## Warning

There is no warranty of any kind — neither express nor implied. Before
installing the driver, there is one quirk of this hardware worth knowing.

The LBP6000 firmware stops servicing its USB pipes if you send it data it
cannot parse. The printer keeps answering control requests and looks
healthy — it enumerates and reports its identifier — but it no longer
responds to any CAPT command. Software cannot recover it: clear-halt, USB
reset, re-configuring the device, and a printer-class SOFT_RESET all fail.
Only a power cycle helps.

The driver itself never drives the printer into this state and
deliberately goes quiet once the printer stops responding. But a stray
queue created with the wrong PPD gets there easily — that is exactly how
this experience was earned. If there is a second queue pointing at the
same printer, delete it.

## What it is made of

| Component | What it does |
|---|---|
| `rastertolbp6000` | CUPS filter: raster → Hi-SCoA bands + page parameters |
| `capt` | CUPS backend: USB transport and the CAPT job state machine |
| `Canon-LBP6000.ppd` | PPD with page geometry, toner density, media types |
| `captstat` | Diagnostics: printer status, prints nothing |
| `captdump` | Parses the intermediate stream, decodes pages back to PBM |
| `captwire` | Parses a raw CAPT dump into a readable command sequence |

### Why the filter and backend are split the way they are

CAPT is a fully bidirectional protocol with flow control. For every
command that has a reply, the reply **must** be read before the next
command is sent, otherwise the printer wedges solid. On top of that, the
next page may only be handed over once the printer has said it has room,
and the fact that a sheet has come out has to be learned by polling
counters.

This dialogue has to happen where the device is open. In CUPS terms that
is the backend. Hence the split:

* the **filter** does all the compute-heavy work — raster to Hi-SCoA, page
  parameters — and emits a simple stream of records (`src/captstream.h`);
* the **backend** owns the USB device and carries the conversation.

Existing CAPT drivers carry the dialogue from the filter through the CUPS
back- and side-channel, and that has long been a source of their
fragility. Here the backend talks to the device directly via libusb.

## Building and installing

You need `libcups2-dev`, `libcupsimage2-dev`, `libusb-1.0-0-dev`,
`pkg-config`, and a C compiler.

    make
    make check          # codec tests + PPD check
    sudo make install

It installs into `/usr/lib/cups/filter`, `/usr/lib/cups/backend`,
`/usr/share/cups/model/capt6000`, and `/usr/local/bin`.

## Installing from a .deb package

On Debian and Ubuntu you can skip compiling. Download the latest
`capt6000_*.deb` from the [releases page][releases] and install it — `apt`
pulls in the dependencies:

    sudo apt install ./capt6000_1.0_amd64.deb

To build the package yourself instead:

    sudo apt install build-essential debhelper \
         libcups2-dev libcupsimage2-dev libusb-1.0-0-dev pkg-config
    dpkg-buildpackage -us -uc -b

The `.deb` is written to the parent directory.

[releases]: https://github.com/ViktorGozhiy/capt6000/releases

## Setting up a queue

    sudo /usr/lib/cups/backend/capt          # see what was found
    sudo lpadmin -p LBP6000 -E \
         -v 'capt://Canon/LBP6000%2FLBP6018?serial=XXXXXXXX' \
         -P /usr/share/cups/model/capt6000/Canon-LBP6000.ppd

Print options: `PageSize`, `MediaType`, `CaptDarkness` (0–15, default 7),
`CaptTonerSave`.

## How this was verified

The protocol is closed, so verification was built so as not to depend on a
single lucky print alone.

1. **Codec round-trip.** A decompressor was written to match the
   compressor; 51 tests — an empty band, a solid fill, horizontal and
   vertical stripes, a text-like page, random noise, degenerate
   geometries, long runs through PREFIX, and 40 fuzz cases — all match
   byte for byte. Separately, it is checked that a buffer overflow is
   reported rather than silently truncated, and that a truncated stream is
   rejected.

2. **End-to-end page check.** An A4 test page goes through
   PDF → CUPS raster → Hi-SCoA → decode back: all 4,011,392 raster bytes
   are recovered exactly, and the image was checked by eye.

3. **Wire check.** Offline mode (`CAPT_SINK=file`) writes exactly the
   stream that would have gone to the printer, and `captwire` shows the
   command sequence.

4. **Live printing** on a real LBP6000 — single-page jobs both directly
   through the backend and through a CUPS queue, plus a three-page job on
   which it was checked that the printer accepts the next page while
   printing the previous one, and that the page counters stay in step.

## Page geometry

The numbers in `src/pagegeom.c` are what Canon's own driver puts into the
`0xD0A0` command; they are not fully derivable by formula, because the
standard sizes get two extra raster lines while envelopes and custom sizes
do not. For everything else these rules hold:

    line_size = round_up_4(ceil((paper_width - 2*bound_a) / 8))
    num_lines = paper_height - 2*bound_a        (+2 for standard sizes)

The imageable areas in the PPD are chosen so that the RIP hands the filter
exactly the raster the printer expects: for A4 that is 592 bytes per line
and 6776 lines.

## Behaviour on misfeeds

If the printer fails to pick up a sheet, it raises the no-paper flag and
clears the "no errors" bit. In that case the driver:

* blinks the panel indicator so you can see which machine is waiting;
* reports `media-empty-error` to CUPS — the state is visible in the queue;
* waits for the printer to clear the error itself, up to an hour. The page
  stays in the printer's memory and finishes printing on its own; there is
  no need to resend it;
* the job is not lost in the process.

The queue is best kept on the `stop-printer` policy:

    sudo lpadmin -p LBP6000 -o printer-error-policy=stop-printer

With `retry-job`, CUPS will start printing again into a printer that has
not yet recovered, and will almost certainly drive it into the state it
only comes out of with a power cycle.

## Diagnostics

    captstat              # printer status
    captstat -w           # the same, in a loop
    captstat -r           # reset the USB device

A detailed job log:

    sudo cupsctl --debug-logging
    grep capt6000 /var/log/cups/error_log

Tracing: `CAPT_STATUS_TRACE=1` in the backend's environment prints every
change of the status words and page counters, `CAPT_TRACE=1` — on top of
that, every packet in full. This is what a healthy three-page job looks
like:

    status 8E31 0000 0080 0000 0056 ...  pages dec=0 prn=0 out=0 done=0 rcv=0
    status 8E00 0080 0080 0000 0057 ...  pages dec=1 prn=0 out=0 done=0 rcv=1
    status 8E00 0084 0080 0000 0057 ...  pages dec=1 prn=1 out=1 done=0 rcv=1
    ...
    status 8E00 0004 0080 0000 0057 ...  pages dec=3 prn=3 out=3 done=3 rcv=3

The counters should move in step: `dec → rcv → prn → out → done`.

### The printer stopped responding

If the printer answers USB control requests but stays silent on CAPT, its
firmware has wedged — most often because data of the wrong format was sent
to it (for example, a queue with the wrong PPD). The backend tries once to
reset the device itself; if that does not help, the printer has to be
power-cycled. Software cannot recover it: clear-halt, USB reset,
re-configuration, and a printer-class SOFT_RESET do not bring it back to
life.

## Limitations

* Monochrome output at 600 dpi only — that is all the hardware can do.
* No duplex (on the LBP6000 it is manual only).
* Copy ordering is handled by CUPS, not the printer.
* Bytes 2–3 of command `0xD0A0` (`0x2A30`) and byte 13 (`0x11`) are taken
  from a known working configuration; for other models in the family they
  may differ.

## Provenance and attribution

This is a derivative work of
[captdriver](https://github.com/agalakhov/captdriver), and it is
distributed under GPL-3.0-or-later not by choice but because it cannot be
distributed any other way.

CAPT is undocumented. The protocol and the Hi-SCoA format were reverse
engineered by the captdriver contributors, and the code here is written
afresh but follows their work. The following are derivative:

* the Hi-SCoA compressor and decompressor — the command set, the encoding
  rules, and the compression strategy are taken from `hiscoa-compress.c`;
* the CAPT command codes and the bit layout of the status words;
* the packet framing, the handling of BCD lengths, and the parsing of the
  status record;
* the job and page command sequence, the magic payloads, and the layout of
  command `0xE1A1`;
* the layout of the `0xD0A0` page prologue;
* the page geometry table — a reproduction of measurements taken from
  Canon's own driver and published in the project wiki.

Original work: the USB transport, the split into filter and backend, the
intermediate format, the decompressor as a standalone verification tool,
the PPD, the tests, and the diagnostic utilities.

The people whose work made all of this possible:

* Alexey Galakhov \<agalakhov@gmail.com\> — captdriver, LBP2900 reverse
  engineering, the consolidated protocol description
* Nicolas Boichat \<nicolas@boichat.ch\> — LBP-810 reverse engineering
* Benoit Bolsee \<benoit.bolsee@online.be\> — LBP-3010 reverse engineering
* Vitaliy Tomin \<highwaystar.ru@gmail.com\> — captdriver
* Alexei Gordeev \<KP1533TM2@gmail.com\> — LBP3000/LBP6000 support
* Moses Chong — [captdriver fork](https://github.com/mounaiban/captdriver),
  wiki and page parameter tables

The original work was based on Rildo Pragana's work on the Samsung ML-85P.

Their repositories are not included in this one — they are separate works
with their own copyrights. If you need them for cross-reference:

    git clone https://github.com/agalakhov/captdriver
    git clone https://github.com/mounaiban/captdriver
    git clone https://github.com/mounaiban/captdriver.wiki

Two discrepancies with their published `SPECS`, found while working:

* the PREFIX command formula is given as `128 * ((1 << order) - 1 - N)`. It
  does not match the stream the reference compressor produces; the correct
  one is `128 * ((1 << (order + 1)) - 1 - N)`. `src/hiscoa_dec.c` uses the
  corrected form.

* bit 7 of status word 2 is described as "Problem". On a live LBP6000 it
  behaves exactly the other way round: it stays set the whole time the
  printer is healthy, and clears for the duration of a misfeed. That is, it
  is an active-high "no errors" bit — which is what it is called here
  (`CAPT_FL_NO_ERROR`), and it is on its return that the driver resumes the
  job.

## Trademarks

Canon and LBP6000 are trademarks of Canon Inc. They are used here only to
identify the hardware this driver is compatible with. This is unofficial
software. It is not made, endorsed, or otherwise associated with Canon Inc.
in any way. It uses no Canon code.

## License

GNU General Public License, version 3 or any later version — the full text
is in the `LICENSE` file.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
