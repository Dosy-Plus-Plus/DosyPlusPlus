#!/usr/bin/env python3
"""Minimal ADC hub, for development only.

Enough of the protocol to exercise the client handshake without installing a
real hub: SUP negotiation, SID assignment, hub INF, INF broadcast between
connected users, public chat relay, and optional password auth.

This is deliberately NOT a conformance target -- run uhub or ADCH++ for that
(docs/TESTHUB.md). It exists so Phases 0-2 can be developed and tested with
nothing but a Python interpreter, and so we can inject malformed messages and
hostile MOTDs on demand.

    ./tools/stubhub.py [--port 1511] [--password secret] [--motd-ansi]
"""

import argparse
import base64
import hashlib
import os
import selectors
import shutil
import socket
import subprocess
import sys
import threading

HUB_NAME = "DOSyPP Stub Hub"
BASE32 = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567"


def b32(data: bytes) -> str:
    """Base32 without padding, as ADC uses it."""
    return base64.b32encode(data).decode("ascii").rstrip("=")


def stable_cid(nick: str) -> str:
    """A CID for the attic: 24 bytes, base32, the same every run.

    A real client's CID is base32(tiger(PID)) and a strict hub checks that
    -- this one does, for real clients, when rhash is installed. The attic
    is not a real client and cannot compute Tiger, so its CID is derived
    from its nick instead. Nothing verifies a *peer's* CID (neither we nor
    DC++ do), and a stable one matters more than a well-formed one: a fresh
    CID each run shows up in the client as a new node every restart.
    """
    return b32(hashlib.sha256(nick.encode("utf-8")).digest()[:24])


def unb32(text: str) -> bytes:
    pad = "=" * (-len(text) % 8)
    return base64.b32decode(text.upper() + pad)


# Python has no Tiger in the stdlib, so verification leans on rhash when it
# is installed. Without it the stub still runs but cannot check CIDs or
# passwords -- and it says so at startup rather than quietly accepting
# anything, because "it logged into the stub" would otherwise be mistaken
# for evidence that the hashing is right.
RHASH = shutil.which("rhash")


def tiger(data: bytes):
    """Tiger-192 of data, or None if no oracle is available."""
    if not RHASH:
        return None
    try:
        out = subprocess.run([RHASH, "--tiger", "-"], input=data,
                             capture_output=True, timeout=10)
        return bytes.fromhex(out.stdout.decode().split()[0])
    except (OSError, ValueError, IndexError, subprocess.SubprocessError):
        return None


def esc(s: str) -> str:
    return s.replace("\\", "\\\\").replace(" ", "\\s").replace("\n", "\\n")


def unesc(s: str) -> str:
    out, i = [], 0
    while i < len(s):
        c = s[i]
        if c == "\\" and i + 1 < len(s):
            nxt = s[i + 1]
            if nxt == "s":
                out.append(" "); i += 2; continue
            if nxt == "n":
                out.append("\n"); i += 2; continue
            if nxt == "\\":
                out.append("\\"); i += 2; continue
        out.append(c)
        i += 1
    return "".join(out)


def sid_from_index(n: int) -> str:
    """Four-character base32 SID."""
    return "".join(BASE32[(n >> (5 * i)) & 31] for i in (3, 2, 1, 0))


class User:
    def __init__(self, sock, sid):
        self.sock = sock
        self.sid = sid
        self.rbuf = b""
        self.inf = {}            # two-char key -> unescaped value
        self.state = "PROTOCOL"  # PROTOCOL -> IDENTIFY -> VERIFY -> NORMAL
        self.challenge = b""
        self.nick = "?"
        self.keepalives = 0

    def send(self, line: str) -> None:
        # Logged, because the whole point of this hub is being able to see
        # what crossed the wire. Only inbound lines were logged for a long
        # time, which made "the client did not act on what we sent" a
        # guessing game.
        print(f"[{self.sid}] <-- {line}", flush=True)
        try:
            self.sock.sendall((line + "\n").encode("utf-8", "replace"))
        except OSError:
            pass


class StubHub:
    def __init__(self, port, password=None, motd_ansi=False,
                 motd_hostile=False, fake_share=True, loopback_as=None,
                 no_motd=False,
                 i4_for_nick=None, serve_dir=None, serve_ip="10.77.0.2",
                 serve_port=1512, serve_nick="ATTIC"):
        self.loopback_as = loopback_as
        # nick (lower case) -> the address to advertise for them
        self.i4_for_nick = i4_for_nick or {}
        self.port = port
        self.password = password
        self.motd_ansi = motd_ansi
        # Plenty of real hubs send nothing at all after login. The client
        # has its own bulletin for that case, and without this flag there
        # is no way to see it.
        self.no_motd = no_motd
        self.motd_hostile = motd_hostile
        self.fake_share = fake_share
        self.users = {}       # sid -> User
        self.next_index = 1
        self.sel = selectors.DefaultSelector()

        # The one node with real files, if asked for. See AtticPeer.
        self.serve_dir = serve_dir
        self.serve_ip = serve_ip
        self.serve_port = serve_port
        self.serve_nick = serve_nick
        self.attic = None          # the VirtualUser, once started

    def log(self, msg):
        print(msg, flush=True)

    # ------------------------------------------------------------- plumbing

    def start_attic(self):
        """Bring up the serving node, or say clearly why not.

        Refusing to start is the right answer to a share that does not add
        up: a hub that advertises files it cannot send produces downloads
        that fail hash verification, which is a far more confusing thing to
        debug than this message.
        """
        files, problems = load_share(self.serve_dir)

        for p in problems:
            self.log(f"stubhub: {p}")
        if not files:
            raise SystemExit(
                f"stubhub: nothing to serve from {self.serve_dir}\n"
                f"         run: make warez   (tools/mkshare.sh)")
        if problems:
            raise SystemExit(
                "stubhub: that share does not match its SHARE.TXT -- "
                "rehash it with tools/mkshare.sh")

        # A CID the client will accept and remember. Derived from the nick
        # so it is stable across restarts: a new CID every run means a new
        # node in the client's user list every run.
        cid = stable_cid(self.serve_nick)

        peer = AtticPeer(files, "127.0.0.1", self.serve_port, cid, self.log)
        port = peer.listen()
        peer.start()

        total = sum(f.size for f in files)
        inf = {
            "ID": cid,
            "NI": self.serve_nick,
            "DE": "the files are really here",
            "SL": "4",
            "SS": str(total),
            "SF": str(len(files)),
            "I4": self.serve_ip,
            "U4": str(port),
            "SU": "TCP4",
            "VE": "stubhub attic",   # inf_line escapes it
        }

        sid = sid_from_index(self.next_index)
        self.next_index += 1
        self.attic = VirtualUser(self, sid, peer, inf)
        self.users[sid] = self.attic

        self.log(f"stubhub: attic [{sid}] {self.serve_nick} serving "
                 f"{len(files)} file(s), {total / 1048576:.1f}MB "
                 f"on 127.0.0.1:{port}")
        self.log(f"stubhub: the guest must be able to dial "
                 f"{self.serve_ip}:{port} -- for the browser demo that "
                 f"means the relay needs")
        self.log(f"         --map {self.serve_ip}=127.0.0.1:{port}")

    def run(self):
        if self.serve_dir:
            self.start_attic()
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("0.0.0.0", self.port))
        srv.listen(8)
        srv.setblocking(False)
        self.sel.register(srv, selectors.EVENT_READ, None)

        auth = " (password required)" if self.password else ""
        print(f"stubhub: adc://0.0.0.0:{self.port}{auth}", flush=True)
        if RHASH:
            print("stubhub: verifying CIDs and passwords via rhash", flush=True)
        else:
            print("stubhub: WARNING rhash not found -- CIDs and passwords "
                  "will NOT be verified.", flush=True)
            print("stubhub:         a successful login here proves nothing "
                  "about hashing. brew install rhash", flush=True)
        print("stubhub: ctrl-c to stop", flush=True)

        try:
            while True:
                for key, _ in self.sel.select(timeout=1.0):
                    if key.data is None:
                        self.accept(key.fileobj)
                    else:
                        self.readable(key.data)
        except KeyboardInterrupt:
            print("\nstubhub: bye", flush=True)
        finally:
            srv.close()

    def accept(self, srv):
        sock, addr = srv.accept()
        sock.setblocking(False)
        sid = sid_from_index(self.next_index)
        self.next_index += 1
        u = User(sock, sid)
        self.users[sid] = u
        self.sel.register(sock, selectors.EVENT_READ, u)
        print(f"[{sid}] connect from {addr[0]}:{addr[1]}", flush=True)

    def drop(self, u, why="quit"):
        if u.sid in self.users:
            del self.users[u.sid]
        try:
            self.sel.unregister(u.sock)
        except (KeyError, ValueError):
            pass
        u.sock.close()
        print(f"[{u.sid}] {why}", flush=True)
        self.broadcast(f"IQUI {u.sid}")

    def readable(self, u):
        try:
            data = u.sock.recv(4096)
        except OSError:
            data = b""
        if not data:
            self.drop(u, "disconnect")
            return

        u.rbuf += data
        while b"\n" in u.rbuf:
            raw, u.rbuf = u.rbuf.split(b"\n", 1)
            line = raw.decode("utf-8", "replace")
            if not line:
                # An empty line is ADC's no-op, used as a keepalive. Logged
                # rather than dropped: otherwise there is no way to see
                # whether a client is actually sending them.
                u.keepalives += 1
                self.log(f"[{u.sid}] <keepalive #{u.keepalives}>")
                continue
            print(f"[{u.sid}] --> {line}", flush=True)
            self.handle(u, line)
            if u.sid not in self.users:
                return

    def broadcast(self, line, exclude=None):
        for sid, u in list(self.users.items()):
            if sid != exclude and u.state == "NORMAL":
                u.send(line)

    # ------------------------------------------------------------- protocol

    def route_direct(self, u, line, params):
        """Forward a D message to its target, as a real hub does.

        Without this, DRES from a client answering a search goes nowhere and
        searches between real clients silently return nothing -- which looks
        exactly like the share subsystem not working.
        """
        if len(params) < 2:
            return
        target = self.users.get(params[1])
        if target and target.state == "NORMAL":
            target.send(line)
        self.log(f"[{u.sid}] routed {line[:4]} -> {params[1]}")

    def handle(self, u, line):
        if len(line) < 4:
            u.send("ISTA 100 Malformed\\smessage")
            return

        kind, cmd, params = line[0], line[1:4], line[5:].split(" ") if len(line) > 5 else []
        params = [p for p in params if p]

        # Generic routing first, the way a hub actually works: B goes to
        # everyone, D goes to the addressed SID. The command-specific
        # handlers below are extras this stub provides on top.
        if kind == "D" and cmd not in ("MSG",):
            self.route_direct(u, line, params)
            return

        if cmd == "SUP":
            self.on_sup(u, params)
        elif cmd == "INF":
            self.on_inf(u, params)
        elif cmd == "PAS":
            self.on_pas(u, params)
        elif cmd == "MSG":
            self.on_msg(u, kind, params)
        elif cmd == "SCH":
            # Broadcast to the other clients so they can answer for
            # themselves -- that is what a hub does, and it is the only way
            # a client's own share becomes findable.
            if kind == "B":
                self.broadcast(line, exclude=u.sid)
            # Then answer ourselves: from the attic, which really has its
            # files, and from the invented share, which does not.
            #
            # --no-fake-share switches off only the invented half. It used
            # to skip this call altogether, which also silenced the attic:
            # the download rig searched a hub with real files on it and got
            # nothing back.
            self.on_sch(u, params)
        elif cmd == "DSC":
            self.drop(u, "client disconnect")
        else:
            print(f"[{u.sid}] (ignored {kind}{cmd})", flush=True)

    def on_sup(self, u, params):
        feats = [p for p in params if p.startswith("AD")]
        if "ADBASE" not in feats:
            u.send("ISTA 300 BASE\\srequired")
            self.drop(u, "no BASE support")
            return

        u.send("ISUP ADBASE ADTIGR")
        u.send(f"ISID {u.sid}")
        u.send(f"IINF NI{esc(HUB_NAME)} DE{esc(self.describe())} VEstubhub\\s0.1")
        u.state = "IDENTIFY"

    def describe(self):
        if self.motd_ansi:
            # Exercise the ANSI/MOTD sanitiser: real hubs send art, and a
            # hostile one could try cursor moves and a screen clear.
            return "\x1b[2J\x1b[1;32mA T T I C\x1b[0m \x1b[10;1Hdrop"
        return "development stub -- not a real hub"

    def on_inf(self, u, params):
        if u.state not in ("IDENTIFY", "NORMAL"):
            u.send("ISTA 100 INF\\sout\\sof\\ssequence")
            return

        fields = {}
        for p in params[1:] if params and len(params[0]) == 4 else params:
            if len(p) >= 2:
                fields[p[:2]] = unesc(p[2:])
        u.inf.update(fields)

        # Real hubs overwrite I4 with the address the connection actually
        # came from -- a client behind NAT cannot know its own, and sends
        # 0.0.0.0 expecting the hub to fill it in. Without this the stub
        # advertises 0.0.0.0, nobody can be dialled, and every transfer
        # fails with "the hub never told us their address".
        forced = self.i4_for_nick.get(u.inf.get("NI", "").lower())
        if forced:
            # Told explicitly what this nick is reachable at. Overrides even
            # an address the client supplied, because the point is that the
            # client's own idea of its address is useless in this rig.
            if u.inf.get("I4") != forced:
                self.log(f"[{u.sid}] I4 forced to {forced} "
                         f"for {u.inf.get('NI', '?')}")
            u.inf["I4"] = forced
        elif u.inf.get("I4", "0.0.0.0") in ("", "0.0.0.0"):
            try:
                seen = u.sock.getpeername()[0]
            except OSError:
                seen = None
            if seen:
                if self.loopback_as and seen.startswith("127."):
                    self.log(f"[{u.sid}] I4 {seen} -> {self.loopback_as}")
                    seen = self.loopback_as
                u.inf["I4"] = seen

        u.nick = u.inf.get("NI", "?")

        if u.state == "IDENTIFY":
            nick = u.inf.get("NI", "")
            if not nick:
                u.send("ISTA 221 Nick\\srequired")
                self.drop(u, "no nick")
                return
            # Real hubs enforce this (ISTA 22) and the spec requires it.
            # Without it two sessions can share a nick, which looks exactly
            # like a duplicate-user bug in the client.
            for other in self.users.values():
                if other is not u and other.state == "NORMAL" \
                        and other.nick.lower() == nick.lower():
                    u.send("ISTA 222 Nick\\salready\\staken")
                    self.drop(u, f"nick {nick!r} already taken")
                    return

            cid, pid = u.inf.get("ID", ""), u.inf.get("PD", "")
            if not cid:
                u.send("ISTA 227 Missing\\sCID")
                self.drop(u, "no CID")
                return
            if pid and not self.cid_matches(cid, pid):
                # The classic client bug: CID must be base32(tiger(PID)).
                u.send("ISTA 227 CID\\sdoes\\snot\\smatch\\sPID")
                self.drop(u, "CID/PID mismatch")
                return

            if self.password:
                u.challenge = os.urandom(24)
                u.state = "VERIFY"
                u.send(f"IGPA {b32(u.challenge)}")
                return

            self.welcome(u)
        else:
            self.broadcast(self.inf_line(u))

    def cid_matches(self, cid, pid):
        """Verify base32(tiger(PID)) == CID, the check a real hub performs.
        Returns True if it matches or cannot be checked."""
        try:
            raw = unb32(pid)
        except Exception:
            self.log("  (PD is not valid base32)")
            return False
        digest = tiger(raw)
        if digest is None:
            return True            # no oracle; reported at startup
        want = b32(digest)
        if want != cid:
            self.log(f"  CID mismatch: got {cid}, tiger(PID) = {want}")
            return False
        self.log("  CID verified against tiger(PID)")
        return True

    def on_pas(self, u, params):
        if u.state != "VERIFY" or not params:
            u.send("ISTA 100 PAS\\sout\\sof\\ssequence")
            return

        # The spec: base32(H(utf8(password) || random)). Note the PID is NOT
        # part of this -- a plausible mistake that yields "wrong password"
        # with a correct password.
        digest = tiger(self.password.encode("utf-8") + u.challenge)
        if digest is None:
            self.log(f"[{u.sid}] password accepted (no oracle to verify)")
            self.welcome(u)
            return

        want = b32(digest)
        if params[0] != want:
            self.log(f"[{u.sid}] BAD password response")
            self.log(f"           got  {params[0]}")
            self.log(f"           want {want}")
            u.send("ISTA 223 Invalid\\spassword")
            self.drop(u, "wrong password")
            return

        self.log(f"[{u.sid}] password verified")
        self.welcome(u)

    def inf_line(self, u):
        parts = [f"BINF {u.sid}"]
        for k, v in u.inf.items():
            parts.append(k + esc(v))
        return " ".join(parts)

    MOTD_ANSI = (
        "\x1b[1;36m" + "=" * 60 + "\x1b[0m\n"
        "\x1b[1;33m              T H E   A T T I C\x1b[0m\n"
        "\x1b[0;36m" + "-" * 60 + "\x1b[0m\n"
        "\x1b[0;37m  Sysop   : \x1b[1;37mThe Phantom\x1b[0m\n"
        "\x1b[0;37m  Nodes   : \x1b[1;32m4 x 14.4k DUAL STANDARD\x1b[0m\n"
        "\x1b[0;37m  Uptime  : \x1b[1;32m31 days\x1b[0m\n"
        "\x1b[0;37m  Ratio   : \x1b[1;31m1:3  NO LEECHING\x1b[0m\n"
        "\x1b[0;36m" + "-" * 60 + "\x1b[0m\n"
        "\x1b[1;35m  @X1F PCBoard codes work too @X07\x1b[0m\n"
        "\x1b[1;36m" + "=" * 60 + "\x1b[0m"
    )

    # A MOTD that tries to escape its box: cursor positioning far outside
    # the panel, a screen clear, and an OSC that would set a terminal's
    # answerback string. The client must render none of it outside its
    # rectangle. --motd-hostile sends this instead of the art above.
    MOTD_HOSTILE = (
        "\x1b[2J\x1b[1;1HOVERWRITTEN-HEADER"
        "\x1b[25;1HOVERWRITTEN-HOTKEYS"
        "\x1b]0;pwned\x07"
        "\x1b[99;99HOFFSCREEN\n"
        "\x1b[1;32mnormal text after the attack\x1b[0m"
    )

    # A fake share, so search results have something to return. Several
    # files are offered by more than one node on purpose: coalescing results
    # by TTH is the behaviour most worth exercising, and it only shows up
    # when the same hash arrives from different senders.
    # ADC group codes, as the client sends them in GR.
    GROUPS = {
        "1": (".mp3", ".mod", ".s3m", ".xm", ".wav", ".mid"),   # audio
        "2": (".zip", ".arj", ".lzh", ".rar", ".gz"),           # archive
        "3": (".txt", ".doc", ".nfo", ".1st", ".diz"),          # document
        "4": (".exe", ".com", ".bat"),                          # executable
        "5": (".gif", ".jpg", ".pcx", ".bmp"),                  # picture
        "6": (".avi", ".mpg", ".flc"),                          # video
    }

    # What the invented peers claim to have.
    #
    # The names are invented too. They used to be DOOM19S.ZIP, DOOM2.ZIP
    # and PKZ204G.EXE, which read well in a screenshot and read badly in a
    # README: a file area listing commercial games this project does not
    # have and would not ship. Real names belong to the real software that
    # tools/fetchatmos.sh fetches, which is free to redistribute.
    #
    # (path, size, tth, offered-by-index-offsets)
    SHARE = [
        ("warez/games/TURBORUN.ZIP",    2_387_968, "QAHL3II3MKN3H3CBMEPNGTAGLIQKURGL2VM2UII", 3),
        ("warez/games/SKYLINE2.ZIP",   12_582_912, "KEUMTH2Y4MQGVBCZHNRI6OL7QKKGANBQR2IK7GI", 2),
        ("warez/apps/ZIPTOOL2.EXE",       198_144, "ULXIYNB3X47MD5MC6XVVIC4GT5DGJV5QPTHXAKQ", 4),
        ("warez/apps/FIXDISK3.EXE",       221_184, "ORAIBIKHUHP5RK5RG2P4QYZD5GLG5WEPBJ4TMTA", 1),
        ("mods/NIGHTRUN.MOD",             412_672, "M4AWC4C3CRUZ43X2VVFGFKOZAA4NNCM74OYBHXY", 2),
        ("mods/TRACKER_ONE.S3M",          318_464, "KKN2SI6M7S6XYJKFXQV2LP3HWGV7OA7Z6Q5C3FY", 1),
        ("ansi/ATTIC-0894.ZIP",            87_040, "QEFEFW5QUW4XATKHQE4MPSPG4OXOIVHQMPM6GYY", 3),
        ("ansi/ATTIC-1294.ZIP",            94_208, "ONQLVIKXWYHBF2LJEJ4KKUSDX3WE3XLI5JMAI7Y", 2),
        ("docs/BOARDLST.TXT",             143_360, "S6NJG7AR6HV37WGPQUMFFNZCZT2GIXPPCVI4S5Q", 1),
        ("incoming/README.1ST",             2_048, "2LPKE47MLI76P3JHDH75RQD6SLFNXRTIR6NH57I", 1),
    ]

    def attic_hits(self, u, terms, excludes, want_tth, ge, le, group,
                   token):
        """Answer a search from the real share, as the attic.

        Returns the number of files offered. Same filtering as the invented
        share below, against files that exist.
        """
        if self.attic is None:
            return 0

        n = 0
        for f in self.attic.peer.files:
            low = f.name.lower()

            if want_tth:
                if f.tth != want_tth:
                    continue
            else:
                if terms and not all(t in low for t in terms):
                    continue
                if any(x in low for x in excludes):
                    continue
            if ge and f.size < ge:
                continue
            if le and f.size > le:
                continue
            if group:
                if group == "7":
                    continue
                exts = self.GROUPS.get(group)
                if exts and not low.endswith(exts):
                    continue

            u.send(f"DRES {self.attic.sid} {u.sid} FN{esc(f.name)} "
                   f"SI{f.size} SL4 TR{f.tth}"
                   + (f" TO{esc(token)}" if token else ""))
            n += 1

        if n:
            self.log(f"[{u.sid}] attic answered with {n} real file(s)")
        return n

    def on_sch(self, u, params):
        """Answer a search with DRES from whichever nodes are online.

        A real hub forwards the search to every client and each answers for
        itself; the stub answers on their behalf so there is something to
        look at with only one real client connected.
        """
        if u.state != "NORMAL":
            return

        fields = {}
        terms, excludes = [], []
        for p in params[1:]:
            if len(p) < 2:
                continue
            key, val = p[:2], unesc(p[2:])
            if key == "AN":
                terms.append(val.lower())
            elif key == "NO":
                excludes.append(val.lower())
            else:
                fields[key] = val

        token = fields.get("TO", "")
        want_tth = fields.get("TR")
        ge = int(fields.get("GE", 0) or 0)
        le = int(fields.get("LE", 0) or 0)

        hits = self.attic_hits(u, terms, excludes, want_tth, ge, le,
                               fields.get("GR"), token)

        # The invented share is attributed to real clients, never to the
        # attic: the attic is the one node whose results can actually be
        # downloaded, and mixing files it does not have into its listing
        # would turn a working demo into a queue of failures.
        others = [o for o in self.users.values()
                  if o.state == "NORMAL" and o is not u
                  and o is not self.attic] or [u]

        for i, (path, size, tth, nsrc) in enumerate(
                self.SHARE if self.fake_share else []):
            low = path.lower()

            if want_tth:
                if tth != want_tth:
                    continue
            else:
                if terms and not all(t in low for t in terms):
                    continue
                if any(x in low for x in excludes):
                    continue
            if ge and size < ge:
                continue
            if le and size > le:
                continue

            # The AREA selector. Group 7 is directories, which this fake
            # share has none of, so it correctly returns nothing.
            group = fields.get("GR")
            if group:
                if group == "7":
                    continue
                exts = self.GROUPS.get(group)
                if exts and not low.endswith(exts):
                    continue

            # Offer it from several nodes, so the client has to coalesce.
            for k in range(min(nsrc, len(others))):
                src = others[(i + k) % len(others)]
                u.send(f"DRES {src.sid} {u.sid} FN{esc(path)} "
                       f"SI{size} SL{1 + (k % 3)} TR{tth}"
                       + (f" TO{esc(token)}" if token else ""))
            hits += 1

        self.log(f"[{u.sid}] search -> {hits} file(s) "
                 f"from {len(others)} node(s)")

    def welcome(self, u):
        u.state = "NORMAL"

        # Existing users first, then us -- the order DC++ expects.
        for other in self.users.values():
            if other is not u and other.state == "NORMAL":
                u.send(self.inf_line(other))

        self.broadcast(self.inf_line(u))
        u.send(self.inf_line(u))
        if not self.no_motd:
            u.send("IMSG " + esc("Welcome to " + HUB_NAME + ", "
                                 + u.nick + "."))

        if self.no_motd:
            pass
        elif self.motd_hostile:
            u.send("IMSG " + esc(self.MOTD_HOSTILE))
        elif self.motd_ansi:
            u.send("IMSG " + esc(self.MOTD_ANSI))
        print(f"[{u.sid}] logged in as {u.nick!r}", flush=True)

    def on_msg(self, u, kind, params):
        if u.state != "NORMAL":
            return
        if kind == "B" and len(params) >= 2:
            self.broadcast(f"BMSG {u.sid} {params[1]}")
        elif kind == "D" and len(params) >= 3:
            target = self.users.get(params[1])
            if target:
                target.send(f"DMSG {u.sid} {params[1]} {params[2]} PM{u.sid}")


# --------------------------------------------------------------- the attic
#
# One node on this hub really has the files. Everything above it is a hub
# pretending to be busy; this is the part that makes the browser demo a
# demo rather than a screenshot: a DOS client in a browser tab searches,
# downloads a real game over real ADC, verifies the hash and runs it.
#
# It is deliberately not a second copy of the client. It speaks only the
# uploading half of the client-to-client protocol -- CSUP, CINF, CGET,
# CSND -- which is small enough to read in one sitting, and it serves from
# a directory with the hashes precomputed by tools/mkshare.sh.
#
# Why precomputed: the client verifies every byte it downloads against the
# TTH the hub advertised. Computing that here would need Tiger, which
# Python does not have, and the whole point of this file is that a visitor
# needs nothing but python3. So the hashes are built into the share.


class Served:
    """One file in the attic's share."""

    def __init__(self, name, size, tth, path):
        self.name = name          # virtual path, forward slashes
        self.size = size
        self.tth = tth
        self.path = path          # where it really is


def load_share(dirpath):
    """Read SHARE.TXT and the files it names. Returns (files, problems).

    Loud about disagreement rather than quietly serving less: a share
    where half the files went missing looks, from the client's side,
    exactly like a hub that cannot answer searches.
    """
    index = os.path.join(dirpath, "SHARE.TXT")
    files, problems = [], []

    if not os.path.isfile(index):
        return [], [f"{index} is missing -- run tools/mkshare.sh"]

    with open(index, "r", encoding="utf-8") as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            bits = line.split()
            if len(bits) < 3:
                problems.append(f"SHARE.TXT:{lineno}: cannot parse {line!r}")
                continue

            tth, size, name = bits[0], bits[1], " ".join(bits[2:])
            path = os.path.join(dirpath, name)

            if not os.path.isfile(path):
                problems.append(f"SHARE.TXT:{lineno}: {name} is not here")
                continue
            try:
                size = int(size)
            except ValueError:
                problems.append(f"SHARE.TXT:{lineno}: bad size {size!r}")
                continue

            real = os.path.getsize(path)
            if real != size:
                problems.append(f"{name}: SHARE.TXT says {size} bytes, "
                                f"the file is {real} -- rehash it")
                continue
            if len(tth) != 39:
                problems.append(f"{name}: {tth!r} is not a 39-character TTH")
                continue

            files.append(Served(name, size, tth, path))

    return files, problems


class AtticPeer(threading.Thread):
    """The TCP side: serve files to whoever dials in.

    A thread rather than part of the hub's selector loop because a transfer
    is a blocking copy of a few megabytes and the hub's job meanwhile is to
    stay responsive -- a stalled hub looks like a protocol bug. One further
    thread per connection, which for a demo with a handful of nodes is the
    simplest thing that cannot stall.
    """

    FEATURES = "CSUP ADBASE ADTIGR ADBZIP"
    CHUNK = 32 * 1024

    def __init__(self, files, host, port, cid, log):
        super().__init__(daemon=True)
        self.files = files
        self.by_tth = {f.tth: f for f in files}
        self.by_name = {f.name.lower(): f for f in files}
        self.host = host
        self.port = port
        self.cid = cid
        self.log = log
        self.srv = None

    # ------------------------------------------------------------- listening

    def listen(self):
        self.srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind((self.host, self.port))
        self.srv.listen(8)
        return self.srv.getsockname()[1]

    def run(self):
        while True:
            try:
                conn, addr = self.srv.accept()
            except OSError:
                return
            threading.Thread(target=self.serve, args=(conn, addr),
                             daemon=True).start()

    # --------------------------------------------------------------- serving

    def serve(self, conn, addr):
        who = f"attic<{addr[0]}:{addr[1]}>"
        self.log(f"[{who}] connect")
        try:
            conn.settimeout(60)
            self.send(conn, who, self.FEATURES)
            self.send(conn, who, f"CINF ID{self.cid}")

            buf = b""
            while True:
                data = conn.recv(4096)
                if not data:
                    break
                buf += data
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    line = raw.decode("utf-8", "replace").strip("\r")
                    if not line:
                        continue
                    self.log(f"[{who}] --> {line}")
                    if not self.handle(conn, who, line):
                        return
        except (OSError, socket.timeout) as e:
            self.log(f"[{who}] closed: {e}")
        finally:
            try:
                conn.close()
            except OSError:
                pass
            self.log(f"[{who}] done")

    def send(self, conn, who, line):
        self.log(f"[{who}] <-- {line}")
        conn.sendall((line + "\n").encode("utf-8", "replace"))

    def handle(self, conn, who, line):
        """False to hang up, True to carry on."""
        if len(line) < 4:
            return True
        cmd, params = line[1:4], line[5:].split(" ") if len(line) > 5 else []
        params = [p for p in params if p]

        if cmd == "SUP":
            return True              # ours went out on connect
        if cmd == "INF":
            return True              # we do not care who they are
        if cmd == "GET":
            return self.on_get(conn, who, params)

        self.log(f"[{who}] (ignored C{cmd})")
        return True

    def on_get(self, conn, who, params):
        # CGET <type> <identifier> <start> <bytes>
        if len(params) < 4:
            self.send(conn, who, "CSTA 140 Malformed\\sGET")
            return True

        kind, ident = params[0], unesc(params[1])
        try:
            start = int(params[2])
            count = int(params[3])
        except ValueError:
            self.send(conn, who, "CSTA 140 Malformed\\sGET")
            return True

        if kind == "tthl":
            # Honest refusal rather than invented leaves: computing them
            # needs Tiger. The client retries without them, which is a path
            # worth exercising anyway.
            self.send(conn, who,
                      "CSTA 151 Leaf\\shashes\\snot\\savailable\\shere")
            return True

        if kind == "list":
            return self.send_list(conn, who, ident)

        if kind != "file":
            self.send(conn, who, "CSTA 151 Unsupported\\srequest\\stype")
            return True

        f = None
        if ident.startswith("TTH/"):
            f = self.by_tth.get(ident[4:])
        else:
            f = self.by_name.get(ident.lstrip("/").lower())

        if f is None:
            self.send(conn, who, "CSTA 151 File\\snot\\savailable")
            return True
        if start < 0 or start >= f.size:
            self.send(conn, who, "CSTA 152 Invalid\\soffset")
            return True

        if count < 0 or start + count > f.size:
            count = f.size - start

        self.send(conn, who,
                  f"CSND file {esc(ident)} {start} {count}")
        self.log(f"[{who}] sending {f.name} [{start}..{start + count})")

        sent = 0
        with open(f.path, "rb") as fh:
            fh.seek(start)
            while sent < count:
                block = fh.read(min(self.CHUNK, count - sent))
                if not block:
                    break
                conn.sendall(block)
                sent += len(block)

        self.log(f"[{who}] sent {sent} byte(s) of {f.name}")
        # Stay open: the client may ask for the next segment on the same
        # connection, which is exactly what it does for a large file.
        return True

    # ------------------------------------------------------------ file lists

    def files_xml(self, base="/"):
        """A plain (uncompressed) files.xml of the share.

        The client sniffs the format, so bzip2 is optional here -- and
        AirDC++ sends these uncompressed too, which is where that sniffing
        came from.
        """
        want = base if base.startswith("/") else "/" + base
        want = want.rstrip("/") + "/"

        # Build the tree of everything under `want`.
        tree = {}
        for f in self.files:
            full = "/" + f.name
            if not full.startswith(want):
                continue
            parts = full[len(want):].split("/")
            node = tree
            for p in parts[:-1]:
                node = node.setdefault(p, {})
            node.setdefault("", []).append(f)

        out = ['<?xml version="1.0" encoding="utf-8" standalone="yes"?>',
               f'<FileListing Version="1" CID="{self.cid}" '
               f'Base="{xml_attr(want)}" Generator="stubhub attic">']

        def emit(node, depth):
            pad = " " * depth
            for name in sorted(k for k in node if k != ""):
                out.append(f'{pad}<Directory Name="{xml_attr(name)}">')
                emit(node[name], depth + 1)
                out.append(f"{pad}</Directory>")
            for f in sorted(node.get("", []), key=lambda f: f.name):
                out.append(f'{pad}<File Name="'
                           f'{xml_attr(f.name.rsplit("/", 1)[-1])}" '
                           f'Size="{f.size}" TTH="{f.tth}"/>')

        emit(tree, 1)
        out.append("</FileListing>")
        return ("\n".join(out) + "\n").encode("utf-8")

    def send_list(self, conn, who, ident):
        xml = self.files_xml(ident or "/")
        self.send(conn, who, f"CSND list {esc(ident)} 0 {len(xml)}")
        conn.sendall(xml)
        self.log(f"[{who}] sent a {len(xml)}-byte file list for {ident!r}")
        return True


def xml_attr(s):
    return (s.replace("&", "&amp;").replace("<", "&lt;")
             .replace(">", "&gt;").replace('"', "&quot;"))


class VirtualUser:
    """The attic, as the hub's user table sees it.

    Has no socket. Everything the hub sends it is either ignored or, in the
    one case that matters, answered: a passive client asking for a
    connection (DRCM) gets told where to dial (DCTM). That is the whole
    reason a browser guest can download anything -- it cannot be dialled,
    so it has to do the dialling.
    """

    def __init__(self, hub, sid, peer, inf):
        self.hub = hub
        self.sid = sid
        self.peer = peer
        self.inf = inf
        self.state = "NORMAL"
        self.nick = inf.get("NI", "attic")
        self.rbuf = b""
        self.challenge = b""
        self.keepalives = 0
        self.sock = None

    def send(self, line):
        if not line.startswith("DRCM "):
            return
        params = line[5:].split(" ")
        if len(params) < 4:
            return
        frm, to, token = params[0], params[1], params[3]
        if to != self.sid:
            return

        asker = self.hub.users.get(frm)
        if asker is None or asker.state != "NORMAL":
            return

        self.hub.log(f"[{self.sid}] {asker.nick} wants a connection -- "
                     f"telling them to dial port {self.peer.port}")
        asker.send(f"DCTM {self.sid} {frm} ADC/1.0 {self.peer.port} {token}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", type=int, default=1511)
    ap.add_argument("--password", default=None,
                    help="require a password (response is not verified)")
    ap.add_argument("--motd-ansi", action="store_true",
                    help="send an ANSI welcome screen, for the welcome door")
    ap.add_argument("--no-motd", action="store_true",
                    help="send no welcome text at all, like a quiet hub")
    ap.add_argument("--no-fake-share", action="store_true",
                    help="do not answer searches from the built-in fake "
                         "share (for testing real client transfers)")
    ap.add_argument("--motd-hostile", action="store_true",
                    help="send a MOTD that tries to escape its rectangle")
    ap.add_argument("--loopback-as", default=None, metavar="IP",
                    help="advertise this address for clients that connect "
                         "from loopback. For the DOSBox-X rig: a host "
                         "client's real address is 127.0.0.1, which the "
                         "guest cannot dial, while the host is 10.0.2.2 "
                         "from inside slirp.")
    ap.add_argument("--i4-for", action="append", default=[],
                    metavar="NICK=IP",
                    help="advertise IP as this nick's address, whatever the "
                         "connection says. Repeatable. For rigs where the "
                         "two clients cannot reach each other's real "
                         "addresses: on Docker Desktop a container's "
                         "traffic reaches the host as 127.0.0.1 and the "
                         "host is 192.168.65.254 from inside, so both ends "
                         "look like loopback and neither can dial what the "
                         "hub observed. A real hub on a real network never "
                         "needs this.")
    ap.add_argument("--serve", default=None, metavar="DIR",
                    help="serve the files in DIR for real, as one extra "
                         "node on this hub. DIR needs a SHARE.TXT with "
                         "their hashes: build one with tools/mkshare.sh. "
                         "This is what makes a download work rather than "
                         "merely appear in a search.")
    ap.add_argument("--serve-ip", default="10.77.0.2", metavar="IP",
                    help="the address to advertise for the serving node "
                         "(default 10.77.0.2). A browser guest dials this "
                         "and the Wisp relay maps it; on a real network it "
                         "would be an address that resolves to this host.")
    ap.add_argument("--serve-port", type=int, default=1512,
                    help="the serving node's TCP port (default 1512)")
    ap.add_argument("--serve-nick", default="ATTIC",
                    help="the serving node's nick (default ATTIC)")
    args = ap.parse_args()

    by_nick = {}
    for spec in args.i4_for:
        if "=" not in spec:
            print(f"stubhub: ignoring --i4-for {spec!r}: no '='", flush=True)
            continue
        nick, addr = spec.split("=", 1)
        by_nick[nick.lower()] = addr

    StubHub(args.port, args.password, args.motd_ansi,
            args.motd_hostile, not args.no_fake_share,
            args.loopback_as, args.no_motd, by_nick,
            serve_dir=args.serve, serve_ip=args.serve_ip,
            serve_port=args.serve_port, serve_nick=args.serve_nick).run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
