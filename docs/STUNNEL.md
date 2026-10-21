# ADCS via a TLS relay

DOSy++ contains no crypto. To reach an `adcs://` hub, run a TLS relay on the
LAN and point the client at the relay's plaintext side. Most public ADC hubs
are ADCS-only, so in practice this is how you connect to anything you did not
set up yourself.

This path is tested, not just described: `make test-adcs` runs a real uhub
with `tls_enable` *and* `tls_require` set, puts a relay in front of it, and
logs in through it with a password. It also checks the two things that would
make the test meaningless -- that a plaintext dial at the hub fails, and
that the relay refuses an unverifiable certificate unless told not to.

## Two relays to choose from

`tools/tlsrelay.py` ships with DOSy++ and needs nothing but Python 3:

```sh
# Bind the LAN address the DOS box can reach, not 127.0.0.1.
tools/tlsrelay.py --listen 192.168.1.10:1511 --connect somehub.example:2780

# A hub with a self-signed certificate, which is most small ones:
tools/tlsrelay.py -l 1511 -c somehub.example:2780 --cafile ./thathub.pem
```

It exists because "install stunnel first" is a real barrier for somebody who
only wants to find out whether their DOS box can reach a hub at all, and
because it is what the test rig drives. For anything you leave running,
stunnel is the better answer: it has had twenty years of review that a
200-line Python script has not.

`brew install stunnel` (5.82 at time of writing).

## Config

`stunnel-adcs.conf`:

```ini
; Relay for DOSy++. Plaintext on the LAN side, TLS to the hub.
foreground = yes
output     = /dev/stdout

[adcs-hub]
client  = yes
; Bind to the LAN address the DOS box can reach, NOT 127.0.0.1 --
; the DOS machine is a different host.
accept  = 0.0.0.0:1511
connect = somehub.example:2780
verifyChain = yes
CApath  = /opt/homebrew/etc/openssl@3/certs
checkHost = somehub.example
```

```sh
stunnel stunnel-adcs.conf
```

Then from DOS, dial the relay machine:

```
C:\> dosypp 192.168.1.10 -p 1511
```

## Keep verification on

`verifyChain = yes` for stunnel; for `tlsrelay.py` it is the default and
`--insecure` is what turns it off. It is tempting to disable verification to
get going. Don't: the relay is the *only* thing authenticating the hub in
this architecture, so with verification off the whole TLS leg is decoration.

A hub with a self-signed certificate -- which is most hubs run by one person,
including uhub out of the box -- does not need `--insecure`. Fetch its
certificate once and point `--cafile` (or stunnel's `CAfile`) at it:

```sh
openssl s_client -connect somehub.example:2780 -showcerts </dev/null \
    2>/dev/null | openssl x509 > thathub.pem
```

That pins the hub to that certificate, which for a hub you know is better
than a public CA chain, not worse. `--insecure` is for finding out whether
the plumbing works at all, and `tlsrelay.py` says so in its log every time
so that a run with it on cannot be mistaken for a run without.

## What the test proves, and what it does not

`make test-adcs` establishes that the relay architecture works and that
DOSy++ needs no crypto to use an ADCS hub. It does not make the LAN leg
private -- see below -- and it says nothing about client-to-client
transfers, which are a separate connection and are plaintext whatever the
hub leg does.

## One relay per hub

stunnel maps one listening port to one destination, so each hub needs its own
`[section]` and port. Note the port in the DOSy++ favorites list alongside the
real hub address, so the config screen shows something meaningful rather than
a row of `192.168.1.10:151x` entries.

## Security boundary

The LAN leg is plaintext. Anyone on that network sees your hub traffic,
including the password handshake. On a trusted home LAN that is a reasonable
trade; on anything shared it is not. This is the main reason native TLS is on
the backlog (`TODO.md`).

## In the browser build

The v86 WebSocket→TCP relay occupies exactly this position, so teaching it TLS
gives the browser build ADCS with no extra moving parts — one relay instead of
two. See `IMPLEMENTATION_PLAN.md` §6.
