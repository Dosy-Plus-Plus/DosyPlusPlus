# Security, and reporting a file

Two different things get reported to the same address.

**dosy@fastmail.com**

## Something that should not be bundled

The demo ships other people's software. Every package comes from the
FreeDOS 1.3 repositories, is pinned by sha256, and is listed in
`CREDITS.txt` with the licence its own authors recorded in it — see
[`docs/LICENSING.md`](docs/LICENSING.md) for each claim and what backs it.

If you believe something here should not be, write and say which file and
why. The file comes out first and gets discussed afterwards; nothing in a
demo is worth arguing over. You do not need to be the rightsholder to
report it, and you will get an answer either way.

## A security problem

This is a 1994-era network client, so set your expectations accordingly:
it is 32-bit DOS code with no ASLR, no stack protector and no sandbox,
parsing input from strangers on the network. That is the whole premise,
and bugs of that kind are expected rather than surprising.

What is worth reporting:

- A crash or memory error reachable from hub or peer traffic — an ADC
  message, a search result, a file list, a TTH leaf block. Especially one
  reachable before login.
- Anything that writes outside the share or download directory: a path in
  a file list or a download name that escapes with `..`, an absolute path,
  a DOS device name.
- A failure in the TLS relay (`tools/tlsrelay.py`) or anything that makes
  `adcs://` quieter about a certificate it should refuse.
- Something in the browser demo that reaches outside the page.

Not worth reporting: that DOS has no memory protection, that CWSDPMI runs
everything in ring 0, or that a packet driver sees all traffic. Those are
properties of the platform.

Please do not open a public issue for the first list. Write to the address
above with enough to reproduce it — the input is usually a short ADC line
or a crafted file list, and `tools/stubhub.py` will send anything you ask
it to. There is no bounty, no PGP key and no disclosure deadline; it is
one person and a hobby project. You will get a reply.

## Running your own copy

The demo page is deliberately self-contained: the hub and a serving peer
run in the page, there is no relay and no server, and it connects to
nothing. If you host a copy that talks to a real hub, read
[`docs/HOSTING.md`](docs/HOSTING.md) first — it covers what changes when
the traffic becomes real.
