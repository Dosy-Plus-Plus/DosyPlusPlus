# Showing this to people

Two separate questions, and only the second one needs GitHub.

## Running it yourself, now

```sh
make pages          # build the bundle -> build/pages   (seconds, if warm)
make demo-pages     # serve it, with a hub behind it, and open it
```

`make demo-pages` prints the URL in about two seconds and then **stays in
the foreground**, following the relay's log -- a `CONNECT` line there is
how you know the guest got out. Ctrl-C stops all three servers. If you
would rather have the shell back:

```sh
./tools/demo-pages.sh --detach    # start it and return
./tools/demo-pages.sh --stop      # stop it again
```

It does not build anything. `make demo-pages` used to depend on `make
pages`, and because every target underneath that is phony it rebuilt the
DOS binary (in the container), the images, the share and the bundle every
time -- minutes of work with little output, which is indistinguishable
from a hang. Now it checks the bundle is there and tells you to run `make
pages` if it is not.

### If it hangs with mtools on screen, or nothing at all

`make pages` builds the disk image, and the image build asks questions.
mtools prompts on a name clash -- an `mmd` for a directory that already
exists reads stdin for an answer -- and the prompt was hidden behind a
`2>/dev/null`. A script with stdin redirected gets EOF and carries on,
which is every automated run; a person in a terminal waits for ever,
looking at

      + DL\DEMO2.BIN  32K

and nothing after it. Every mtools-using script in this repository now
begins `exec < /dev/null`, and the image build remembers which
directories it has created so the clash never happens. Proved both ways:
the second `mmd` for the same directory waits for input under a pty, and
the build now finishes under one.

### If it still seems to hang

`make pages` reached for Docker on every run, even with the binary already
built, and `tools/dosmake.sh` waited for `docker info` with no timeout and
no message. With Docker stopped, starting, or wedged, the last line you
saw was

    no local DJGPP: building DOSYPP.EXE in the container

and then nothing, for ever. Two things changed: the DOS binary is rebuilt
only when something in `src/` or `third_party/` is actually newer than it
(so the warm path never touches Docker -- measured at 3s), and the Docker
check is bounded at 25 seconds and says what it is waiting for.

Measured end to end, warm: `make pages` 3s, `make demo-pages` prints the
URL 2s later.

That serves **the exact directory the Pages workflow uploads**, from your
machine, with the three things a visitor's machine would be running: the
hub, the node that really has the files, and the Wisp relay. No GitHub, no
account, nothing uploaded anywhere. The relay serves the page as well, so
everything is one origin and there is no mixed content to argue with.

`make test-pages` is the same bundle checked rather than looked at: served
over HTTP, booted in the form it publishes, and a game downloaded into it.

## Publishing it

Only needed to put it on the internet. Switch Pages to the Actions source
once (**Settings → Pages → Build and deployment → Source: GitHub
Actions**) and push; `.github/workflows/pages.yml` does the rest. See
`docs/PAGES.md`. Nothing about the bundle changes when you publish it --
what you rehearsed locally is what strangers get.

## Nobody else's hub

The thing worth being careful about before a community is pointed at this.
Four properties, three of them checked by a rig:

**The relay is an allowlist.** A browser guest's only route to the network
is the Wisp relay, and the relay connects to what `--map` names and
nothing else. `--allow-direct` turns that off and is documented as what it
is: an open TCP proxy. Leave it alone.

**The demo has its own hub list.** The client's hub-list door defaults to a
real public service (`HUBLIST_DEFAULT_URL`), which is right for a copy of
this on a real machine and wrong for a demo strangers open -- it would
point all of them at hubs that never agreed to be involved. The browser
image therefore sets `hublist = http://10.77.0.3/hublist.xml.bz2`, a
sentinel mapped to a one-entry list naming the demo's own hub
(`tools/mkhublist.py`).

**The guest cannot be dialled.** Wisp carries outbound streams only, so
the image ships `active = no`. Nothing on the internet can open a
connection into somebody's browser tab, and the client says so in its INF
rather than letting peers try.

**It is gated, not asserted:**

```sh
make test-v86-hublist
```

boots the demo image, lets it fetch its hub list, and then reads the
relay's own log: the guest asked for `10.77.0.1:1511` and `10.77.0.3:80`
and nothing else. It fails if any other address is dialled, and fails if
anything reaches for the public hub-list service.

## Before you publish: what you are publishing

Everything bundled is free software and `CREDITS.txt` says so with each
package's own licence line — see [`LICENSING.md`](LICENSING.md), which
also lists what backs each claim. The one gap a script cannot close is
that **this repository has no licence of its own yet**; a demo whose own
terms are unstated is the weakest part of the picture.

## A real hub, for a real experience

The stub hub (`tools/stubhub.py`) is enough to log in, chat, search,
browse and transfer, and `--serve` makes one node on it genuinely serve
files. It is not a conformance target and not meant to run unattended.

For something people can keep using, run a real one. This repository
already builds **uhub** for its conformance gates:

```sh
make uhub-image          # docker image, from tools/docker/Dockerfile.uhub
make test-uhub           # five conformance cases against it
```

Point the relay at it instead of the stub:

```sh
python3 wisprelay.py --port 8086 --map 10.77.0.1=your-hub.example:411
```

or, for an ADCS hub -- there is no TLS in the DOS binary, so the relay
terminates it:

```sh
python3 wisprelay.py --port 8086 \
        --map 10.77.0.1=your-hub.example:2780:tls
```

Two things to decide if you host the hub publicly:

- **Who runs the relay.** If each visitor runs their own (the `demo.sh`
  route), nothing of theirs passes through you, and your hub sees ordinary
  ADC connections from their addresses. If you host a public Wisp relay so
  visitors need no install at all, every connection is yours to carry and
  you see their traffic in the clear -- TLS included, because you are the
  TLS endpoint. That is a privacy statement you would owe them.
- **What the hub list says.** Regenerate it with your hub's real address so
  door 0 shows something true:

  ```sh
  tools/mkhublist.py build/pages-demo \
      --name "Your Hub" --address adc://your-hub.example:411
  ```

  Keep it to hubs that agreed to be on it.

## Before the community sees it

What has a rig behind it, and what does not:

| | |
|---|---|
| the bundle is complete and boots as published | `make test-pages` |
| a download works end to end, hash verified | `make test-v86-dl` |
| the demo touches no hub but its own | `make test-v86-hublist` |
| game MIDI reaches a synthesiser | `make test-v86-midi`, `make test-v86-doom` |
| the page renders, the buttons work, sound comes out of a speaker | **nothing** |

That last row is the honest state: there is no browser in any of this.
`make demo-pages` is how you check it yourself, and it is worth doing
before anybody else does.
