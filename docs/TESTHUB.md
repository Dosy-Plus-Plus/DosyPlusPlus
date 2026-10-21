# Test hubs

Two tiers, used for different things.

## Tier 1 — the stub hub (`tools/stubhub.py`)

Zero dependencies beyond Python 3. Use it for everyday development.

```sh
make hub                                  # or: ./tools/stubhub.py
./tools/stubhub.py --port 1511 --password secret
./tools/stubhub.py --motd-ansi            # hostile ANSI description
```

It implements SUP negotiation, SID assignment, hub INF, INF broadcast between
users, public and private chat relay, a password challenge, search forwarding
and `CTM`/`RCM` routing. It logs every line in both directions (`-->` inbound,
`<--` outbound), which is most of why it exists — you can see exactly what
each side was told. Outbound logging was missing for a long time, and "the
client did not act on what we sent" is unanswerable without it.

Two behaviours matter for transfers, both of which a real hub also has:

- **It rewrites `I4`** to the address the connection came from. A client
  behind NAT cannot know its own address and sends `0.0.0.0`, expecting the
  hub to fill it in; the dialling address in a transfer comes from that
  field, not from the `CTM`. Without the rewrite nothing is reachable and
  every download fails with "the hub never told us their address".
- **`--loopback-as <ip>`** substitutes an address for clients connecting from
  loopback. That exists for the DOSBox-X rig: a host-side peer really is at
  127.0.0.1, which inside the guest means the guest itself, while the host is
  10.0.2.2 as slirp presents it.

Two things it deliberately does **not** do:

- **Verify the password response.** Computing it needs Tiger, which Python
  lacks in the stdlib. It accepts anything, so the UI path can be built before
  Phase 1 lands.
- **Verify CID against PID.** It checks only if your Python happens to have
  `tiger192,3` available. Do not trust a successful stub login as evidence the
  CID derivation is right — that is what Tier 2 is for.

It also answers searches from a small fake share, so the file areas door has
something to show before Phase 5 exists. Terms that return something:
`warez` (4), `zip` (4), `doom` (2), `mods` (2), `ansi` (2), `exe` (2),
`phrack` (1). The AREA selector maps to ADC group codes and filters by
extension; group 7 (directories) correctly returns nothing, because the fake
share has none.

This is the hub answering **on behalf of** peers that are not really there.
On a real hub each client answers for itself, so a DOSy++ client cannot be
found until it has a share to search -- which is Phase 5.

It is not a conformance target. Passing against the stub proves the client is
self-consistent, not that it is correct.

## Tier 2 — uhub, a real hub (automated)

```sh
docker build -f tools/docker/Dockerfile.uhub -t dosypp-uhub .   # once
./test/hub/run-uhub.sh          # five conformance cases
./test/hub/run-uhub.sh --serve  # just run it on :1511 and watch the log
```

This is the test that matters. uhub 0.8.0 is an independent C implementation:
it enforces sequencing, recomputes our CID from the PID we send, and verifies
the password hash in its own code. **Passing here is evidence; passing against
the stub is not.**

Cases covered: unregistered login, registered login with the correct password,
wrong password refused, missing password refused, and two clients seeing each
other.

Config lives in `test/hub/`. Three things about it were not obvious and each
cost a debugging round:

- **uhub enables TLS on its own.** It generates a self-signed certificate at
  first start and announces `adcs://`. With `tls_enable=1` it accepts both on
  one port, but the config sets `tls_enable = 0` so the plaintext path is
  what gets tested rather than OpenSSL's.
- **Accounts are not in `users.conf`.** That file is the ACL (bans and denied
  nicks). Accounts go in a separate file read by `mod_auth_simple`, in the
  format `<credential> <username> <password>` — space separated, credential
  *first*. The old `nick:password:cred` form and the `user_reg` keywords are
  obsolete and ignored with a warning.
- **uhub's shipped ACL denies the nicks `guest`, `user` and `admin`.** A
  login test using one of those fails for reasons that have nothing to do
  with the client.

Also worth running: **[ADCH++](https://sourceforge.net/projects/adchpp/)**, the
reference-ish implementation, which is stricter still.

Public hubs additionally enforce share minimums and client-tag whitelists, and
will reject a `VEDOSy++` tag — so a failure there is not necessarily a bug.

**Phase 2 gate: met.** `./test/hub/run-uhub.sh` passes all five cases, and the
DOS build logs into uhub over a real packet driver
(`./test/dos/run-dosbox-net.sh 10.0.2.2 1511` with uhub running on the host).

## Tier 3 — a real peer (automated)

```sh
make test-airdcpp
```

**AirDC++w 2.14**, in a container, on the other end of the stub hub. This is
the test that matters most and it found more than every other test put
together, because everything else checks DOSy++ against itself: two ends of
the same wrong idea agree perfectly.

What it drives, in both directions:

| | |
|---|---|
| We search | AirDC++ answers; we read its `DRES` |
| We browse | we fetch and parse its real `files.xml.bz2` |
| We download | its file arrives and the hash matches |
| It searches | it finds our file by name and reads our `TTH` |
| It downloads | it asks for leaf hashes, transfers, and the hash matches |

**Three real bugs it found on the first run**, none of which any other test
could have:

1. **An active client could never download.** We send `CTM`, the peer dials
   us, and the only thing tying that connection to our request is the token
   it quotes in `CINF`. We never looked at it. Every other test had the
   downloader dialling out.
2. **We dropped the token when we dialled out.** A connection made in answer
   to somebody's `CTM` carries *their* token, and the side that dials must
   quote it. AirDC++ replies `CSTA 200 TO missing` and hangs up, which is
   correct of it.
3. **Refusing `CGET tthl` cost us the transfer.** A client asks for the
   Merkle tree's leaf hashes before transferring. Answering "file not
   available" reads as *we do not have the file*, so AirDC++ dropped us as a
   source entirely. Leaves are now computed on demand
   (`share_write_tthl`).

Smaller things it also showed: `CSTA 000` is informational (AirDC++ uses it
to name the hub a connection came from) and must not be read as a refusal;
two connections to one peer need the `MCN1` feature, which we do not have,
so requests to a peer are serialised; and `ADBZIP` is worth advertising
since we read and write bzipped lists.

### The topology, and why it is odd

Docker Desktop NATs a container's traffic so it reaches the host as
`127.0.0.1`, while the host is `192.168.65.254` from inside. Both ends
therefore look like loopback to the hub, and neither can dial the address
the hub observed. So the stub hub is told what each nick is reachable at
(`--i4-for NICK=IP`) and AirDC++'s client port is published. A real hub on a
real network needs none of this; it is Docker, not ADC.

AirDC++ is configured by writing its `DCPlusPlus.xml` and `Favorites.xml`
before first start, and driven afterwards through its REST API — a web user
is created with `airdcppd --add-user` fed from a pipe. `--cdm-hub` and
`--cdm-client` make it log every protocol line, which is where the evidence
for all three bugs came from.

### Still unverified against a real client

Resume offsets (`CGET` from a non-zero start), our *partial* file list
refusal, and anything to do with segmented downloads. A 100MB transfer has
not been run either; the files here are small so the test stays quick.
