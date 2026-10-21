# DOSBox-X

Nothing here needs doing by hand any more: every `make dos-*-check` target
builds `DOSYPP.EXE` (locally or in the container), copies it into
`dosbox-x/c/`, and tells you which fetched file is missing if one is. The
rest of this document is what those steps do and why.

## The test rig

```sh
brew install dosbox-x          # verified with 2026.10.01, linked against
                               # both libslirp and libpcap
cd dosbox-x
dosbox-x -conf dosbox-x.conf
```

The profile mounts `./c` as `C:`, emulates an NE2000 ISA card at `io 0x300 /
irq 10`, and loads the packet driver from `AUTOEXEC`.

## What you must add by hand

Two files are not redistributable and are not in the repo:

| File | Where it goes | Source |
|---|---|---|
| `NE2000.COM` | `dosbox-x/c/` | Crynwr NE2000 packet driver — packetdriversdos.net, or the vogonsdrivers archive |
| `CWSDPMI.EXE` | `dosbox-x/c/` | Ships with DJGPP (`cwsdpmi/bin/`) |

`make dosfiles` fetches what it can and says what it cannot.

`DOSYPP.EXE` itself is staged for you: `make stage-dos` builds it and
copies it there, and every rig depends on that. The hand-copy this line
used to recommend is what went wrong often enough to be worth automating --
a rig running an hour-old binary looks exactly like a rig finding a bug.

## Choosing a network backend — this is the important decision

Set `backend=` in `[ne2000]`.

### `slirp` (the default here)

User-mode NAT. No privileges, works out of the box, survives Wi-Fi. The guest
gets `10.0.2.15` by DHCP and **the host is `10.0.2.2`** — so with the stub hub
running on your Mac:

```
C:\> dosypp 10.0.2.2
```

**Limitation that matters:** outbound only. Nothing on the outside can open a
connection *into* the guest, so DOSy++ is effectively a passive-mode client.
Chat, search and downloads from active peers work; active mode and serving an
upload cannot be tested.

That is enough for a verified download: `make dos-xfer-check` runs the DOS
client as the passive end, the host client as the active one, and checks the
TTH of what arrived. One thing it has to work around — worth knowing before
you build a rig of your own — is that a peer is dialled at the address the
*hub* observed. A host-side peer connects over loopback, and 127.0.0.1 inside
the guest is the guest, so the stub hub is told `--loopback-as 10.0.2.2`.

### `pcap`

Bridges the emulated NIC onto a real interface, so the guest is a first-class
host on your LAN and inbound connections work. **Required from Phase 6**, where
active mode, `DCTM` and upload slots have to be exercised in both directions.

Costs: needs `sudo` (raw packet access), does not work over most Wi-Fi
interfaces in bridged mode, and the guest consumes a DHCP lease on your real
network.

```sh
sudo dosbox-x -conf dosbox-x.conf -set "ne2000 backend=pcap" \
              -set "ne2000 realnic=en0"
```

List candidate interfaces with `dosbox-x -set "ne2000 realnic=list"`.

## Honest performance testing

`cycles=max` with `core=dynamic` is far faster than any real 486 and will give
you a flattering, useless picture of hashing and transfer speed. Before
believing any throughput number, re-run under a cycle-limited profile:

```sh
# roughly 486DX2-66 territory, with prefetch-queue accuracy
dosbox-x -conf dosbox-x.conf \
    -set "cpu core=normal" \
    -set "cpu cputype=486_prefetch" \
    -set "cpu cycles=25000"
```

`cputype=486_prefetch` only works with `core=normal` — DOSBox-X exits with
`prefetch queue emulation requires the normal core setting` otherwise, which is
why the base profile uses plain `cputype=486`.

TTH hashing is the thing to measure this way — see `IMPLEMENTATION_PLAN.md` §5.

## Premium game audio (MPU-401 → FluidSynth, or an MT-32)

```sh
make sf2                 # fetch a SoundFont, sha256-pinned (40MB)
cd dosbox-x && dosbox-x -conf dosbox-x.conf
make dos-midi-check      # and prove it makes a sound
```

An **MPU-401 is a serial port, not an instrument**. A DOS game that offers
you "Roland MPU-401" writes MIDI notes into ports 0x330/0x331 and something
else turns them into sound — in 1994 an MT-32 or an SC-55 on the end of a
cable, here FluidSynth, which this DOSBox-X links in. All of the quality
lives in the thing on the other end, which is why the SoundFont is the
decision that matters and `mpu401 = intelligent` is just plumbing.

The committed conf sets:

```ini
[midi]
mpu401          = intelligent
mididevice      = fluidsynth
fluid.soundfont = sf2/MuseScore_General.sf3
```

MuseScore General (MIT, 40MB, in the SC-55 tradition) is what
`tools/fetchsf2.sh` fetches and pins. Any SoundFont works:
`DOSYPP_SF2=~/FluidR3_GM.sf2 make dos-midi-check`, or edit the conf. On
macOS, `mididevice = coreaudio` needs no SoundFont at all and sounds
noticeably worse.

**Own MT-32 ROMs?** `mididevice = mt32` with `mt32.romdir = ./mt32` gives
you munt, which is the right sound for anything written before General
MIDI and the wrong one for anything after. The ROMs are not
redistributable, so nothing here fetches them.

### The thing worth knowing

**DOSBox-X's FluidSynth opens your sound card itself.** It does not go
through the emulator's mixer, so the mixer's volume does not touch MIDI and
neither does its wave capture.

That is not a detail: it is why `make dos-midi-check` exists in the shape
it does. Writing it produced a run where the MPU-401 answered its reset,
FluidSynth loaded all 40MB of SoundFont, the program played its notes, and
eleven seconds of captured audio were every-sample-zero. Nothing in any log
said anything was wrong, because nothing was — the audio was going
somewhere else entirely.

So the rig sets `fluid.driver = file`, which makes FluidSynth render to
`fluidsynth.wav` instead, and `test/dos/midirms.py` measures it: two
sustained chords of about 1,100 RMS with 1,250ms of quiet between them,
which is the pattern `test/dos/miditest.c` plays. Then it runs the whole
thing again with `mpu401 = none` and requires silence — because a
measurement of sound with nothing to compare it against would pass on a
startup beep.

What no rig here can check is the last hop, from FluidSynth to your
speakers. Leave `fluid.driver` unset (as the committed conf does) and that
is the hop you get; it needs ears.

## Deliberate configuration choices

- **`lfn = false`.** Long filename support is off on purpose. DOSy++ has to
  handle real 8.3 constraints and map UTF-8 ADC paths onto them (`namemap.c`).
  Enabling LFN would hide exactly the bug class we most need to find.
- **`memsize = 16`.** Matches a well-equipped period machine. If something
  only works at 64MB, that is a bug, not a requirement.
- **`cputype = 486_prefetch`.** Keeps the emulated CPU honest about the target,
  including no assumptions about newer instructions.

## Troubleshooting

**`TCP/IP init failed` from DOSy++** — the packet driver did not load. Check
`NE2000.COM` is in `C:\`, and that the `ne2000` parameters in `AUTOEXEC` match
`nicbase`/`nicirq` in the conf.

**Driver loads but no address** — slirp's DHCP should just work; for `pcap`,
confirm your LAN has DHCP, or set a static address in `C:\WATTCP.CFG`.

**Nothing happens on connect** — make sure the stub hub is bound to `0.0.0.0`
and not just loopback (it is, by default), and that macOS is not firewalling
the port.
