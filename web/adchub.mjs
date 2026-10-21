/* adchub.mjs - an ADC hub, in the page.
 *
 * The DOS client in the tab speaks real ADC over a real (emulated)
 * Ethernet. This is what answers: hub login, the user list, chat, search,
 * and the connection dance that lets a passive client download from an
 * active peer. It is a port of the subset of tools/stubhub.py the client
 * exercises, which is the subset with rigs behind it.
 *
 * Why in the page at all: a browser cannot open a TCP socket, so the
 * alternative is a relay somebody has to run and a hub somebody has to
 * host. Here the hub is an object in the same tab, reached over the Wisp
 * server in web/wisp.mjs, and the demo needs no server of any kind.
 *
 * What it is not: a conformance target, or a place two people can meet.
 * Every visitor gets their own hub with their own share, which is also why
 * it cannot affect anybody else's -- there is no network involved.
 *
 * The share is the one published beside this file (attic/SHARE.TXT), and
 * the serving peer fetches a file only when the client asks for it, so a
 * visitor who never downloads anything never downloads anything.
 */

const HUB_NAME = "The Attic (in this tab)";
const BASE32 = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

const enc = new TextEncoder();
const dec = new TextDecoder();

function esc(s) {
    return String(s).replace(/\\/g, "\\\\").replace(/ /g, "\\s")
        .replace(/\n/g, "\\n");
}

function unesc(s) {
    return String(s).replace(/\\s/g, " ").replace(/\\n/g, "\n")
        .replace(/\\\\/g, "\\");
}

function sidFromIndex(n) {
    return [3, 2, 1, 0].map((i) => BASE32[(n >> (5 * i)) & 31]).join("");
}

/* A CID for the serving node: 39 base32 characters, stable across
   reloads. A real client's is base32(tiger(PID)) and nothing verifies a
   peer's, so what matters is that it does not change every time -- a fresh
   one would appear in the client as a new node on every boot. */
const ATTIC_CID = "WEBATTICCIDWEBATTICCIDWEBATTICCIDWEBATT";

/** One line-oriented ADC connection over a Wisp stream. */
class Connection {
    constructor(stream, onLine) {
        this.stream = stream;
        this.buffer = "";

        stream.onData = (bytes) => {
            this.buffer += dec.decode(bytes, { stream: true });
            let nl;
            while ((nl = this.buffer.indexOf("\n")) >= 0) {
                const line = this.buffer.slice(0, nl);
                this.buffer = this.buffer.slice(nl + 1);
                if (line.length)
                    onLine(line);
            }
        };
    }

    send(line) {
        this.stream.writeLine(line);
    }
}

/**
 * The hub.
 *
 * `share` is the list from SHARE.TXT: [{name, size, tth}], and `fetchFile`
 * is how the serving peer gets the bytes -- injected so the page fetches
 * over HTTP and a test rig can read from disk.
 */
export class AdcHub {
    constructor({ share = [], fetchFile = null, atticIp = "10.77.0.2",
                  atticPort = 1512, nick = "ATTIC" } = {}) {
        this.share = share;
        this.fetchFile = fetchFile;
        this.atticIp = atticIp;
        this.atticPort = atticPort;
        this.atticNick = nick;
        this.atticSid = sidFromIndex(1);

        this.users = new Map();     /* sid -> {conn, inf, state, nick} */
        this.nextIndex = 2;
        this.onLog = null;
        this.onEvent = null;        /* for the page: logins, searches... */
        this.sent = 0;
        this.served = 0;            /* bytes the attic has sent */
    }

    log(msg) {
        if (this.onLog)
            this.onLog(msg);
    }

    event(kind, detail) {
        if (this.onEvent)
            this.onEvent(kind, detail);
    }

    /** The listeners a WispServer needs: the hub, and the serving node. */
    listeners() {
        return {
            [`10.77.0.1:1511`]: (s) => this.accept(s),
            [`${this.atticIp}:${this.atticPort}`]: (s) => this.acceptPeer(s),
            /* The client may dial the peer on whatever port the CTM named;
               an address-only match covers that. */
            [this.atticIp]: (s) => this.acceptPeer(s),
            "10.77.0.1": (s) => this.accept(s),
        };
    }

    /* ------------------------------------------------------------- the hub */

    accept(stream) {
        const sid = sidFromIndex(this.nextIndex++);
        const user = { sid, inf: {}, state: "PROTOCOL", nick: "?" };

        user.conn = new Connection(stream, (line) => {
            this.log(`[${sid}] --> ${line}`);
            try {
                this.handle(user, line);
            } catch (e) {
                this.log(`[${sid}] handler threw: ${e && e.message}`);
            }
        });

        stream.onClose = () => {
            if (this.users.delete(sid))
                this.broadcast(`IQUI ${sid}`);
            this.log(`[${sid}] gone`);
        };

        this.users.set(sid, user);
        this.log(`[${sid}] connected`);
    }

    send(user, line) {
        this.log(`[${user.sid}] <-- ${line}`);
        this.sent++;
        user.conn.send(line);
    }

    broadcast(line, exclude = null) {
        for (const [sid, u] of this.users)
            if (sid !== exclude && u.state === "NORMAL")
                this.send(u, line);
    }

    handle(user, line) {
        if (line.length < 4)
            return;

        const kind = line[0];
        const cmd = line.slice(1, 4);
        const params = line.length > 5 ? line.slice(5).split(" ").filter(Boolean)
                                       : [];

        /* D messages go to the addressed SID, which is how a passive
           client asks the attic for a connection. */
        if (kind === "D" && cmd !== "MSG") {
            this.routeDirect(user, line, params);
            return;
        }

        if (cmd === "SUP")
            this.onSup(user);
        else if (cmd === "INF")
            this.onInf(user, params);
        else if (cmd === "MSG")
            this.onMsg(user, kind, params);
        else if (cmd === "SCH")
            this.onSch(user, params);
        else if (cmd === "DSC")
            user.stream && user.stream.close();
    }

    onSup(user) {
        this.send(user, "ISUP ADBASE ADTIGR");
        this.send(user, `ISID ${user.sid}`);
        this.send(user, `IINF NI${esc(HUB_NAME)} `
                  + `DE${esc("in a browser tab, serving real files")} `
                  + "VEinpage\\s0.1");
        user.state = "IDENTIFY";
    }

    onInf(user, params) {
        /* params[0] is the sender's SID, not a field. Parsing it as one
           put a key "AA" with value "AC" into the user's INF, and the
           echo then read `BINF AAAC AAAC ID...` -- the SID twice. */
        for (const p of params.slice(1)) {
            if (p.length >= 2)
                user.inf[p.slice(0, 2)] = unesc(p.slice(2));
        }
        user.nick = user.inf.NI || "?";

        if (user.state === "IDENTIFY") {
            user.state = "NORMAL";
            this.welcome(user);
        } else {
            /* Pass it on as it arrived: it already carries the SID. */
            this.broadcast(`B${"INF"} ${params.join(" ")}`);
        }
    }

    infLine(sid, inf) {
        const parts = [`BINF ${sid}`];
        for (const [k, v] of Object.entries(inf))
            parts.push(k + esc(v));
        return parts.join(" ");
    }

    atticInf() {
        const total = this.share.reduce((n, f) => n + f.size, 0);
        return {
            ID: ATTIC_CID,
            NI: this.atticNick,
            DE: "the files are really here",
            SL: "4",
            SS: String(total),
            SF: String(this.share.length),
            I4: this.atticIp,
            U4: String(this.atticPort),
            SU: "TCP4",
            VE: "inpage attic",
        };
    }

    welcome(user) {
        /* The attic first, then everybody else, then themselves: the order
           DC++ expects, and the order that makes the user list right. */
        this.send(user, this.infLine(this.atticSid, this.atticInf()));

        for (const [sid, u] of this.users)
            if (sid !== user.sid && u.state === "NORMAL")
                this.send(user, this.infLine(sid, u.inf));

        this.broadcast(this.infLine(user.sid, user.inf), user.sid);
        this.send(user, this.infLine(user.sid, user.inf));

        this.send(user, "IMSG " + esc(`Welcome to ${HUB_NAME}, `
                                      + `${user.nick}.`));
        this.send(user, "IMSG " + esc(
            "This hub is running inside your browser tab. Nothing here "
            + "touches the internet: search for BOOM and download it."));
        this.send(user, "IMSG " + esc(
            "Everything shared here is free software from the FreeDOS "
            + "repositories -- no shareware, no commercial games. BOOM is "
            + "the GPL Doom engine with Freedoom's BSD game data. See "
            + "CREDITS.txt beside this page."));

        this.log(`[${user.sid}] logged in as ${user.nick}`);
        this.event("login", { sid: user.sid, nick: user.nick });
    }

    onMsg(user, kind, params) {
        if (user.state !== "NORMAL")
            return;
        if (kind === "B" && params.length >= 2) {
            this.broadcast(`BMSG ${user.sid} ${params[1]}`);
            this.event("chat", { nick: user.nick, text: unesc(params[1]) });
        } else if (kind === "D" && params.length >= 3) {
            const target = this.users.get(params[1]);
            if (target)
                this.send(target,
                          `DMSG ${user.sid} ${params[1]} ${params[2]} `
                          + `PM${user.sid}`);
        }
    }

    routeDirect(user, line, params) {
        if (params.length < 2)
            return;

        /* "Reverse connect to me": the client is passive and wants the
           attic to dial it. The attic cannot -- nothing can dial into a
           browser tab -- so it answers with "connect to me" instead, which
           is the whole reason downloads work here. */
        if (line.startsWith("DRCM ") && params[1] === this.atticSid) {
            const token = params[3] || "";
            this.send(user, `DCTM ${this.atticSid} ${params[0]} ADC/1.0 `
                      + `${this.atticPort} ${token}`);
            return;
        }

        const target = this.users.get(params[1]);
        if (target && target.state === "NORMAL")
            this.send(target, line);
    }

    onSch(user, params) {
        if (user.state !== "NORMAL")
            return;

        const terms = [];
        const excludes = [];
        const fields = {};

        for (const p of params.slice(1)) {
            if (p.length < 2)
                continue;
            const key = p.slice(0, 2);
            const val = unesc(p.slice(2));
            if (key === "AN") terms.push(val.toLowerCase());
            else if (key === "NO") excludes.push(val.toLowerCase());
            else fields[key] = val;
        }

        const token = fields.TO || "";
        const wantTth = fields.TR;
        const ge = parseInt(fields.GE || "0", 10) || 0;
        const le = parseInt(fields.LE || "0", 10) || 0;

        let hits = 0;
        for (const f of this.share) {
            const low = f.name.toLowerCase();

            if (wantTth) {
                if (f.tth !== wantTth) continue;
            } else {
                if (terms.length && !terms.every((t) => low.includes(t)))
                    continue;
                if (excludes.some((x) => low.includes(x)))
                    continue;
            }
            if (ge && f.size < ge) continue;
            if (le && f.size > le) continue;

            this.send(user, `DRES ${this.atticSid} ${user.sid} `
                      + `FN${esc(f.name)} SI${f.size} SL4 TR${f.tth}`
                      + (token ? ` TO${esc(token)}` : ""));
            hits++;
        }

        this.log(`[${user.sid}] search -> ${hits} file(s)`);
        this.event("search", { terms: terms.join(" "), hits });
    }

    /* ------------------------------------------------------- the attic */

    acceptPeer(stream) {
        /* Both sides send CSUP at once -- but not in the same task as the
           guest's CONNECT. Over a real socket the connect costs a round
           trip; here the listener runs immediately, and bytes written
           before the guest's TCP socket had finished opening were dropped
           and the connection closed before a single line arrived. One
           turn of the event loop is the whole fix. */
        setTimeout(() => {
            if (stream.closed)
                return;
            stream.writeLine("CSUP ADBASE ADTIGR ADBZIP");
            stream.writeLine(`CINF ID${ATTIC_CID}`);
        }, 0);

        const conn = new Connection(stream, (line) => {
            this.log(`[attic] --> ${line}`);
            this.peerLine(stream, line).catch((e) =>
                this.log(`[attic] ${e && e.message}`));
        });
        void conn;

        stream.onClose = () => this.log("[attic] peer gone");
    }

    async peerLine(stream, line) {
        if (line.length < 4)
            return;

        const cmd = line.slice(1, 4);
        const params = line.length > 5
            ? line.slice(5).split(" ").filter(Boolean) : [];

        if (cmd === "SUP" || cmd === "INF")
            return;                 /* ours already went out */

        if (cmd !== "GET") {
            this.log(`[attic] ignored C${cmd}`);
            return;
        }

        if (params.length < 4) {
            stream.writeLine("CSTA 140 Malformed\\sGET");
            return;
        }

        const kind = params[0];
        const ident = unesc(params[1]);
        const start = parseInt(params[2], 10);
        let count = parseInt(params[3], 10);

        if (kind === "tthl") {
            /* Honest refusal: the leaves are not published, and the client
               falls back to the whole file -- a path with its own rig
               (test/host/run-attic-test.sh), because it used to be fatal. */
            stream.writeLine(
                "CSTA 151 Leaf\\shashes\\snot\\savailable\\shere");
            return;
        }

        if (kind === "list") {
            const xml = this.filesXml(ident || "/");
            stream.writeLine(`CSND list ${esc(ident)} 0 ${xml.length}`);
            stream.write(xml);
            return;
        }

        if (kind !== "file") {
            stream.writeLine("CSTA 151 Unsupported\\srequest\\stype");
            return;
        }

        const file = ident.startsWith("TTH/")
            ? this.share.find((f) => f.tth === ident.slice(4))
            : this.share.find((f) =>
                f.name.toLowerCase() === ident.replace(/^\//, "")
                    .toLowerCase());

        if (!file) {
            stream.writeLine("CSTA 151 File\\snot\\savailable");
            return;
        }
        if (start < 0 || start >= file.size) {
            stream.writeLine("CSTA 152 Invalid\\soffset");
            return;
        }
        if (count < 0 || start + count > file.size)
            count = file.size - start;

        if (!this.fetchFile) {
            stream.writeLine("CSTA 151 No\\sway\\sto\\sread\\sthe\\sfile");
            return;
        }

        this.log(`[attic] sending ${file.name} [${start}..${start + count})`);
        this.event("upload", { name: file.name, size: count });

        const bytes = await this.fetchFile(file, start, count);

        stream.writeLine(`CSND file ${esc(ident)} ${start} ${count}`);

        /* In chunks, so a 7MB game does not become one 7MB frame: v86's
           adapter has a send window and the guest has a 16KB TCP buffer. */
        const CHUNK = 16 * 1024;
        for (let i = 0; i < bytes.length; i += CHUNK) {
            stream.write(bytes.subarray(i, Math.min(i + CHUNK,
                                                    bytes.length)));
            this.served += Math.min(CHUNK, bytes.length - i);
            /* Yield, or the whole transfer happens inside one task and the
               emulator never runs to receive it. */
            await new Promise((r) => setTimeout(r, 0));
        }

        this.log(`[attic] sent ${bytes.length} byte(s) of ${file.name}`);
    }

    filesXml(base) {
        let want = base.startsWith("/") ? base : "/" + base;
        want = want.replace(/\/+$/, "") + "/";

        const xmlAttr = (s) => String(s).replace(/&/g, "&amp;")
            .replace(/</g, "&lt;").replace(/>/g, "&gt;")
            .replace(/"/g, "&quot;");

        const tree = {};
        for (const f of this.share) {
            const full = "/" + f.name;
            if (!full.startsWith(want))
                continue;
            const parts = full.slice(want.length).split("/");
            let node = tree;
            for (const p of parts.slice(0, -1)) {
                node[p] = node[p] || {};
                node = node[p];
            }
            node[""] = node[""] || [];
            node[""].push(f);
        }

        const out = [
            '<?xml version="1.0" encoding="utf-8" standalone="yes"?>',
            `<FileListing Version="1" CID="${ATTIC_CID}" `
            + `Base="${xmlAttr(want)}" Generator="inpage attic">`,
        ];

        const emit = (node, depth) => {
            const pad = " ".repeat(depth);
            for (const name of Object.keys(node).filter((k) => k !== "")
                    .sort()) {
                out.push(`${pad}<Directory Name="${xmlAttr(name)}">`);
                emit(node[name], depth + 1);
                out.push(`${pad}</Directory>`);
            }
            for (const f of (node[""] || []).sort((a, b) =>
                    a.name.localeCompare(b.name))) {
                out.push(`${pad}<File Name="`
                         + `${xmlAttr(f.name.split("/").pop())}" `
                         + `Size="${f.size}" TTH="${f.tth}"/>`);
            }
        };

        emit(tree, 1);
        out.push("</FileListing>");
        return enc.encode(out.join("\n") + "\n");
    }
}

/**
 * Parse SHARE.TXT -- the same file the Python hub reads, with the hashes
 * tools/mkshare.sh computed. The client verifies every download against
 * them, so a wrong one is a corrupt transfer rather than a wrong file.
 */
export function parseShare(text) {
    const files = [];
    const problems = [];

    for (const [i, raw] of text.split("\n").entries()) {
        const line = raw.trim();
        if (!line || line.startsWith("#"))
            continue;

        const bits = line.split(/\s+/);
        if (bits.length < 3) {
            problems.push(`SHARE.TXT:${i + 1}: cannot parse ${line}`);
            continue;
        }

        const [tth, size, ...rest] = bits;
        const name = rest.join(" ");

        if (tth.length !== 39) {
            problems.push(`${name}: ${tth} is not a 39-character TTH`);
            continue;
        }
        files.push({ name, size: Number(size), tth });
    }

    return { files, problems };
}
