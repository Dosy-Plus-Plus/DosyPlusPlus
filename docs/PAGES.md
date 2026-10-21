# The browser demo, published

DOSy++ in a browser tab, from a static host, with no install: the real
DJGPP binary on an emulated 486, talking to a real ADC hub, downloading a
real game and running it.

    make pages          # build the bundle  -> build/pages
    make test-pages     # serve it, boot it, download a game through it

The published site is half of it. The other half -- a hub and a TCP relay --
runs on the visitor's machine, because a browser tab cannot open a TCP
socket and because a relay hosted here would see everybody's traffic.

## Running it without publishing it

```sh
make demo-pages
```

Serves this bundle from your own machine with a hub, a serving node and
the relay behind it -- the whole visitor experience, no GitHub account, no
upload. `docs/HOSTING.md` covers that and what to watch for before a
community is pointed at it.

## Turning it on

The workflow needs Pages switched to the Actions source once, in the
repository's **Settings -> Pages -> Build and deployment -> Source: GitHub
Actions**. Nothing else: the workflow asks for the `pages: write` and
`id-token: write` permissions it needs and creates the `github-pages`
environment itself.

Then push to the default branch, or run it by hand from the Actions tab
(`workflow_dispatch`). The first run builds the DJGPP cross-compiler from
source, which takes about ten minutes; after that it comes from the layer
cache and the build is a couple of minutes plus the gate.

A fork publishes to its own `https://<owner>.github.io/<repo>/`, and
`relay/demo.sh` is built with that URL baked in, so a fork's demo fetches a
fork's files.

## What is published

```
index.html              the page
relayurl.mjs            relay URL normalisation (the scheme matters)
midi.mjs                takes game MIDI off the emulated MPU-401
synth.mjs               plays it: a SoundFont synthesiser
midiworklet.mjs         ...on the audio thread
webmidi.mjs             or sends it out to a synth the visitor owns
vendor/synth/           spessasynth_core + stb-vorbis (Apache-2.0)
sf2/MuseScore_General.sf3   40MB, MIT, fetched only if you press Enable MIDI
vendor/                 the v86 engine, its BIOSes
images/boot.img         a FreeDOS 1.3 boot floppy with our startup
images/dosypp-*.img.zst 128MB FAT16 as ~9MB of compressed pieces:
                        DOSYPP.EXE, CWSDPMI, PCIPKT, MIDITEST, the share
images/dosypp.img.json  what the pieces are and how they are cut
relay/demo.sh           fetches the two scripts and the share, starts both
relay/wisprelay.py      Wisp (WebSocket) <-> TCP, with TLS for ADCS hubs
relay/stubhub.py        a small ADC hub, with one node that really serves
relay/attic.zip         that node's share: 49MB of free DOS software, Doom
                        included, with the hashes the client verifies
build.json              what this build is, shown in the page's footer
```

`tools/mkpages.sh` assembles it; `.github/workflows/pages.yml` builds and
publishes it on a push to the default branch, after the gate passes.

## Why no COOP/COEP, and why that matters

GitHub Pages serves static files and sets no headers you choose. A v86 built
with threads needs `SharedArrayBuffer`, which needs
`Cross-Origin-Opener-Policy` and `Cross-Origin-Embedder-Policy` -- headers
Pages cannot send, which is why so many emulator demos end up behind a
service worker that fakes them.

This bundle does not need any of that. The engine's WebAssembly memory is
unshared and `libv86.mjs` never mentions `SharedArrayBuffer`, so the page
works from any dumb static host. `make test-pages` checks both facts, and
fails if a future engine update changes either -- which it would be free to
do, since it is fetched rather than vendored.

## Size, and where it went

```
   74MB   total, of which
   38MB     the SoundFont (fetched only if somebody presses "play it here")
   22MB     the demo hub's share (fetched only by whoever runs the hub)
   10MB     the disk, as 128 compressed pieces
  3.1MB     the v86 engine, its BIOSes and the synthesiser
```

It was 220MB. Three things changed:

**The disk is published in pieces.** v86 can read an image as separate
per-chunk files (`use_parts`), and when the extension ends in `.zst` it
decompresses each piece with the zstd already inside `v86.wasm`. A 128MB
FAT16 image with 6MB of files in it is mostly zeros, and an empty piece
compresses to about fifty bytes: `tools/splitimg.mjs` turns it into 128
files totalling 8.9MB. The browser then fetches whole small files, so **the
demo no longer needs the host to honour `Range` requests** at all.

`make test-pages` boots the pieces, which matters more than it sounds: it
means the form that gets published is the form a rig has run. It also
decompresses every piece and checks the lot reassembles to the original
image byte for byte -- a splitter that dropped, padded or misnamed
something would otherwise produce a filesystem that mounts and then goes
wrong somewhere nobody is looking. (v86 decompresses in a Web Worker,
which Node does not have, so `test/v86/workershim.mjs` provides one over
`node:worker_threads`. That shim is the one part of this path a browser
does differently.)

**FreeDoom is out of the default share.** BOOM already contains a complete
playable free Doom -- GPL engine, its own CWSDPMI, a free WAD -- so
FreeDoom's own 27MB package added no game, just the largest incompressible
file in the bundle. `SHARE_FULL=1 make warez` puts it back.

**The SoundFont is optional.** `PAGES_SF=none ./tools/mkpages.sh` leaves it
out and the bundle is 36MB; the page then offers the two ways to hear game
MIDI that cost no download -- Web MIDI out to a synth the visitor owns, or
a SoundFont file from their own disk.

## No visitor's half at all

The page's default needs nothing: **the hub runs in the tab**.

A browser cannot open a TCP socket, and WebAssembly does not change that --
wasm reaches the network through the same JavaScript APIs, and none of them
do TCP. So instead of asking for a relay, the page becomes the other end of
the wire. v86's Wisp adapter opens one WebSocket and multiplexes the
guest's TCP over it; `web/wisp.mjs` answers that socket in the page
(`installInPageSocket` is the seam -- v86 builds its socket from the global
constructor, so there is nowhere else to hand it an object), and
`web/adchub.mjs` answers as the hub and as the node that has the files.

Nothing becomes TCP. The DOS client still speaks real ADC -- login, user
list, chat, search, `RCM`/`CTM`, `CGET`/`CSND` -- and still verifies every
download against the hash the hub advertised. The share is the one
published beside the page, and a file is fetched (by HTTP range) only when
the client asks for it, so a visitor who downloads nothing downloads
nothing.

What it is not: a place two people can meet. Every visitor gets their own
hub with their own share. That is also exactly why it cannot affect anybody
else: there is no network involved.

```sh
make test-v86-inpage
```

boots the real DOS binary against these modules with Node where the browser
would be -- the same code the page loads, not a model of it -- and requires
the client to log in, search, download BLKDROP.ZIP and report it complete,
which it only does after the TTH matches. Nothing listens on a port for the
whole run.

## The relay, for a hub somebody else is also in

    curl -fsSL https://<site>/relay/demo.sh | sh

It writes to `./dosypp-demo`, fetches `wisprelay.py`, `stubhub.py` and
`attic.zip`, starts the hub on 127.0.0.1:1511 and the relay on 8086, and
prints what to paste into the page. Nothing else; it is about seventy lines of `sh` and
`relay/README.txt` spells out the by-hand version for anybody who would
rather not pipe curl into a shell.

The relay box wants `wisp://127.0.0.1:8086/`. The scheme is load-bearing:
v86 chooses its network adapter from it, and only `wisp://` and `wisps://`
select the Wisp one -- a `ws://` URL silently selects the old adapter that
speaks raw Ethernet frames, and the guest then boots, loads its packet
driver and reaches nothing, with no error to say why. The page normalises
whatever is typed (`web/relayurl.mjs`, tested by
`node test/web/test-relayurl.mjs`), because a public Wisp relay's URL is
published as `wss://` and that is what somebody will paste.

Then, in the page: Boot, `3`, `BOOM`, Enter, Enter, `4` to watch it
arrive, `/quit`, and `PLAY BOOM`. The walkthrough's buttons send those
keystrokes through the emulated keyboard controller if you would rather
click, and the page spells out the unzip step both ways -- `PLAY` does it
for you, or by hand:

```
DIR *.ZIP                      what you downloaded
UNZIP -o -q BOOM.ZIP           unpack it
COPY BOOMMIDI.CFG GAMES\BOOM\BOOM.CFG    music through the MPU-401
CD GAMES\BOOM
BOOM
```

That `COPY` is the line that matters for sound, and the page says why:
Boom's autodetect picks an FM device v86 does not implement, so without it
Doom runs perfectly and plays silence.

Search `BOOM` instead and you download Doom, and if you press **Enable
MIDI** first you hear its soundtrack. That works because v86's MPU-401
answers a game's probe and then discards every note, so the page wraps the
I/O port itself, reassembles the MIDI stream and plays it through a
SoundFont on the audio thread. `make test-v86-doom` measures the result:
2,430 messages in 75 seconds rendering to 60 seconds of continuous music.
The SoundFont is 40MB and is fetched only when somebody asks for MIDI --
or they can point the page at one of their own, which never leaves their
machine.

**Or no download at all.** *Send it to my MIDI device* forwards the game's
notes out of the browser over Web MIDI, to whatever the visitor has: a
Roland module, a keyboard, a software synth, Windows' own GS Wavetable. On
a desk with an SC-55 on it that is the best this demo can sound -- a 1994
DOS game driving the hardware it was written for, with an emulator and a
browser in between. Safari has no Web MIDI; Firefox and Chrome may ask
permission, and may grant notes while refusing sysex, in which case the
page says so rather than letting an MT-32 game play with the wrong
instruments. `node test/web/test-webmidi.mjs` covers every decision that
module makes, with a stub in place of the browser.

### Their own hub

The relay decides what the guest's sentinel addresses mean:

    python3 wisprelay.py --port 8086 --map 10.77.0.1=hub.example.net:411

ADCS, with the relay as the TLS endpoint -- there is no crypto in the DOS
binary:

    python3 wisprelay.py --port 8086 --map 10.77.0.1=hub.example.net:2780:tls

The guest always dials `10.77.0.1`, so nothing inside DOS changes and no DNS
is involved. v86 hands the relay the dotted quad the guest dialled as the
Wisp CONNECT hostname, which is what makes the mapping possible at all.

A link can carry the relay: `?relay=wisps://relay.example/` fills the box
in (and `?stream=0` loads the whole disk image up front). The value is put
in the box rather than acted on, so it is visible before anything connects.

A public Wisp relay works too -- paste its `wss://` URL into the page -- but
then the guest's dotted quad is what gets connected to, so you dial the hub
by address (`DOSYPP 1.2.3.4`), and whoever runs that relay sees your traffic.

## Mixed content, and the awkward case

The published page is `https`. The relay on your machine is plain `ws`.
Browsers exempt loopback from mixed-content blocking, so
`ws://127.0.0.1:8086/` from an `https` page is allowed -- but some browsers
gate local-network access behind a permission prompt, and a corporate
policy can switch it off entirely.

When that happens the fallback needs no new software, because the relay is
also a web server:

    python3 wisprelay.py --port 8086 --web . --map 10.77.0.1=127.0.0.1:1511

Save the page and its `vendor/` and `images/` next to it (or clone the repo
and use `make demo-web`), and the whole demo is one origin on `http://
127.0.0.1:8086/`, with no mixed content and no permissions involved.

## What the gate proves, and what it does not

`make test-pages`:

- serves the bundle over HTTP with a `Range`-honouring server
  (`test/web/rangeserver.py` -- `python3 -m http.server` answers a Range
  request with the whole file and a 200, which is the one thing this has to
  tell apart)
- fetches every file the page references, by reading the references out of
  the page
- checks the engine needs no cross-origin isolation
- runs `relay/demo.sh` from the served bundle
- boots the bundle's own disk image, streamed, and downloads BLKDROP.ZIP
  from the bundled share through the relay, verifies it against the hub's
  hash and unpacks `BLKDROP.EXE`

It does not render the page: there is no browser in the rig. The same engine
runs under Node, which exercises the emulator, the network and the disk, but
not the layout, the buttons or the audio output. Those have been written
carefully and not watched working -- the honest status of anything in this
project that needs eyes or ears.

See also `docs/V86.md` for the emulator details and `docs/STUNNEL.md` for
the other way to reach an ADCS hub.
