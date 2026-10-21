# Contributing

Patches, bug reports and "it booted on my 486" are all welcome. There is
no CLA and no style bot.

## Build it first

```sh
make host                      # native build: a C99 compiler and Python 3
make test                      # 10,384 unit checks, a few seconds
```

That is enough for most changes. The DOS binary needs DJGPP, which
`make dos-docker` runs in a container so you do not have to install it;
`make dos` uses a local toolchain if you have one. DOSBox-X runs it
(`cd dosbox-x && dosbox-x -conf dosbox-x.conf`), and `make demo-web` runs
it in a browser. [`docs/TOOLCHAIN.md`](docs/TOOLCHAIN.md) has the details.

## The two C dialects

`src/` compiles twice: once for the host with `-std=c99`, and once for
DJGPP with **`-std=gnu89`**. The DOS build is the one that matters, so the
rule is C89 — declarations at the top of a block, no `//` comments, no
declaring a loop variable in the `for`. The host build will happily accept
what DJGPP then rejects, so run `make dos-docker` before sending anything
that touches `src/`.

Two other things the target imposes:

- **`long` is 32 bits.** File sizes and share totals are 64-bit values
  printed and parsed where `long` is not big enough. There is a real bug
  in the history from exactly this, and `test/host/test_tokenize.c` runs
  on DOS as well as the host because of it.
- **Memory is 16MB and there is no protection.** A share scan of 5GB has
  to work without holding it all; `make test-bigshare` is the gate.

## Tests

Nothing in this repository asserts that a feature exists. Each rig
measures something and fails loudly, and that is the one convention worth
insisting on:

> **Sabotage a new check once, and watch it fail.** Break the thing it is
> supposed to catch, confirm the message says something useful, then put
> it back. A check that has never failed is a check nobody has tested.

This is not pedantry. Rigs in this tree have passed while testing the
wrong hub, passed on a race, and passed while a binary was never built.
Each of those was found by breaking something on purpose.

```sh
./tools/sweep.sh               # everything, in five tiers, ~45 minutes
./tools/sweep.sh host          # just the fast one, ~9 minutes
./tools/sweep.sh host web      # and the browser gates
```

The tiers are cheapest-first, so a failure usually arrives in the first
minute: `host` needs a compiler and Python; `big` the 5GB share rigs;
`others` runs uhub and AirDC++ in Docker; `dos` is DOSBox-X headless;
`web` is v86 under Node. CI runs the `host` tier on every push, which is
the floor, not the bar — if your change touches DOS or the page, run that
tier locally and say so in the PR.

If a rig needs a port, use `free_port` and `start_stub_hub` from
`test/host/hublib.sh`. Hard-coded ports are how several rigs ended up
testing whatever else was running on the machine.

## Protocol changes

Agreeing with `tools/stubhub.py` proves we are self-consistent, not
correct, because that hub is our code too. Anything that changes what goes
on the wire should also pass `make test-uhub` and `make test-airdcpp`,
which are other people's implementations.

## Adding software to the demo share

Only free software, only from a source that states its licence, only
pinned by hash. `tools/mkcredits.sh` reads each FreeDOS package's own
`APPINFO/*.LSM` licence line and refuses to generate `CREDITS.txt` if a
package does not have one — and `make test-pages` fails if the demo serves
a file `CREDITS.txt` does not name. Do not work around either of those.
[`docs/LICENSING.md`](docs/LICENSING.md) explains why.

## Commits and PRs

Say what changed and why it was wrong before. If you fixed a bug, the
message is a good place for the symptom you saw and how you proved it was
gone — most of the commits here read that way, and it is the only record
of why some of this code looks the way it does.

Screenshots are welcome, especially from real hardware. `make screens`
regenerates `docs/screens/*.txt` from a live session; they are captures,
so regenerate them rather than editing them by hand.

## Reporting things

Bugs and ideas: open an issue. Questions, hardware stories and "does this
work with X": Discussions. A bundled file that should not be here, or a
memory-safety bug reachable from the network: see
[`SECURITY.md`](SECURITY.md).
