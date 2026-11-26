# EDIScope

EDIScope is a DAB/DAB+ ETI/EDI player and diagnostic tool built around
**EDI**: receiving, timing and inspecting an ODR-DabMux EDI output live
over the network, with real-time pacing and on-screen diagnostics
(buffering margin, live ensemble changes, CU occupancy, FIG repetition,
etc.).

## Screenshot

![Screenshot of EDIScope](doc/ediscope.png)


## Features

* **EDI network input.** Connect directly to a live TCP or UDP (uni- or
  multicast) EDI source from the source box (with PFT/Reed-Solomon FEC
  fragment reassembly handled transparently). The last 20 sources
  successfully connected to are remembered, the connection is
  re-established automatically if it drops, and a different mux can be
  chosen at any time without restarting. See
  [EDI network input](#edi-network-input).

* **Real-time EDI timestamp pacing and diagnostics.** Each EDI frame's
  embedded ATST/TIST timestamp is decoded and used to play every frame
  at exactly its own scheduled time (rather than just draining the
  stream as fast as it arrives), and the current **margin delay** (how
  far ahead of schedule the network is actually delivering frames) is
  shown live, with the mux's `TIST@FCT0` configuration value available
  as a tooltip. See [EDI timing diagnostics](#edi-timing-diagnostics).

* **Live ensemble/service monitoring.** A sortable **services table**
  (SubChId, SID, ECC, label, short label, CUs, protection, bit rate,
  format/samplerate/audio mode, language, PTY, SPI and RadioDNS
  availability), a **CU occupancy bar** (864 CUs/CIF) with click-to-play,
  a live **ECC → country** lookup, the connection state and the FIB CRC
  errors in the status bar, and automatic removal of services that
  disappear from FIG 0/2. See
  [Services table and CU bar](#services-table-and-cu-bar).

* **FIG analysis.** The repetition of every FIG is measured - per entity:
  each sub-channel, service, linkage set, frequency entry, label... - and
  checked against the repetition rates of ETSI TS 103 176, including the
  CEI and announcement bursts. See [FIG analysis](#fig-analysis).

* **Service following.** Service linking (FIG 0/6), Frequency
  Information (FIG 0/21) and OE Services (FIG 0/24), cross-referenced
  with each other. See [Links](#links).

* **RadioDNS.** Every audio service is resolved automatically (ETSI TS
  103 270), stage by stage, up to its Service Information. See
  [RadioDNS](#radiodns).

* **SPI (Service and Programme Information) / EPG.** When the ensemble
  carries an SPI data service, EDIScope detects it automatically and
  decodes its services, programme schedules, groups and logos. See
  [SPI (Service and Programme Information)](#spi-service-and-programme-information).

* **Visualizations.** A frequency **spectrum analyzer** (bar or bubble
  style, mono or stereo), a stereo **vectorscope/goniometer**, a phase
  **correlation meter** and a peak **level meter**, alongside the
  Dynamic Label (with DL Plus) and the Slideshow (with the last 4 slides).

* **Recording.** The current service (untouched audio stream), or the
  whole ensemble as EDI or as ETI-NI. See [Recording](#recording).

* **No live DAB/RTL-SDR reception.** EDIScope focuses on ETI and EDI
  *streams* - from the network or from a file - not on demodulating DAB
  off the air itself.

* **Desktop integration.** Its own icon embedded in the binary and a
  `.desktop` entry so it shows up correctly in a taskbar, dock or
  Alt-Tab switcher.


## Requirements

### General

EDIScope targets a current Linux distribution - Debian 13 (trixie) is the
reference. Besides Git, a C/C++ compiler (with C++17 support) and CMake are
required to build it:

```sh
sudo apt install git g++ cmake
```


### Libraries

The following libraries are required:

* Qt 6 (Widgets)
* mpg123 (1.14.0 or higher)
* FAAD2
* SDL2
* OpenSSL (for RadioDNS over HTTPS)

Usually the `glibc` implementation of `iconv` is available. If this is
not the case, in addition `libiconv` is required.

In rare cases, the target architecture does not support *atomics*. In
such a case, EDIScope is linked against GCC's `libatomic`. This lib
usually is an (indirect) dependency of GCC itself.

On Debian 13 they are installed with:

```sh
sudo apt install qt6-base-dev libmpg123-dev libfaad-dev libsdl2-dev libssl-dev
```

### Alternative DAB+ decoder

Instead of using FAAD2, DAB+ channels can be decoded with [FDK-AAC](https://github.com/mstorsjo/fdk-aac).
You can also use [OpenDigitalradio's fork](https://github.com/Opendigitalradio/fdk-aac), if already installed.

On Debian 13, FDK-AAC is installed with:

```sh
sudo apt install libfdk-aac-dev
```

When the alternative AAC decoder is used, the FAAD2 library mentioned
above is no longer required.

After installing the library, to use FDK-AAC instead of FAAD2, you have to
have `-DUSE_FDK-AAC=1` as part of the `cmake` command.


### Audio output

The decoded audio is played through SDL2.

Services with surround sound are only decoded from their Mono/Stereo
core, as unfortunately there is no FOSS AAC decoder which supports the
required Spatial Audio Coding (SAC) extension of MPEG Surround at the
moment.


## Compilation

From this repository's root:

```sh
mkdir build
cd build
cmake ..
make
sudo make install
```

This produces `src/ediscope`. Each build also registers a desktop
launcher and the icon for the current user, pointing at the build's own
output.


## Usage

Start EDIScope from the applications menu (or run `ediscope`) and choose
a source in the source box (see below).

The **Tools** menu opens the Links, RadioDNS, SPI / EPG and FIG Analysis
windows. Up to three of them can also be given a shortcut button in the
toolbar.

**Settings** holds the recordings folder and the choice of toolbar
shortcuts. They are kept in `~/.config/ediscope/ediscope.conf`.


### EDI network input

In the source box, enter an EDI URI and click "Connect" (or press Enter):

* `tcp://host:port`
* `udp://host:port` (unicast)
* `udp://mcast-ip:port` (multicast)

PFT fragments (ETSI TS 102 821 Annex B), if used by the sender, are
reassembled transparently, including Reed-Solomon FEC correction of
missing fragments. If a TCP connection drops (e.g. the mux restarts),
EDIScope reconnects by itself every 5 seconds.

The last 20 sources successfully connected to - those from which at
least one frame was received, so a mistyped one never ends up there -
are remembered across restarts (`~/.config/ediscope/network_history`).
Clicking "Disconnect" (or connecting to a different source) stops the
current one and switches over cleanly.

The source box also takes a file - `file:///path/to/file.eti` (or
`.edi`), or "Open EDI/ETI File..." from its drop-down. A file is played
at the fixed pace of one frame per 24 ms, regardless of the timestamps
its frames may carry.


### EDI timing diagnostics

An EDI stream normally carries an accurate timestamp (ATST, inside the
`deti` TAG item's TIST/FCT fields) with every frame, set by the mux to
some configured amount of time (`tist_offset`) ahead of when the frame
is actually meant to be played. EDIScope decodes this timestamp and
paces playback against it directly: each frame is held until its own
scheduled time, rather than being played back as fast as it arrives.
A frame that is already due by the time it's received (e.g. an old
frame replayed in a mux's initial TCP preroll burst) is dropped instead
of being rushed through, so reconnecting to a mux doesn't cause a burst
of fast-forwarded, glitchy audio.

The "Margin" figure in the status bar shows, live, how far ahead of its
own schedule the network is currently delivering frames - this should
settle close to (but just under) zero minus the mux's `tist_offset`,
net of real network latency, and will naturally jitter with it. Its
tooltip shows the `TIST@FCT0` value observed on the wire (the
fractional-second part of the timestamp at FCT 0), letting you verify a
mux's `tist_at_fct0` configuration directly from the stream.


### Services table and CU bar

The services table lists every service currently in the ensemble
(SubChId, SID, ECC - with a country name looked up when known - label,
short label, CUs, protection, bit rate, format/samplerate/audio mode,
language, PTY), sortable by any column. Data-only sub-channels (e.g. an
EPG) are listed too, in italics, so that their CUs are accounted for.
The "SPI/SI", "SPI/PI" and "rDNS" columns mark whether SPI has Service
Information or at least one day of Programme Information for the
service, and whether its RadioDNS resolution succeeded.

Below it, the CU occupancy bar shows all 864 CUs/CIF, one block per
sub-channel (the selected service's outlined, data sub-channels
hatched), with the number of CUs used and free; hovering a block shows
its service, and clicking it plays that service. Both are locked while a
service is being recorded, so it can't be switched away from by mistake.

Services and sub-channels that disappear from FIG 0/2 are automatically
removed after a timeout, rather than lingering forever.


### FIG analysis

The "FIG Analysis" window shows how often every FIG is repeated, checked
against ETSI TS 103 176 (clause 4 and table 1):

* MCI (FIG 0/0, 0/1, 0/2, 0/3, 0/7, 0/14): all of it every 96 ms;
  FIG 0/8 and FIG 0/13: at least every second (for FIG 0/8, as in the
  standard's annex F);
* SI (FIG 0/5, 0/9, 0/10, 0/17, 0/18, 0/20, 0/25, FIG 1/x, FIG 2/x):
  once per second;
* FIG 0/6, 0/21, 0/24: all of the database at least every 2 minutes, the
  activation state of each linkage set at least every 10 seconds, and
  each change event indication (CEI) once per second for 5 seconds;
* FIG 0/19, 0/26: the start of an announcement 10 times per second for 5
  seconds, its continuation once per second, its end 10 times per second
  for 2 seconds.

The repetition is measured per entity (each sub-channel, service,
component, linkage set, frequency entry, label, ...), as the standard
phrases it, and time is counted in frames, so network jitter doesn't
affect it. Each requirement gets a verdict: ✓ at the nominal rate, ~
slower but not below one third of it (the minimum the standard allows),
✗ below, or "…" while the observation is still too short to tell.
Selecting a requirement shows its measurement per entity, or per event
for the bursts.

The window also lists every FIG seen (count, rate, bytes, longest
interval) and every burst (CEI, announcements) with its own verdict,
together with the FIC load, the FIB CRC errors and - from the number of
service components - what the FIC capacity allows according to the
standard. The analysis runs all the time, from the moment a source is
started; "Reset" starts it again.


### Links

The "Links" window shows the service following information of the
ensemble: for each service, the linkage set it is the key service of
(FIG 0/6) with its members, every Frequency Information entry (FIG 0/21)
and every OE Services entry (FIG 0/24). A DAB member of a linkage set is
cross-referenced against FIG 0/24 to find the ensemble(s) carrying it,
and from there against FIG 0/21 to find their frequencies. Services of
another ensemble named by a linkage set are listed too. Any cell or row
can be copied with a right-click.


### RadioDNS

Every audio service is resolved automatically (ETSI TS 103 270) as soon
as its broadcast parameters are known. The "RadioDNS" window shows, per
service, how far the resolution got - the RadioDNS FQDN, the DNS CNAME,
the DNS SRV of its SPI application, and the HTTP(S) fetch of its SI.xml -
and, once resolved, what the SI.xml says about it: names, descriptions,
logos (shown full size on hover), bearers (copied with a right-click,
streams can be opened) and the document itself.


### SPI (Service and Programme Information)

When the ensemble's FIG 0/13 signals a data service carrying the EPG
user application (ETSI TS 102 371 clause 6.2), EDIScope automatically
locks onto its MSC packet-mode sub-channel, reassembles the underlying
MOT Directory-mode carousel (ETSI EN 301 234 clause 7.2) and decodes
whichever SPI binary objects (ETSI TS 102 371 clause 5 / ETSI TS 102
818) it finds inside - no action required from you. As the carousel
repeats slowly, the services and their logos can take a minute or more
to arrive, the schedules even longer.

The "SPI / EPG" window has four tabs:

* **Services** - one card per service known to the SPI Service
  Information (SI) document: its name, the DAB bearer it refers to, and
  its logos (shown full size on hover).
* **Programme** - a per-service day timeline built from that service's
  own Programme Information (PI) schedule object(s) - one exists per
  service per day - picked from a drop-down.
* **Groups** - one card per programme group described by the ensemble's
  Group Information (GI) document (series, shows, magazines, etc.).
* **XML** - a reconstruction, as readable XML, of whichever of the
  three document types above you pick from a drop-down.

Compressed MOT directories (data group type 7) are not supported, as
ETSI TS 102 371 forbids a mux from using them for SPI in the first
place.


### Date/Time

The local date/time is displayed in the status bar as soon as both the
UTC date/time and the Local Time Offset (LTO) have been received. The
clock is then resynchronised upon further received UTC date/time.

If an ensemble transmits both short (minute precision) and long (millisecond
precision) form, only the long form will be used, as soon as received for the
first time.


### Announcement support/switching

The status bar shows which announcements the current service supports.
During a matching announcement, its type is highlighted and marked with
▶; an announcement that would lead to a (temporary) switch to a
different sub-channel is marked with ◆ instead, e.g. if traffic news of
a different channel shall affect also listeners of a different service
(which does not have its own announcements).

An active alarm announcement on ensemble level is indicated by "⚠ Alarm"
after the ensemble label, highlighted in red.


### Hotkeys

Hotkey           | Meaning
-----------------|--------------------------------------------------------------
`m`              | Enable/disable audio mute
`r`              | Start recording the current service / stop the recording
`Ctrl` + `c`     | Copy the Dynamic Label text to the clipboard
`-`              | Switch to previous service
`+`              | Switch to next service
`1`..`0`         | Switch to 1st..10th service
`Alt` + `1`..`0` | Switch to 11th..20th service

The hotkeys don't apply while typing in the source box.


### Recording

The "Record" menu starts a recording:

* **Record current service** (`r`) - the current service's audio, as
  broadcast;
* **Record ensemble as EDI (.edi)** - every EDI AF packet as received
  (EDI sources only);
* **Record ensemble as ETI (.eti)** - every frame as ETI-NI: converted
  from EDI, or as received from an ETI source. EDIScope can play it back.

One recording runs at a time. It is stopped from the "Record" menu, with
`r`, or with the "Stop recording" button of the toolbar; a summary of
the recording (file, type, duration, size) is then shown. While the
current service is being recorded, the service can't be changed; while
any recording is in progress, EDIScope can't be closed.

Recordings are stored in the folder chosen in Settings (`/tmp` by
default). The filename of a recording
contains the timestamp of the start of the recording and the service or
ensemble name, e.g. `2018-09-02 - 17-53-54 - SWR3.aac`.

Service recordings are lossless: A DAB service is directly stored in MP2
format. A DAB+ service is packaged into the LATM/LOAS container format.
This format is necessary in order to signal the special 960 samples per
frame transformation to the AAC decoder for playback.

As both formats are streamable, a recording can already be played while
the actual recording is still in progress.

The AAC recordings can be played e.g. by VLC player. FFmpeg will play
them as well, but currently does not support the SBR extension together
with the 960 samples per frame transformation and will thus only play
the AAC core then.

### Secondary component audio services

Some ensembles may contain audio services that consist of additional
"sub services" called secondary components, in addition to the primary
component.

In the services table such components are shown prefixed with `» `
(e.g. `» BBC R5LiveSportX`). Meanwhile, the related primary component is
suffixed with ` »` (e.g. `BBC Radio 5 Live »`).


### Dynamic Label Plus (DL+)

When DL+ objects are received, they are listed in the Dynamic Label Plus
panel. Some stations only transmit DUMMY objects - in this case the
panel stays empty.

If the running state indicates that the current item is running, all
objects with an ITEM content type are marked with ▶ and highlighted.

Depending on the object category, the object text is highlighted in bold
text or a different text color. Deleted objects are struck through and
greyed out.


### Slideshow

The MOT Slideshow is shown in the main window. If the current service
signals to transmit a Slideshow, a slide is shown after it has been
received completely and without errors.

While the next slide is being received, the progress bar below the slide
shows the fraction that has successfully been received so far. It is
always updated after a new segment has arrived free from error. The
first segments of the initial slide usually are received before the
corresponding header comes by. In this case a busy indicator is shown
until the header is available.

The last 4 slides shown before the current one are kept as a vertical
strip of thumbnails to its right. Hovering over the current slide or a
thumbnail shows it at full size with its resolution, file size, format,
content name and ClickThroughURL (if any). Right-clicking it offers to
save the original file to disk or copy the image to the clipboard;
thumbnails additionally offer "View at normal size".

Currently the following limitations apply:

* slideshows in a separate sub-channel are not supported (just X-PAD);
* the TriggerTime field does not support values other than Now


## Standards

EDIScope implements (at least partly) the following DAB standards:

### General
* ETSI EN 300 401 (DAB system)
* ETSI TS 101 756 (Registered tables)
* ETSI TS 103 466 (DAB audio)
* ETSI TS 102 563 (DAB+ audio)
* ETSI ETS 300 799 (ETI), including the conversion of EDI to ETI-NI
* ETSI TS 102 693 (EDI), including its ATST/TIST timestamp fields,
  together with ETSI TS 102 821 (DCP), including its Annex B PFT/FEC
  fragmentation
* ETSI TS 103 176 (Rules of implementation; service information
  features): FIG repetition rates, service following

### Data applications and services
* ETSI TS 102 980 (Dynamic Label Plus)
* ETSI EN 301 234 (MOT, including Directory mode, clause 7.2)
* ETSI TS 101 499 (MOT Slideshow)
* ETSI TS 102 371 (SPI - transport, including FIG 0/13 EPG signalling)
* ETSI TS 102 818 (SPI - XML Schema/content model)
* ETSI TS 103 270 (RadioDNS Hybrid Radio)


## License

This software is licensed under the GNU General Public License Version 3
(please see the file COPYING for further details).
![GPLv3 Image](https://www.gnu.org/graphics/gplv3-88x31.png)

*Please note that the included FEC lib by KA9Q has a separate license!*

EDIScope - DAB+/EDI diagnostic player
Copyright (C) 2025-2026 Yoann Queret

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
