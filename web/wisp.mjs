/* wisp.mjs - the relay, in the page.
 *
 * v86 gives the guest's Ethernet to a Wisp adapter, which opens one
 * WebSocket and multiplexes every TCP connection the guest makes over it.
 * Something at the far end turns those streams into real sockets, and a
 * browser cannot: there is no TCP in a page, and compiling a relay to
 * WebAssembly does not change that, because wasm reaches the network only
 * through the same JS APIs.
 *
 * So the far end moves into the page. Nothing becomes TCP at all: a Wisp
 * stream is handed to a listener in JavaScript, and the "hub" the DOS
 * client dials is an object a few hundred lines away (web/adchub.mjs).
 * No relay, no server, nothing to install, and it works from a static
 * host exactly as published.
 *
 * Two pieces:
 *
 *   WispServer   speaks the protocol tools/wisprelay.py speaks, against
 *                in-page listeners instead of sockets
 *   installInPageSocket()
 *                makes `new WebSocket(...)` return one of these for one
 *                specific URL, because v86 constructs its own socket from
 *                the global and there is no way to hand it an object
 *
 * The frame format is Wisp v1, taken from the relay rather than from a
 * specification, so that both ends of this project agree:
 *
 *     [u8 type][u32le stream id][payload]
 *
 *     CONNECT  0x01  [u8 stream type][u16le port][hostname...]
 *     DATA     0x02  bytes
 *     CONTINUE 0x03  [u32le how many more DATA packets may be sent]
 *     CLOSE    0x04  [u8 reason]
 */

export const PKT_CONNECT = 0x01;
export const PKT_DATA = 0x02;
export const PKT_CONTINUE = 0x03;
export const PKT_CLOSE = 0x04;

export const STREAM_TCP = 0x01;

export const CLOSE_VOLUNTARY = 0x02;
export const CLOSE_INVALID_INFO = 0x41;
export const CLOSE_UNREACHABLE = 0x42;
export const CLOSE_REFUSED = 0x44;
export const CLOSE_BLOCKED = 0x48;

/* How many DATA packets the guest may send before it must wait to be told
   it can send more, and how often we top that up. The relay uses the same
   numbers; a window that never refills is a transfer that stalls at
   exactly 128 packets, which is the kind of bug that looks like a network
   fault. */
const BUFFER_SIZE = 128;
const REFILL_AFTER = 32;

function packet(type, streamId, payload = new Uint8Array(0)) {
    const out = new Uint8Array(5 + payload.length);
    const view = new DataView(out.buffer);

    out[0] = type;
    view.setUint32(1, streamId, true);
    out.set(payload, 5);
    return out;
}

function u32(n) {
    const out = new Uint8Array(4);
    new DataView(out.buffer).setUint32(0, n, true);
    return out;
}

/**
 * One guest TCP connection, as the thing being dialled sees it.
 *
 * Deliberately the shape of a socket and not of a promise: the hub and the
 * serving peer are state machines fed bytes as they arrive, which is what
 * they would be over a real socket too.
 */
class WispStream {
    constructor(server, id, host, port) {
        this.server = server;
        this.id = id;
        this.host = host;
        this.port = port;
        this.closed = false;
        this.onData = null;         /* (Uint8Array) => void */
        this.onClose = null;
        this.sent = 0;
        this.received = 0;
    }

    /** Bytes to the guest. */
    write(bytes) {
        if (this.closed)
            return false;

        const data = bytes instanceof Uint8Array
            ? bytes
            : new Uint8Array(bytes);

        this.sent += data.length;
        this.server.send(packet(PKT_DATA, this.id, data));
        return true;
    }

    /** A string, which is what ADC is made of. */
    writeLine(line) {
        return this.write(new TextEncoder().encode(line + "\n"));
    }

    close(reason = CLOSE_VOLUNTARY) {
        if (this.closed)
            return;
        this.closed = true;
        this.server.send(packet(PKT_CLOSE, this.id, new Uint8Array([reason])));
        this.server.streams.delete(this.id);
        if (this.onClose)
            this.onClose();
    }
}

/**
 * The far end of v86's WebSocket.
 *
 * `listeners` maps "host:port" to a function that is handed each new
 * stream. An address with no listener is refused, exactly as the relay
 * refuses an unmapped one -- which is also what keeps this honest: the
 * guest can reach what this page offers and nothing else, because there is
 * nothing else to reach.
 */
export class WispServer {
    constructor(listeners = {}) {
        this.listeners = listeners;
        this.streams = new Map();
        this.nextSend = null;       /* set by the socket shim */
        this.onLog = null;
        this.connects = [];         /* every address the guest asked for */
    }

    log(msg) {
        if (this.onLog)
            this.onLog(msg);
    }

    /** Called by the shim once the guest's socket is "open". */
    start(send) {
        this.nextSend = send;
        /* The client waits for this before it will send anything. */
        this.send(packet(PKT_CONTINUE, 0, u32(BUFFER_SIZE)));
    }

    send(bytes) {
        if (this.nextSend)
            this.nextSend(bytes);
    }

    /** A frame from the guest. */
    receive(bytes) {
        if (bytes.length < 5)
            return;

        const type = bytes[0];
        const id = new DataView(bytes.buffer, bytes.byteOffset)
            .getUint32(1, true);
        const payload = bytes.subarray(5);

        if (type === PKT_CONNECT)
            this.onConnect(id, payload);
        else if (type === PKT_DATA)
            this.onData(id, payload);
        else if (type === PKT_CLOSE)
            this.onCloseFrame(id);
        /* CONTINUE from the guest would be meaningless: we are the end
           that grants the window, not the one that waits for it. */
    }

    onConnect(id, payload) {
        if (payload.length < 3) {
            this.send(packet(PKT_CLOSE, id,
                             new Uint8Array([CLOSE_INVALID_INFO])));
            return;
        }

        const streamType = payload[0];
        const port = new DataView(payload.buffer, payload.byteOffset)
            .getUint16(1, true);
        const host = new TextDecoder().decode(payload.subarray(3));

        this.connects.push(`${host}:${port}`);

        if (streamType !== STREAM_TCP) {
            /* UDP would be ADC search results; v86 does not forward it. */
            this.log(`refused UDP to ${host}:${port}`);
            this.send(packet(PKT_CLOSE, id, new Uint8Array([CLOSE_BLOCKED])));
            return;
        }

        const listener = this.listeners[`${host}:${port}`]
            || this.listeners[host];

        if (!listener) {
            this.log(`nothing is listening on ${host}:${port}`);
            this.send(packet(PKT_CLOSE, id,
                             new Uint8Array([CLOSE_UNREACHABLE])));
            return;
        }

        const stream = new WispStream(this, id, host, port);
        this.streams.set(id, stream);
        this.send(packet(PKT_CONTINUE, id, u32(BUFFER_SIZE)));
        this.log(`connected to ${host}:${port}`);

        try {
            listener(stream);
        } catch (e) {
            this.log(`listener for ${host}:${port} threw: ${e && e.message}`);
            stream.close(CLOSE_REFUSED);
        }
    }

    onData(id, payload) {
        const stream = this.streams.get(id);

        if (!stream)
            return;

        stream.received += payload.length;

        /* Top the window up before it runs out. */
        if (stream.received % (REFILL_AFTER * 1024) < payload.length)
            this.send(packet(PKT_CONTINUE, id, u32(BUFFER_SIZE)));

        if (stream.onData) {
            /* A copy: the caller may keep it, and the buffer underneath is
               v86's and will be reused. */
            stream.onData(new Uint8Array(payload));
        }
    }

    onCloseFrame(id) {
        const stream = this.streams.get(id);

        if (!stream)
            return;
        stream.closed = true;
        this.streams.delete(id);
        if (stream.onClose)
            stream.onClose();
    }
}

/**
 * Make `new WebSocket(url)` hand back an in-page endpoint for one URL.
 *
 * v86 builds its own socket out of the global constructor, so there is no
 * seam to pass an object through -- this is the seam. Every other URL goes
 * to the real WebSocket, which matters because the page still probes a
 * relay with one.
 *
 * Returns a function that puts the global back.
 */
export function installInPageSocket(server, host = "in-page") {
    const Real = globalThis.WebSocket;
    const match = `ws://${host}`;

    function Shim(url, protocols) {
        if (typeof url !== "string" || !url.startsWith(match))
            return new Real(url, protocols);

        const sock = {
            url,
            binaryType: "arraybuffer",
            readyState: 0,
            onopen: null,
            onmessage: null,
            onclose: null,
            onerror: null,
            send(data) {
                const bytes = data instanceof ArrayBuffer
                    ? new Uint8Array(data)
                    : new Uint8Array(data.buffer || data);
                server.receive(bytes);
            },
            close() {
                sock.readyState = 3;
                if (sock.onclose)
                    sock.onclose({ code: 1000 });
            },
            addEventListener(type, fn) {
                if (type === "message") sock.onmessage = fn;
                else if (type === "open") sock.onopen = fn;
                else if (type === "close") sock.onclose = fn;
            },
            removeEventListener() {},
        };

        /* Open on the next turn, not this one: v86 assigns its handlers
           after the constructor returns, and an immediately-open socket
           would deliver the first CONTINUE into a null onmessage. */
        setTimeout(() => {
            sock.readyState = 1;
            if (sock.onopen)
                sock.onopen({});
            server.start((bytes) => {
                if (sock.onmessage) {
                    /* v86 wants an ArrayBuffer, as from a real socket. */
                    const copy = bytes.slice();
                    sock.onmessage({ data: copy.buffer });
                }
            });
        }, 0);

        return sock;
    }

    /* Keep the constants and the prototype: anything feature-detecting a
       WebSocket should still find one. */
    Shim.prototype = Real ? Real.prototype : {};
    for (const k of ["CONNECTING", "OPEN", "CLOSING", "CLOSED"])
        if (Real) Shim[k] = Real[k];

    globalThis.WebSocket = Shim;

    return function uninstall() {
        globalThis.WebSocket = Real;
    };
}
