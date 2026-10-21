# DOSy++ - dual-target Makefile.
#
#   make host        native build of the DOS-independent modules (default)
#   make test        build and run the host unit tests
#   make dos         DOSYPP.EXE via the DJGPP cross-compiler
#   make clean
#
# The host target exists because almost every bug in this project will be in
# protocol, hashing or indexing code, and debugging those inside DOSBox is
# slow enough to change how much gets built.

# ---------------------------------------------------------------- toolchains

# The compiler triple, NOT an install path. Deliberately not called
# DJGPP_PREFIX: build-djgpp and Watt-32 both use that name for different
# things (an install path and a triple respectively), and an exported
# DJGPP_PREFIX would silently override a `?=` here.
DJGPP_TRIPLE ?= i586-pc-msdosdjgpp
DJGPP_CC     := $(DJGPP_TRIPLE)-gcc
WATT32_ROOT  ?= $(HOME)/djgpp/watt32

HOST_CC ?= cc

# Warnings we want everywhere. -Wall -Wextra on both toolchains; DJGPP's gcc
# is old enough that newer flags must not leak into the DOS build.
# Every snprintf truncation in this project is deliberate: an 80-column
# screen clamps a 96-byte config string or a 251-byte chat line by design,
# and snprintf always NUL-terminates. Warning about it just trains people to
# ignore warnings.
WARN := -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation

# No floating point anywhere in DOSy++ (some 486 cores are SX-class and FPGA
# cores may have incomplete x87). -msoft-float turns a stray
# double into a link error instead of a trap on real hardware.
DOS_CFLAGS  := -O2 $(WARN) -std=gnu89 -march=i386 -msoft-float \
               -DDOSYPP_WATT32 -I src -I $(WATT32_ROOT)/inc
DOS_LDFLAGS := -L $(WATT32_ROOT)/lib
DOS_LIBS    := -lwatt

HOST_CFLAGS := -O1 -g $(WARN) -std=c99 -I src

# Header dependencies. Make has no idea that a .c includes a .h, so without
# this, changing a struct in a header rebuilds nothing that uses it. That is
# not a tidiness issue: adding three fields to adc_user left half the objects
# compiled against the old layout, in one binary, and the symptom was an
# active peer reading as passive. -MP keeps a deleted header from wedging the
# build.
DEPFLAGS := -MMD -MP
HOST_LIBS   :=

BUILD := build

# ------------------------------------------------------------------- sources

# Portable: compiles for both targets, unit-testable natively.
PORTABLE_SRC := \
	src/adc/tokenize.c \
	src/ui/screen.c \
	src/ui/layout.c \
	src/ui/ansi.c \
	src/ui/art.c \
	src/ui/cp437.c \
	src/ui/theme.c \
	src/ui/widget.c \
	src/adc/ident.c \
	src/adc/userlist.c \
	src/adc/hubconn.c \
	src/adc/search.c \
	src/adc/hublist.c \
	src/adc/favs.c \
	src/adc/peerconn.c \
	src/xfer/queue.c \
	src/share/index.c \
	src/share/hashq.c \
	src/share/filelist.c \
	src/share/namemap.c \
	src/share/bz.c \
	src/share/tthlcache.c \
	src/snd/mod.c \
	src/snd/mus.c \
	src/share/xmlpull.c \
	src/share/browse.c \
	src/hash/tiger.c \
	src/hash/base32.c \
	src/hash/tth.c

# libbzip2, fetched into third_party/ by tools/fetchbzip2.sh. An ADC file
# list is files.xml.bz2 and every client expects real bzip2, so browse needs
# this in both directions. Built with warnings off: upstream is clean for its
# own flags, and patching someone else's warnings is how you acquire a fork.
BZ_SRC := \
	third_party/bzip2/blocksort.c \
	third_party/bzip2/huffman.c \
	third_party/bzip2/crctable.c \
	third_party/bzip2/randtable.c \
	third_party/bzip2/compress.c \
	third_party/bzip2/decompress.c \
	third_party/bzip2/bzlib.c

# BZ_NO_STDIO removes libbzip2's own FILE* layer, which we do not use, and
# with it the only floating point in the library (its "ratio %5.2f" progress
# lines). DJGPP has no soft-float library, so with -msoft-float those few
# printf arguments break the link. See src/share/bz.c.
BZ_INC := -I third_party/bzip2 -DBZ_NO_STDIO

# Platform-dependent but present in both builds (different #ifdef branches).
PLATFORM_SRC := \
	src/net/netinit.c \
	src/net/tp_plain.c \
	src/net/http.c \
	src/share/scan.c \
	src/snd/pcspkr.c \
	src/snd/sndout.c \
	src/ui/vga.c \
	src/ui/kbd.c

APP_SRC := \
	src/main.c \
	src/cfg.c \
	src/ui/app.c

# One binary per test file. All of them build for the host; test_hash also
# builds for DOS (see dos-test) because Tiger's 64-bit arithmetic and TTH's
# byte ordering are exactly the things a different compiler and target can
# break without any warning.
TEST_SRC := \
	test/host/test_tokenize.c \
	test/host/test_hash.c \
	test/host/test_hub.c \
	test/host/test_ui.c \
	test/host/test_share.c \
	test/host/test_peer.c \
	test/host/test_browse.c \
	test/host/test_seg.c \
	test/host/test_hublist.c \
	test/host/test_favs.c \
	test/host/test_mod.c

HOST_OBJ := $(patsubst %.c,$(BUILD)/host/%.o,$(PORTABLE_SRC) $(PLATFORM_SRC) $(APP_SRC) $(BZ_SRC))
DOS_OBJ  := $(patsubst %.c,$(BUILD)/dos/%.o,$(PORTABLE_SRC) $(PLATFORM_SRC) $(APP_SRC) $(BZ_SRC))

TEST_BINS := $(patsubst test/host/%.c,$(BUILD)/host/%,$(TEST_SRC))

# --------------------------------------------------------------------- rules

.PHONY: all host dos dos-small dos-test dos-test-run test test-relay \
        test-oracle test-transfer test-hash-mode test-browse test-resume \
        test-rescan test-airdcpp test-segments test-setup test-hublist \
        test-favs \
        test-all clean toolchain-check hub dos-docker web images \
        relay dosfiles dos-bench dos-bench-run bench pktdrv dos-net-run \
        test-v86 test-v86-xfer test-v86-state test-v86-sound test-uhub test-adcs \
        dos-any dos-test-any dos-bench-any stage-dos dos-ui-run dos-vga-check dos-kbd-check \
        dos-share-check dos-xfer-check dos-browse-check \
        dos-hublist-check dos-music-check dos-rows-check \
        screenshot screens art modules atmos demo demo-dos demo-web demo-stop bzip2 \
        tthof warez pages test-pages test-v86-dl test-attic \
        sf2 dos-midi-check dos-midi-any synth test-v86-midi test-selfbrowse \
        test-v86-doom test-v86-hublist test-v86-inpage test-v86-play demo-pages \
        test-music test-commands test-bigshare dos-bigshare-check \
        sweep

all: host

host: $(BUILD)/host/dosypp
	@echo "built $< (native)"

# The TTH of a file, from our own implementation. Used by tools/mkshare.sh
# to write the demo hub's SHARE.TXT, so that what the hub advertises is
# what the client will compute after downloading.
tthof: $(BUILD)/host/tthof
	@echo "built $<"

$(BUILD)/host/dosypp: $(HOST_OBJ)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $^ $(HOST_LIBS)

$(BUILD)/host/third_party/%.o: third_party/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -O2 -w $(BZ_INC) $(DEPFLAGS) -c -o $@ $<

$(BUILD)/host/%.o: %.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) $(BZ_INC) $(DEPFLAGS) -c -o $@ $<

# ---- unit tests

# The shipped tune and the test tone, written rather than committed: see
# tools/makemod.py for why (licensing, and they are test fixtures whose
# every note has to be known).
MODULES := art/DOSYPP.MOD art/TESTTONE.MOD art/FXTEST.MOD

modules: $(MODULES)

# mkdir first: art/ holds nothing but generated modules, so a fresh clone
# does not have the directory at all and makemod.py cannot write into it.
# `make test` failed on a clean checkout for want of these three lines --
# found by running the host tier in a fresh clone, which is what CI does
# on every commit.
art/DOSYPP.MOD: tools/makemod.py
	@mkdir -p $(dir $@)
	@./tools/makemod.py $@

art/TESTTONE.MOD: tools/makemod.py
	@mkdir -p $(dir $@)
	@./tools/makemod.py --test $@

art/FXTEST.MOD: tools/makemod.py
	@mkdir -p $(dir $@)
	@./tools/makemod.py --fx $@

test: $(TEST_BINS) $(MODULES)
	@fail=0; for t in $(TEST_BINS); do \
		echo "--- $$t"; $$t || fail=1; \
	done; exit $$fail

# Tests link PLATFORM_SRC as well as PORTABLE_SRC: hubconn uses the
# transport helpers in net/, which are #ifdef'd and build natively.
TEST_OBJ := $(patsubst %.c,$(BUILD)/host/%.o,$(PORTABLE_SRC) $(PLATFORM_SRC) $(BZ_SRC))

$(BUILD)/host/%: test/host/%.c $(TEST_OBJ)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $< $(TEST_OBJ) $(HOST_LIBS)

# Randomised cross-check against an independent implementation (rhash).
# Not part of `make test` because it needs rhash installed.
test-oracle: host
	./test/host/crosscheck_rhash.sh

# ---- DOS

dos: toolchain-check $(BUILD)/dos/DOSYPP.EXE
	@echo "built $(BUILD)/dos/DOSYPP.EXE"

# Symbols are kept in the normal build: symify is the only way to turn a DOS
# crash address back into a function name, and there is no debugger. This
# target is for when size matters (a floppy, a slow load off real hardware) --
# roughly 760KB stripped versus 1.6MB.
dos-small: dos
	cp $(BUILD)/dos/DOSYPP.EXE $(BUILD)/dos/DOSYPP-S.EXE
	$(DJGPP_TRIPLE)-strip $(BUILD)/dos/DOSYPP-S.EXE
	@ls -l $(BUILD)/dos/DOSYPP.EXE $(BUILD)/dos/DOSYPP-S.EXE

# rm first: the output name is upper case, a case-insensitive host stores
# whatever case created the file, and the DJGPP container then cannot create
# DOSYPP.EXE over a DOSYPP.exe it can see but not match. That shows up as
# "ld: cannot open output file ... No such file or directory" for a
# directory that plainly exists, on every other build.
$(BUILD)/dos/DOSYPP.EXE: $(DOS_OBJ)
	@mkdir -p $(dir $@)
	@rm -f $@
	$(DJGPP_CC) $(DOS_CFLAGS) $(DOS_LDFLAGS) -o $@ $^ $(DOS_LIBS)

# The hash tests, built for DOS. This is the other half of the Phase 1 gate:
# passing on the host proves nothing about i386 + gnu89 + a 2022 GCC.
dos-test: toolchain-check $(BUILD)/dos/HASHTEST.EXE $(BUILD)/dos/TOKTEST.EXE
	@echo "built HASHTEST.EXE and TOKTEST.EXE in $(BUILD)/dos -- run them with: make dos-test-run"

DOS_TEST_OBJ := $(patsubst %.c,$(BUILD)/dos/%.o,$(PORTABLE_SRC) $(PLATFORM_SRC) $(BZ_SRC))

# The protocol's text handling, on the target. 64-bit sizes printed and
# parsed where `long` is 32 bits is exactly the condition the share-size
# bug needed, so the test that covers it belongs here and not only on the
# host.
$(BUILD)/dos/TOKTEST.EXE: test/host/test_tokenize.c $(DOS_TEST_OBJ)
	@mkdir -p $(dir $@)
	@rm -f $@
	$(DJGPP_CC) $(DOS_CFLAGS) $(DOS_LDFLAGS) -o $@ $< $(DOS_TEST_OBJ) $(DOS_LIBS)

$(BUILD)/dos/HASHTEST.EXE: test/host/test_hash.c $(DOS_TEST_OBJ)
	@mkdir -p $(dir $@)
	@rm -f $@
	$(DJGPP_CC) $(DOS_CFLAGS) $(DOS_LDFLAGS) -o $@ $< $(DOS_TEST_OBJ) $(DOS_LIBS)

# The MPU-401, which is a serial port and not an instrument: this writes
# MIDI notes at it and something else makes the sound. Standalone -- it
# needs none of the client, which is the point: it tests the interface and
# whatever is listening, not us.
$(BUILD)/dos/MIDITEST.EXE: test/dos/miditest.c
	@mkdir -p $(dir $@)
	@rm -f $@
	$(DJGPP_CC) $(DOS_CFLAGS) -o $@ $<

# Premium game audio, measured: a DOS program drives the MPU-401, DOSBox-X
# renders it through FluidSynth and a SoundFont, and the rig measures the
# audio that comes out. Needs the SoundFont fetched.
dos-midi-check: dos-midi-any sf2
	./test/dos/run-dosbox-midi.sh

sf2:
	./tools/fetchsf2.sh

# Run the DOS hash tests under DOSBox-X and report the result on the host.
dos-test-run: dos-test-any
	./test/dos/run-dosbox-tests.sh
	./test/dos/run-dosbox-tests.sh --tokens

# TTH throughput. The DOS build must be run cycle-limited to mean anything.
dos-bench: toolchain-check $(BUILD)/dos/HASHBENC.EXE $(BUILD)/dos/SRCHBENC.EXE \
           $(BUILD)/dos/MUSBENCH.EXE
	@echo "built HASHBENC.EXE, SRCHBENC.EXE and MUSBENCH.EXE in $(BUILD)/dos -- run them with: make dos-bench-run"

$(BUILD)/dos/HASHBENC.EXE: test/dos/hashbench.c $(DOS_TEST_OBJ)
	@mkdir -p $(dir $@)
	@rm -f $@
	$(DJGPP_CC) $(DOS_CFLAGS) $(DOS_LDFLAGS) -o $@ $< $(DOS_TEST_OBJ) $(DOS_LIBS)

dos-bench-run: dos-bench-any $(MODULES)
	./test/dos/run-dosbox-tests.sh --bench
	./test/dos/run-dosbox-tests.sh --searchbench
	./test/dos/run-dosbox-tests.sh --musbench

$(BUILD)/host/searchbench: test/dos/searchbench.c $(TEST_OBJ)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) -O2 -o $@ $< $(TEST_OBJ)

$(BUILD)/dos/SRCHBENC.EXE: test/dos/searchbench.c $(DOS_TEST_OBJ)
	@mkdir -p $(dir $@)
	@rm -f $@
	$(DJGPP_CC) $(DOS_CFLAGS) $(DOS_LDFLAGS) -o $@ $< $(DOS_TEST_OBJ) $(DOS_LIBS)

bench: $(BUILD)/host/hashbench $(BUILD)/host/searchbench \
       $(BUILD)/host/musbench $(MODULES)
	@$(BUILD)/host/hashbench
	@echo
	@$(BUILD)/host/searchbench
	@echo
	@$(BUILD)/host/musbench art/DOSYPP.MOD art/FXTEST.MOD

$(BUILD)/host/musbench: test/dos/musbench.c $(TEST_OBJ)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) -O2 -o $@ $< $(TEST_OBJ)

$(BUILD)/dos/MUSBENCH.EXE: test/dos/musbench.c $(DOS_TEST_OBJ)
	@mkdir -p $(dir $@)
	@rm -f $@
	$(DJGPP_CC) $(DOS_CFLAGS) $(DOS_LDFLAGS) -o $@ $< $(DOS_TEST_OBJ) $(DOS_LIBS)

$(BUILD)/host/hashbench: test/dos/hashbench.c $(TEST_OBJ)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) -O2 -o $@ $< $(TEST_OBJ)

$(BUILD)/dos/third_party/%.o: third_party/%.c
	@mkdir -p $(dir $@)
	$(DJGPP_CC) -O2 -w -march=i386 -msoft-float $(BZ_INC) $(DEPFLAGS) -c -o $@ $<

$(BUILD)/dos/%.o: %.c
	@mkdir -p $(dir $@)
	$(DJGPP_CC) $(DOS_CFLAGS) $(BZ_INC) $(DEPFLAGS) -c -o $@ $<

toolchain-check:
	@command -v $(DJGPP_CC) >/dev/null 2>&1 || { \
		echo "error: $(DJGPP_CC) not found in PATH."; \
		echo "       see docs/TOOLCHAIN.md"; exit 1; }
	@test -f "$(WATT32_ROOT)/lib/libwatt.a" || { \
		echo "error: Watt-32 not found at $(WATT32_ROOT)"; \
		echo "       set WATT32_ROOT, or see docs/TOOLCHAIN.md"; exit 1; }

# DJGPP in a container: GCC 12.2's build system does not survive clang 21 plus
# a current macOS SDK. See tools/docker/Dockerfile.djgpp.
dos-docker:
	./tools/dosmake.sh make dos

# ------------------------------------------------- prerequisites for rigs
#
# Every rig below builds what it needs. This is not tidiness: a rig that
# fails because a binary was not staged has spent your time to tell you
# nothing, and the one that did it here (dos-bench-run, for want of three
# benchmark binaries) failed after the sweep had already reported the
# tree green.
#
# The DOS binary, however this machine can build it: the local toolchain
# when there is one, the container when there is not. Separate from `dos`
# because `dos` insists on the toolchain and says so, which is right when
# you asked for it by name and wrong as a side effect of running a test.
# The container is the slowest thing in this repository and the quietest:
# on a machine with no local DJGPP, this used to call Docker every single
# time, even when the binary was already built and nothing had changed.
# `make pages && make demo-pages` therefore reached for Docker twice, and
# if Docker was stopped or still starting, it sat there with no output --
# which is what "it hangs" turned out to be.
#
# So: do what make would do, and only build when something is newer than
# the binary.
#
# Against src/ and third_party/ and NOT the Makefile, which matters: the
# DOS rules do not list the Makefile as a prerequisite, so a Makefile edit
# made this think a rebuild was due, the rebuild then found nothing to do,
# the binary's timestamp did not move -- and it asked Docker again on every
# single invocation afterwards. Checking what `make dos` itself checks is
# the only version of this that settles.
dos-any:
	@if test -f "$(BUILD)/dos/DOSYPP.EXE" && \
	   test -z "`find src third_party -newer $(BUILD)/dos/DOSYPP.EXE 2>/dev/null | head -1`"; then \
		echo "DOSYPP.EXE is up to date (nothing in src/ is newer)"; \
	elif command -v $(DJGPP_CC) >/dev/null 2>&1 && \
	   test -f "$(WATT32_ROOT)/lib/libwatt.a"; then \
		$(MAKE) --no-print-directory dos; \
	else \
		echo "no local DJGPP: building DOSYPP.EXE in the container"; \
		./tools/dosmake.sh make dos; \
	fi

dos-test-any:
	@if command -v $(DJGPP_CC) >/dev/null 2>&1 && \
	   test -f "$(WATT32_ROOT)/lib/libwatt.a"; then \
		$(MAKE) --no-print-directory dos-test; \
	else \
		./tools/dosmake.sh make dos-test; \
	fi

dos-bench-any:
	@if command -v $(DJGPP_CC) >/dev/null 2>&1 && \
	   test -f "$(WATT32_ROOT)/lib/libwatt.a"; then \
		$(MAKE) --no-print-directory dos-bench; \
	else \
		./tools/dosmake.sh make dos-bench; \
	fi

dos-midi-any:
	@if command -v $(DJGPP_CC) >/dev/null 2>&1; then \
		$(MAKE) --no-print-directory $(BUILD)/dos/MIDITEST.EXE; \
	else \
		./tools/dosmake.sh make $(BUILD)/dos/MIDITEST.EXE; \
	fi

# DOSYPP.EXE where the DOSBox-X rigs look for it.
#
# Nothing used to do this copy: the build wrote $(BUILD)/dos and the rigs
# read dosbox-x/c, so the two were joined by a human remembering. The
# fetched files are not ours to build, so a missing one is reported here
# rather than inside an emulator thirty seconds later.
stage-dos: dos-any
	@mkdir -p dosbox-x/c
	@cp $(BUILD)/dos/DOSYPP.EXE dosbox-x/c/DOSYPP.EXE
	@echo "staged dosbox-x/c/DOSYPP.EXE ($$(wc -c < dosbox-x/c/DOSYPP.EXE | tr -d ' ') bytes)"
	@missing=; \
	for f in CWSDPMI.EXE NE2000.COM WATTCP.CFG; do \
		test -f dosbox-x/c/$$f || missing="$$missing $$f"; \
	done; \
	if [ -n "$$missing" ]; then \
		echo "missing from dosbox-x/c:$$missing"; \
		echo "  run: make dosfiles   (and make pktdrv for PCIPKT.COM)"; \
		exit 1; \
	fi

# ---- v86 browser host

web: images
	@echo "start the relay with 'make relay', then open http://127.0.0.1:8086/"

$(BUILD)/.v86-fetched:
	./tools/fetchv86.sh
	@mkdir -p $(BUILD) && touch $@

# The data image carries DOSYPP.EXE, so it depends on having one: an image
# built from a stale binary is the same failure as a stale binary.
images: $(BUILD)/.v86-fetched dos-any
	./tools/mkv86img.sh

relay:
	./tools/wisprelay.py

# Real DOS software for the demo share: free packages from the FreeDOS
# repositories, fetched rather than committed. Not needed by any test --
# the rigs use generated filler -- but it is what somebody looking at the
# share door sees.
atmos:
	./tools/fetchatmos.sh

# ---- images this repo does not build for you
#
# A Docker image takes minutes to build and pulls from the network, so
# these are targets you ask for rather than side effects of running a
# test. The rigs that need them name the target in their error, which is
# the part that was missing: "image dosypp-uhub missing" is only helpful
# if it also says how to get one.

uhub-image:
	docker build -f tools/docker/Dockerfile.uhub -t dosypp-uhub .

djgpp-image:
	docker build -f tools/docker/Dockerfile.djgpp -t dosypp-djgpp .

# AirDC++ is pulled rather than built; the rig does it on its own if the
# image is absent, which is why there is no target for it here.

# ---- convenience

hub:
	./tools/stubhub.py

# CWSDPMI and friends into dosbox-x/c/
dosfiles:
	./tools/fetchdos.sh

# libbzip2 sources into third_party/bzip2/
bzip2:
	./tools/fetchbzip2.sh

third_party/bzip2/bzlib.h:
	./tools/fetchbzip2.sh

$(BZ_SRC): third_party/bzip2/bzlib.h

# And so does the one file of ours that includes it. Without this, whether
# the fetch runs before bz.c compiles is down to the order make happens to
# walk a link's prerequisites -- and on a machine whose SDK ships its own
# bzlib.h (macOS does) the mistake is invisible, because the compile
# quietly uses that one instead of the pinned 1.0.8 sources. On a runner
# with no system copy, every gate fails at the first object.
$(BUILD)/host/src/share/bz.o $(BUILD)/dos/src/share/bz.o: \
	third_party/bzip2/bzlib.h

# Packet drivers into dosbox-x/c/
pktdrv:
	./tools/fetchpktdrv.sh

# Boot DOSy++ on DOS for real: packet driver, Watt-32 DHCP, live handshake.
dos-net-run: stage-dos
	./test/dos/run-dosbox-net.sh

# Conformance against uhub, an independent ADC implementation. This is the
# test that proves the protocol work; the stub hub only proves consistency.
test-uhub: host
	./test/hub/run-uhub.sh

# ADCS, end to end: a TLS-only uhub, tools/tlsrelay.py in front of it, and
# a login through the relay. Needs Docker and the uhub image.
test-adcs: host
	./test/hub/run-adcs.sh

# The BBS interface on real DOS. `dos-ui-run` brings back what it drew;
# `dos-vga-check` draws for real and reads 0xB8000 back to prove the
# renderer put the right bytes in video memory.
dos-ui-run: stage-dos
	./test/dos/run-dosbox-ui.sh $(DOOR)

# The 50-row mode on real DOS: the BIOS has to load the 8x8 font and report
# fifty rows, and the layout has to put the hotkey bar on the last one.
dos-rows-check: stage-dos
	./test/dos/run-dosbox-ui.sh 1 shot 50
	./test/dos/run-dosbox-ui.sh 1 vga 50

dos-vga-check: stage-dos
	./test/dos/run-dosbox-ui.sh 0 vga

# The int 9h handler, driven by scancodes injected through the keyboard
# controller so the whole path (controller -> IRQ1 -> ISR -> UI) is real.
dos-kbd-check: stage-dos
	./test/dos/run-dosbox-keys.sh

# Scan and hash a share on real DOS: FINDFIRST/FINDNEXT, 8.3 names, packed
# DOS timestamps in the cache, all inside 16MB.
dos-share-check: stage-dos
	./test/dos/run-dosbox-share.sh

# Screenshot the interface on the host, against a stub hub. DOOR=0|1|2|3
screenshot:
	@./tools/screenshot.sh $(DOOR)

# Every door, against a staged session with real software in the share:
# docs/screens/*.txt and docs/SCREENS.md. Takes a few minutes, most of it
# hashing.
screens: host $(MODULES)
	./tools/screenshots.sh

# Render the splash and sign-off screens. No hub needed -- this is for
# iterating on the art without quitting a live session to see it.
art: host
	@$(BUILD)/host/dosypp --art $(BUILD)/art.txt -c $(BUILD)/art.ini \
		-n Warlord >/dev/null && cat $(BUILD)/art.txt

# Try it. `demo` runs the UI in your terminal; `demo-dos` runs the real DOS
# binary in a DOSBox-X window. Both need an actual terminal.
demo:
	@./tools/demo.sh $(THEME)

demo-dos:
	@./tools/demo-dos.sh

# DOSy++ in a browser tab, via v86 and the Wisp relay.
demo-web:
	@./tools/demo-web.sh

# The published bundle, served locally with a hub behind it. No GitHub
# involved: this is the rehearsal of what a stranger would see.
#
# Deliberately NOT dependent on `pages`. Every target it would pull in is
# phony -- images, warez, atmos, the DOS binary -- so `make demo-pages`
# rebuilt all of them, container and all, even straight after `make
# pages`. Minutes of silence before the URL appears reads exactly like a
# hang, and it was one as far as anybody waiting was concerned. The script
# checks for the bundle and says `make pages` if it is not there.
demo-pages:
	@./tools/demo-pages.sh

# Does the browser demo's hub-list door show this demo's own hub, and does
# the guest reach anything nobody mapped for it?
test-v86-hublist: images
	./test/v86/run-hublist-test.sh

# The demo with no server at all: the relay, the hub and the serving peer
# are JavaScript in the page (web/wisp.mjs, web/adchub.mjs), so a visitor
# needs nothing but a browser. Nothing listens on a port during this run.
test-v86-inpage: images warez
	./test/v86/run-inpage-test.sh

# Stop anything a demo left running (and free the port).
demo-stop:
	@./tools/demo-stop.sh $(PORT)

# Boot the browser host headlessly in Node and verify a live handshake.
test-v86: images
	./test/v86/run-node-test.sh

# The browser host's transfer gate: a file leaves the tab through the Wisp
# relay and its hash is checked. test-v86 proves a handshake; this proves a
# client.
test-v86-xfer: host images
	./test/v86/run-xfer-test.sh

# Session persistence for the browser host: v86's own state round trip with
# DOS running inside it. The IndexedDB half is browser-only.
test-v86-state: images
	./test/v86/run-state-test.sh

# The browser demo's headline claim: search a hub, download a real game
# over ADC through a WebSocket, verify it against the hub's hash, unpack
# it. Needs the share built, which needs the packages fetched.
test-v86-dl: images warez
	./test/v86/run-dl-test.sh

# The share the demo hub serves, with its hashes precomputed. Depends on
# the packages being fetched, like every other rig prerequisite here: a
# gate that fails because somebody forgot a fetch has spent your time to
# tell you nothing. fetchatmos.sh is a no-op once they are cached.
warez: tthof atmos
	./tools/mkshare.sh

# What gets published to GitHub Pages: the page, the engine, the images,
# and the two scripts that give the browser a network. See docs/PAGES.md.
pages: images warez web/vendor/synth/spessasynth_core.js
	./tools/mkpages.sh

# The vendored synthesiser, as a file rather than a phony target: `make
# pages` has to be able to depend on it. (`make synth` above is the
# spelling to type.)
web/vendor/synth/spessasynth_core.js:
	./tools/fetchsynth.sh

test-pages: pages
	./test/web/run-pages-test.sh

# Game MIDI in the browser: v86's MPU-401 answers a game's probe and
# discards the notes, so the page taps the port and plays them itself.
# Checks the bytes come out intact and that the synthesiser renders them.
test-v86-midi: images synth sf2
	./test/v86/run-midi-test.sh

# A real game's music, out of the browser demo: BOOM (the GPL Doom engine,
# with the free WAD the FreeDOS package ships) writing its soundtrack to
# the MPU-401, tapped and rendered. Builds its own scratch image -- 19MB of
# unpacked Doom does not belong in a published one.
test-v86-doom: synth sf2 atmos $(BUILD)/.v86-fetched
	./test/v86/run-doom-test.sh

# Does a downloaded game actually start? Unpacking is not playing: DOS has
# no mouse driver unless one is loaded, and a game that wants int 33h just
# dies.
test-v86-play: images warez
	./test/v86/run-play-test.sh

# The SoundFont synthesiser the page plays game MIDI through.
synth:
	./tools/fetchsynth.sh

# The emulated Sound Blaster: does the browser demo have music? v86 has an
# SB16 but no OPL, so this is the PCM path -- which is the one this client
# uses.
test-v86-sound: images
	./test/v86/run-sound-test.sh

test-relay: host
	./test/host/test_wisprelay.py

# Browsing your own share, which used to do nothing at all.
test-selfbrowse: host
	./test/host/run-selfbrowse-test.sh

# The demo hub's serving node: the share index, which decides what the hub
# advertises and therefore what a client will check its download against.
test-attic: host warez
	./test/host/test_attic.py
	./test/host/run-attic-test.sh

# Move a real file between two clients and verify it by hash.
test-transfer: host
	./test/host/run-transfer-test.sh

# Deferred hashing: that "manual" really does no work, and that H starts it.
test-hash-mode: host
	./test/host/run-hashmode-test.sh

# Browsing: files.xml -> bzip2 -> CGET -> parse -> a tree you can walk,
# ending in a file queued from the browse door and downloaded.
test-browse: host
	./test/host/run-browse-test.sh

# Resume: half a file on disk plus a queue file, and only the missing part
# fetched -- with the finished file still verified against its hash.
test-resume: host
	./test/host/run-resume-test.sh

# The rescan timer. Takes about ninety seconds of real time, because the
# shortest interval is a minute and faking the clock would test the fake.
test-rescan: host
	./test/host/run-rescan-test.sh

# Segmented, multi-source downloading: two clients serving one file, parts
# verified individually against the Merkle tree's leaves.
test-segments: host
	./test/host/run-segment-test.sh

# The settings door: edits must reach DOSYPP.INI and come back.
test-setup: host
	./test/host/run-setup-test.sh

# The hub list: HTTP, bzip2, XML, and a session that moves to a hub from it.
test-hublist: host
	./test/host/run-hublist-test.sh

# Favourites, and the per-hub passwords that are the point of them: a hub
# the config file's password cannot get into.
test-favs: host
	./test/host/run-favs-test.sh

# The real-client gate: AirDC++ in a container, searching us, browsing us,
# downloading from us and serving us. Needs Docker, and on Apple silicon it
# runs an amd64 image under emulation -- a few minutes. Not in test-all for
# that reason, but it is the test that found the most.
test-airdcpp: host
	./test/host/run-airdcpp-test.sh

# The same thing on real DOS, over a packet driver: the DOS client is the
# passive downloader, because slirp cannot accept inbound connections.
dos-xfer-check: stage-dos
	./test/dos/run-dosbox-xfer.sh

# Browsing on real DOS. The point is bzip2 decompression inside 16MB, which
# the host build cannot tell you anything about.
dos-browse-check: stage-dos
	./test/dos/run-dosbox-browse.sh

# The hub list on real DOS: HTTP over Watt-32, which is code the host build
# exercises on a different stack entirely.
dos-hublist-check: stage-dos
	./test/dos/run-dosbox-hublist.sh

# The Sound Blaster: BLASTER, the DSP reset, and a DMA ring that keeps
# moving. Nothing else touches any of it -- the host build writes a WAV.
dos-music-check: stage-dos $(MODULES)
	./test/dos/run-dosbox-music.sh

# test-transfer is included: it is the only check that a file actually moves
# between two clients, and it needs nothing but the host build and rhash.
# The /-commands that change something: the two that have to reach the hub
# as an INF update, the one that moves hubs, and the one that deliberately
# waits for the next run.
test-commands: host $(MODULES)
	./test/host/run-commands-test.sh

# A share bigger than 4GB, advertised correctly. Sparse files, so 5GB
# costs 180KB of disk; the hashing takes a few minutes because the queue
# spends a fixed budget per loop pass.
test-bigshare: host
	./test/host/run-bigshare-test.sh

# The same on the target, where `long` is 32 bits: the hashes are computed
# on the host and handed to the guest, because 5GB on DOS is three hours.
dos-bigshare-check: host stage-dos
	./test/dos/run-dosbox-bigshare.sh

# The music, end to end: the client renders the shipped module and the bytes
# are compared with the mixer's own render of it. "A WAV appeared" would
# pass with silence in it.
test-music: host $(TEST_BINS) $(MODULES)
	./test/host/run-music-test.sh

# Every gate in the repository, in four tiers, with a report at the end.
# test-all is the host tier alone; this is the one to run before saying the
# tree is green.
sweep:
	./tools/sweep.sh

test-all: test test-relay test-transfer test-hash-mode test-browse \
          test-resume test-segments test-setup test-hublist test-favs \
          test-music test-commands test-bigshare dos-bigshare-check \
        sweep

clean:
	rm -rf $(BUILD)

# Generated by DEPFLAGS; absent on a clean tree, hence the leading dash.
# Listed from the object lists rather than found on disk: a $(shell find)
# here runs on every make invocation, and inside the build container it
# failed outright with "INTERNAL: readdir: Cannot allocate memory".
-include $(HOST_OBJ:.o=.d) $(DOS_OBJ:.o=.d) \
        $(patsubst %.c,$(BUILD)/host/%.d,$(TEST_SRC)) \
        $(patsubst %.c,$(BUILD)/dos/%.d,$(TEST_SRC))
