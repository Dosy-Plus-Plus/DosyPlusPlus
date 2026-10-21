# What is bundled, and why it can be

Short version: **everything here is free software, and none of it is
shareware.** That is a stronger position than shareware, which is why it
is worth being exact about.

`CREDITS.txt` is generated at build time by `tools/mkcredits.sh`, which
reads each package's own `APPINFO/*.LSM` file — the licence line is what
the people who packaged the software wrote, not what anybody here
remembered. `make test-pages` fails if a file the demo serves is not
named in it.

## The claims, and what backs them

| Claim | What backs it |
|---|---|
| Every bundled program is free software | each package's own `Copying-policy` line, listed in `CREDITS.txt` |
| Nothing is shareware or abandonware | the above: GPL-2, BSD-2, BSD-3, zlib, Info-ZIP |
| The Doom data is not id Software's | `DOOM.WAD` inside `boom.zip` is **Freedoom** — its own copyright notice says "Contributors to the Freedoom project", BSD 3-clause |
| The engine is not id Software's | Boom 2.02a, "A GPL port of the Doom source code", relicensed by its copyright holders in 1999 |
| Nothing was obtained from a warez site | every artifact is fetched from the FreeDOS 1.3 repositories by `tools/fetchatmos.sh`, pinned by sha256 |
| The file names are not real releases | the invented filler in `tools/stubhub.py` was renamed for this reason: a listing of commercial titles this project does not have would be a claim it cannot support |
| The music is ours | `art/DOSYPP.MOD` is written by `tools/makemod.py`; no MOD from 1994 is shipped, because that music belongs to whoever tracked it |

## Where it is said

- **`CREDITS.txt`**, published beside the page and linked from it.
- **The page**, in a panel next to the demo rather than buried in a
  footer.
- **The hub in the tab**, which says it in its welcome message — the
  first thing a visitor reads inside the client.
- **This file**, and the README.

## If something is reported

Say which file and why. Anything credibly reported as infringing comes
out first and is discussed afterwards: there is nothing in this bundle
worth arguing over, and everything in it has a free replacement.

## This project's own licence

**MIT** — see [`LICENSE`](../LICENSE). Chosen because it is the one that
argues with nothing else here:

- The page serves Apache-2.0 code (the synthesiser and its ogg decoder)
  and BSD-2 code (v86). Apache-2.0 is not compatible with GPL-2.0, so a
  copyleft choice here would have made the published bundle a question to
  answer rather than a thing to ship.
- `src/hash/tiger_sbox.h` carries ISC-style tables from RHash. MIT sits
  under that without friction.
- The demo exists to be forked and hosted by other people. A licence that
  needs reading before you may put a copy on your own Pages site is a
  licence that stops some of them bothering.

The alternative worth knowing about is **GPL-2.0**, which is what FreeDOS
and most of the bundled software use, and which would keep derivative DOS
clients free. It is a defensible choice for the client; it is a worse one
for this repository as a whole, because the same tree builds the page.

Changing it is easy now and hard once other people have contributed.

## Reporting something

If you believe a bundled file should not be here, write to **dosy@fastmail.com**
or open an issue, and say which file and why. The file comes out first and
gets discussed afterwards; nothing here is worth arguing over.

That address is in `CREDITS.txt`, in the page's "What is in here" panel and
in the README, so it is reachable from the artifact itself and not only from
the source tree. `DOSYPP_CONTACT` overrides it at build time if someone else
ever publishes a copy.

## One more thing

- **The 1994 styling is a costume.** "The Attic", the ANSI art and
  the door numbers are a period interface, not a description of what is
  being served. Anybody reading the page learns that in the first panel;
  anybody reading only the screenshots might not.
