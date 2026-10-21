```
 ·▄▄▄▄  ▒█████   ██████ ▓██   ██▓     ██▓███
 ██▪ ██ ▒██▒  ██▒▒██    ▒▒ ▒ ▒        ██    ██
 ▐█· ▐█▌▒██░  ██▒░ ▓██▄  ░░   ░░      ██▓███▀
 ██▪  ██▒██   ██░  ▒   ██▒ ░ ░ ░      ██
 ·▀▀▀▀▀  ░ ████▓▒░██████▒▒ ░ ░        ▀▀   + +
```

# DOSy++

An ADC (DC++) client for MS-DOS with a 1994-era BBS interface.

Chat, search, browse, download and upload against a real ADC hub, from a
386/486 — or a browser tab. 32-bit protected mode via DJGPP, TCP/IP via
Watt-32 over a DOS packet driver, CP437 drawn straight into video memory.

**Status: it works, and the claims below are tested.** One binary runs on
real DOS (DOSBox-X), and in a browser (v86) with no changes. The same client
logs into **uhub** and interoperates with **AirDC++** in both directions.
A 100MB download completes on DOS over a packet driver in 1136 seconds —
85 KB/s including boot, login and hashing — verified part by part against
its Merkle tree.

```
 DOSy++  •  DOSyPP Stub Hub                                      8 NODES ONLINE 
┌─┤ BROWSING Razor ├───────────────────────────────────────────────────────────┐
│/DL/WAREZ/GAMES                                                               │
│──────────────────────────────────────────────────────────────────────────────│
│   ELIZA.ZIP                                         15.7K                    │
│   FREEDOOM.ZIP                                      26.3M                    │
│   DUNGEON5.ZIP                                      17.1M                    │
│   CARDPACK.ZIP                                     410.1K                    │
│   MAZERUN2.ZIP                                       2.2M                    │
│   FLIGHTSM.ZIP                                     344.8K                    │
│   GNUCHESS.ZIP                                     578.3K                    │
│   BOOM.ZIP                                           7.6M                    │
│   SPACEWAR.ZIP                                     683.5K                    │
└──────────────────────────────────────────────────────────────────────────────┘
 1-MSGS 2-WHO 3-FILES 4-XFERS 5-SHARE 6-MOTD 7-INFO 8-BROWSE 9-SETUP 0-HUBS
```

Browsing another node's share, one directory at a time. Every door is in
[`docs/SCREENS.md`](docs/SCREENS.md) — all ten plus the splash, the
sign-off and the 80x50 layout — captured by `make screens` against a live
session: four nodes logged in, a peer sharing real DOS software, a search
done, a transfer finished.

## What it does

**The hub.** Logs in (with a password if the account is registered), holds
a conversation, private messages, tracks the user list, and answers other
people's searches from its own share. Twenty-five `/`-commands, listed by
`/help`.

**Files.** Searches the hub, queues from the results or from somebody's
share, and downloads **segmented from several sources at once**: it fetches
the Merkle tree's leaf hashes, checks they rebuild the hash it is looking
for, then pulls different parts from different people and verifies each
against its own node of the tree. One bad source costs one part. A download
interrupted by a crash or a power cut resumes per part, and a finished file
needs no second pass to prove it is right.

Uploads respect a slot limit and publish how many are free. Files of 2GB or
more are refused with a reason: every seek in the program is a 32-bit
`long`, so past that the failure would not be an error but a seek to the
wrong place.

**Sharing.** Scans a directory tree, hashes it at about 500 KB/s on a 486
(33 minutes a gigabyte) and caches the result in `DOSYPP.TTH`, so only the
first run pays. `hash = manual` defers it; `rescan = 60` picks changes up
later. Leaf hashes are cached too, next to the main cache.

**Browsing, one directory at a time.** The client asks for a partial file
list — `CGET list /` for the top level, then each directory as it is opened
— because a hub regular's 200,000-file list does not fit in 16MB and one
directory always does. A peer that refuses partial lists is asked once for
the whole thing. We serve them too, generated on the spot, so a DOS client
with a big share is browsable by anyone.

**The screen.** 80×25 CP437 with ten doors, driven by its own int 9h
keyboard handler, written straight to 0xB8000 with only the changed cells
touched. `rows = 50` loads the 8×8 font and gives the chat pane 43 lines
instead of 18, with a logo twice the size on the splash. Three themes.

**Hubs.** A settings screen that writes the config file, and a hubs door
with two views: your favourites — each with its own handle and password,
because a registered account is per hub — and the public list fetched over
HTTP. One favourite can be dialled on startup.

**Music.** A ProTracker module plays while the client runs: four channels
mixed to signed 8-bit and fed to a Sound Blaster over an auto-init DMA
ring, with no interrupt handler — the DMA controller's own count register
says how much the card has taken. Effects 0 through 7 (arpeggio,
portamento, vibrato, tremolo and the combined slides); panning and the
E-series are ignored and the info door says so for whatever module is
loaded. On a 486DX2-66 the mixer costs 5.7% of the CPU at 11025 Hz, and
hashing a share drops from 521 to 490 KB/s with the music keeping up.

The tune is generated by `tools/makemod.py` rather than committed: every MOD
worth listening to belongs to whoever tracked it in 1993, and "it was on a
BBS" is not a licence. It is also the test fixture — every note is known, so
the mixer is measured rather than eyeballed.

**ADCS, through a relay.** The client contains no crypto and will not: a TLS
stack on a 386 with 16MB is a different project, and one with a bug in it is
worse than none. A relay terminates TLS on the LAN and DOSy++ dials its
plaintext side — stunnel, or `tools/tlsrelay.py`, which needs only Python 3.
The LAN leg is plaintext, including the password handshake; see
[`docs/STUNNEL.md`](docs/STUNNEL.md) for what that does and does not buy.

Favourites live in `DOSYPP.FAV`, passwords and all, in the clear. There is
nowhere better on a DOS machine — no keychain, no file permissions — and
hiding them would only misrepresent how safe they are. The file says so at
the top.

## Run it

The host build needs nothing but a C compiler, and the stub hub nothing but
Python 3 — so this works immediately, with no DOS toolchain:

```sh
make host                      # native build
make hub &                     # stub ADC hub on port 1511
./build/host/dosypp 127.0.0.1  # the UI, in your terminal
make demo                      # the same, with a share and some noise
```

For the real thing:

```sh
make dos-docker                # DJGPP in a container -> build/dos/DOSYPP.EXE
make dosfiles                  # stage it with CWSDPMI and a packet driver
make sf2                       # a SoundFont, so MPU-401 games have a synth
cd dosbox-x && dosbox-x -conf dosbox-x.conf
```

`make sf2` is the Roland path: a game writing MIDI to an MPU-401 gets
FluidSynth and a 40MB MIT-licensed SoundFont rather than silence. An
MPU-401 is a serial port, not an instrument, so that SoundFont is where the
sound quality actually comes from — see
[`docs/DOSBOX.md`](docs/DOSBOX.md), and `make dos-midi-check` measures the
audio rather than trusting the logs.

Or in a browser tab, with the whole demo in one command:

```sh
make atmos                     # real DOS software for the demo share
make warez                     # the demo hub's share, hashes precomputed
make demo-web                  # images, hub, relay, and opens the page
```

That boots FreeDOS in [v86](https://github.com/copy/v86), loads a packet
driver, and runs the same DJGPP binary as real hardware. In the client:
press `3`, type `BOOM`, Enter to search, Enter to queue, `4` to watch it
arrive hash-checked — then `/quit` and `PLAY BOOM` at the DOS prompt.
BOOM is Doom — the GPL engine with a free WAD —
and **its music plays in the browser**. v86's MPU-401 answers a game's
probe and discards the notes, so the page wraps that I/O port and
reassembles the MIDI, then either plays it through a SoundFont or sends it
out over Web MIDI to a synthesiser you already own — a Roland module, a
keyboard, Windows' own GS Wavetable. The second costs no download and, on
a desk with an SC-55 on it, is the best this can possibly sound.
The tune is a ProTracker module through the emulated Sound Blaster.

Search `BLKDROP` instead to play a GPL tetris like game
The tune is a ProTracker module through the emulated Sound Blaster.


Failing a real module, the panel also takes any `.sf2`, `.sf3` or `.dls`
file off your own disk — an SC-55 or MT-32 set you already have included.
It is read in the browser and never uploaded. None of those are bundled or
fetched: they are sample recordings of Roland hardware, and shipping one
would undo everything in [`docs/LICENSING.md`](docs/LICENSING.md).

To publish that as a static site, with no install for whoever opens it:

```sh
make pages                     # -> build/pages, ready for GitHub Pages
make demo-pages                # serve that bundle locally, with a hub
make test-pages                # serve it, boot it, download through it
```

`make demo-pages` prints a URL in a couple of seconds and then stays in
the foreground following the relay's log; Ctrl-C stops it, and
`./tools/demo-pages.sh --detach` starts it without holding the shell.

`make demo-pages` needs no GitHub account and uploads nothing: it serves
the exact directory the workflow would publish, with a hub, a serving
node and the relay behind it. See [`docs/HOSTING.md`](docs/HOSTING.md),
which also covers the part worth being careful about — **the demo reaches
no hub but its own**, and `make test-v86-hublist` checks that by reading
the relay's log rather than taking anyone's word for it.

The page needs no cross-origin isolation, so a dumb static host will do —
and **it needs no relay either**. A browser cannot open a TCP socket, so
the page is the other end of the wire instead: `web/wisp.mjs` answers the
WebSocket v86 opens and `web/adchub.mjs` is an ADC hub and a serving peer,
in the tab. The DOS client speaks real ADC and verifies every hash; the
share is fetched a file at a time from the site. `make test-v86-inpage`
boots the client against those modules and downloads a game with nothing
listening on any port. Pointing at a real relay stays an option, and is
the only way two people share a hub. `.github/workflows/pages.yml` builds and publishes it.
[`docs/PAGES.md`](docs/PAGES.md) has the details.

The bundle is 74MB, of which 38MB is an optional SoundFont and 22MB is the
demo hub's share — neither of which a visitor downloads unless they ask.
The 128MB disk image publishes as 128 zstd-compressed pieces totalling
9MB, which v86 reads natively, so nothing here needs HTTP `Range` support.
`PAGES_SF=none` makes it 36MB.

`make atmos` fetches free software from the FreeDOS 1.3 repositories —
**Open Cubic Player**, **Arachne**, **FreeDoom**, **GNU Chess**, the
**Crynwr** packet drivers, open ARJ and gzip — and lays it into the demo
share, the browser image and `make demo`. It is period-correct in the way
that matters: Open Cubic Player is the MOD player people used, Arachne is
the DOS web browser, and crynwr is the packet-driver collection this client
talks to.

No test needs it; the rigs use generated filler. What it buys is that the
share and browse doors have real software in them, and the files work if
you download them. Nothing here is abandonware or a cracked game: the
shareware episodes were licensed for redistribution and the full games
never were, and a 1994 costume does not need anything taken to wear it.

Minimal configuration — everything else has a default and a row in the
settings door:

```ini
hub        = adc.example.org
nick       = Warlord
share      = C:\DL
rows       = 25        ; or 50
music      = yes
```

See [`docs/TOOLCHAIN.md`](docs/TOOLCHAIN.md) for DJGPP and Watt-32,
[`docs/DOSBOX.md`](docs/DOSBOX.md) for the emulator rig,
[`docs/V86.md`](docs/V86.md) for the browser host,
[`docs/PAGES.md`](docs/PAGES.md) for publishing the browser demo,
[`docs/PKTDRV.md`](docs/PKTDRV.md) for which NIC needs which driver, and
[`docs/AO486.md`](docs/AO486.md) for the FPGA checklist.

## Licence

DOSy++ is **MIT** licensed — see [`LICENSE`](LICENSE). Everything it
bundles belongs to somebody else and keeps its own terms, listed in
`CREDITS.txt` and explained in [`docs/LICENSING.md`](docs/LICENSING.md).

## What is bundled

Everything the demo ships is **free software** — not shareware, not
abandonware, not a commercial game. The Doom you can download is the GPL
*Boom* engine with **Freedoom**'s BSD-licensed game data, and the rest
comes from the FreeDOS 1.3 repositories, each download pinned by sha256.

`CREDITS.txt` is generated at build time from each package's own
`APPINFO/*.LSM` licence line, published beside the page, linked from it,
and announced by the hub in the welcome message. `make test-pages` fails
if the demo serves a file that is not named in it.
[`docs/LICENSING.md`](docs/LICENSING.md) sets out each claim and what
backs it.

The 1994 styling is a costume. The invented file names in the demo hub
are invented so that no real release is implied.

If you believe something bundled here should not be, write to
**dosy@fastmail.com** or open an issue, and say which file and why. It comes
out first and gets discussed afterwards.

## How it is verified

Nothing here asserts that a feature exists; each of these measures
something and fails loudly. 10,384 unit checks and every rig below, all
passing as of this commit.

Each target builds what it needs, including the DOS binary (in the DJGPP
container if this machine has no local toolchain), the FreeDOS images and
the generated modules — so any line here works from a fresh clone. What it
cannot build it names: `make uhub-image`, `make djgpp-image`, or the one
`brew install` that is missing.

**On the host, needing nothing but a compiler and Python:**

| | |
|---|---|
| `make test` | protocol, hashing, share index, browse, segments, UI layout, the MOD mixer |
| `make test-all` | the above plus every rig below in this group |
| `make test-transfer` | move a file between two clients, check the hash |
| `make test-browse` | partial lists one directory at a time, and the fallback for a peer that refuses them |
| `make test-resume` | finish a half-downloaded file; only the missing part moves |
| `make test-segments` | one file, two sources, parts verified apart |
| `make test-music` | the client renders the tune; the bytes are compared with the mixer driven directly |
| `make test-commands` | the `/`-commands that change something, including that a dial to a black hole keeps the music playing |
| `make test-setup` `test-hublist` `test-favs` `test-hash-mode` | the doors really write files, fetch lists and switch hubs |
| `make test-attic` | the demo hub's share index, and a download from a peer that refuses leaf hashes — which used to be fatal |
| `make test-bigshare` | a 5GB share counted, hashed, advertised and read back by a second client |
| `make screens` | every door, against a staged session — `docs/SCREENS.md` |
| `make test-oracle` | randomised TTH cross-check against `rhash` |
| `make bench` | TTH, search and mixer throughput |

**Against other people's implementations** (needs Docker):

| | |
|---|---|
| `make test-uhub` | five conformance cases against uhub, an independent C hub |
| `make test-adcs` | a TLS-only uhub reached through the relay, with certificate verification checked |
| `make test-airdcpp` | AirDC++w 2.14 on the other end: search, browse, partial lists, download, upload, leaf hashes, every transfer hash-checked |

**On real DOS, headlessly, under DOSBox-X:**

| | |
|---|---|
| `make dos-test-run` | the hash and protocol-text tests, compiled by DJGPP and run on DOS — 64-bit sizes where `long` is 32 bits |
| `make dos-net-run` | log into a hub over the packet driver |
| `make dos-share-check` | scan and hash a share |
| `make dos-xfer-check` | download a file and verify its TTH (`XFER_SIZE=100000000` for the sustained gate) |
| `make dos-browse-check` | three partial listings fetched, decompressed and walked inside 16MB |
| `make dos-hublist-check` | fetch a hub list over HTTP through Watt-32 |
| `make dos-music-check` | the Sound Blaster: BLASTER, the DSP reset, a DMA ring that keeps moving, and the music keeping up while a share is hashed |
| `make dos-midi-check` | premium game audio: MIDI notes written to an MPU-401, rendered by FluidSynth from a SoundFont, and **measured** — two chords and a gap, against a control run with the interface taken away |
| `make dos-vga-check` | read video memory back and compare every cell |
| `make dos-rows-check` | the same at 80×50, which is the only way to tell the font load happened |
| `make dos-kbd-check` | the int 9h handler, via scancodes injected through the controller |
| `make dos-bigshare-check` | a 5GB share advertised correctly where `long` is 32 bits — sparse files, and the hashes handed over in a translated cache |
| `make dos-bench-run` | hashing, search and the mixer at 486DX2-66 speed |

**In a browser engine, from Node:**

| | |
|---|---|
| `make test-relay` | the Wisp relay alone: streams, refusals, headers, teardown |
| `make test-v86` | boot FreeDOS in v86 and log into a hub through the relay |
| `make test-v86-xfer` | a file leaves the browser tab, hash checked |
| `make test-v86-dl` | the demo's own claim: search a hub, dial a peer through a WebSocket, download a real game, verify it against the hub's hash, unpack its executable |
| `make test-v86-state` | save a session, restore it into a fresh emulator, carry on with the same identity |
| `make test-v86-sound` | the emulated Sound Blaster: the card found, its ring programmed, and the mixer feeding it at 22,050 frames a second |
| `make test-v86-midi` | game MIDI: a DOS program writes to the MPU-401 v86 throws away, every byte is recovered outside the engine, and the page's synthesiser renders the chords — measured, not asserted |
| `make test-v86-doom` | a real game's soundtrack: BOOM writes 2,430 MIDI messages in 75s and they render to 60s of continuous music |
| `make test-pages` | …and the page's own modules, which no emulator exercises: the relay URL scheme, the MIDI stream reassembly, and the Web MIDI sink (stubbed browser) |
| `make test-v86-hublist` | the demo's hub-list door shows its own hub, and the guest dials only the addresses mapped for it — no public hub-list service, no third-party hub |
| `make test-pages` | the published bundle, served over HTTP with Range: every referenced file present, no cross-origin isolation needed, `relay/demo.sh` run from it, and the game downloaded into the bundle's own disk image |

One fetched dependency: an ADC file list is `files.xml.bz2`, so browsing
needs a bzip2 implementation. `make bzip2` pulls libbzip2's library sources
into `third_party/bzip2/` (sha256-checked), and the build does it on its own
if they are missing. It is not committed, for the same reason Watt-32 and
the packet drivers are not.

## Layout

```
src/adc/      protocol: framing, escaping, parsing, identity,
              hub state machine, user list
src/net/      transport vtable, stack bringup, a small HTTP client
src/hash/     Tiger, TTH, base32
src/share/    share index, scanner, hash cache, file lists,
              bzip2 wrapper, XML pull parser, browsing
src/xfer/     download queue, uploads
src/snd/      PC speaker, MOD loader and mixer, Sound Blaster / WAV output
src/ui/       cell buffer, row layout, VGA/terminal backends, widgets,
              doors
test/host/    unit tests and host rigs      test/dos/   DOSBox-X rigs
test/hub/     uhub and ADCS rigs            test/v86/   browser rigs
tools/        stub hub, Wisp relay, TLS relay, image builders,
              DJGPP container, the module writer
art/          the tune, the test tone and the effect fixture (make modules)
web/          v86 browser host page
third_party/  libbzip2, fetched -- not committed
```

Everything in `src/adc/`, `src/hash/` and `src/share/` is pure C with no DOS
dependency, so it builds and unit-tests natively. That is where almost all
the bugs will be, and debugging them inside an emulator is how this project
would die.

## Targets

| Host | Status |
|---|---|
| Real 386/486 + ISA NIC | primary target; nobody has run it yet |
| DOSBox-X | **running** — login, share, browse, a hash-verified 100MB download, the Sound Blaster, 80×25 and 80×50 |
| Browser (v86 + Wisp relay) | **running** — login, a hash-verified transfer out of the tab, and session persistence |
| FPGA 486 (ao486/MiSTer, NE2000 core) | no hardware here; the checklist and its gates are in [`docs/AO486.md`](docs/AO486.md) |

Both emulated rigs boot DOSy++ and run a real ADC session over a real
packet driver. `make pktdrv` fetches the drivers.

## Non-goals

NMDC (`dchub://`), IPv6, UPnP, DHT. Native TLS is deferred — ADCS hubs are
reached through a [TLS relay](docs/STUNNEL.md), which is tested rather than
assumed.

## Constraints that are not negotiable

- **No floating point.** Fixed-point throughout; `-msoft-float` makes a
  stray `double` a link error. Some 486 cores are SX-class.
- **Stream, never buffer.** `files.xml.bz2` in both directions, partial
  lists for browsing, and leaf hashes written to a file rather than held:
  a well-shared peer's list is tens of MB and the machine may have 16.
- **8.3 filenames are real.** `lfn=false` in the emulator on purpose; long
  names live in `DOSYPP.NAM`.
- **Measure, do not assume.** Every number in this file came out of a rig
  in the repo, and the rigs are written to fail rather than to pass.

See [`IMPLEMENTATION_PLAN.md`](IMPLEMENTATION_PLAN.md) for the design and
the reasoning, and [`TODO.md`](TODO.md) for what is left, including what is
known to be wrong.

## Contributing

[`CONTRIBUTING.md`](CONTRIBUTING.md) covers the build, the two C dialects
`src/` has to satisfy, and how the rigs are expected to be written. Bug
reports from real hardware are especially welcome.

A file that should not be bundled, or a memory-safety bug reachable from
hub or peer traffic, goes to the address in
[`SECURITY.md`](SECURITY.md) — **dosy@fastmail.com**.
