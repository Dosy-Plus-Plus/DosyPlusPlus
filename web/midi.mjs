/* midi.mjs - take the MIDI stream out of the emulated MPU-401.
 *
 * A DOS game that offers you "Roland MPU-401" writes MIDI bytes to port
 * 0x330 and expects a synthesiser on the other end of them. v86 emulates
 * just enough of the interface for the game's probe to succeed and then
 * throws every data byte away:
 *
 *     port3x0_write = function(a){ dbg_log(a) }                  // discarded
 *     port3x1_write = function(a){ 255===a && ack(0xFE) }         // reset
 *
 * So a game in the browser demo detects a Roland and plays perfect
 * silence, which is worse than finding nothing: with no MPU-401 at all it
 * would have fallen back to the Sound Blaster.
 *
 * This takes the bytes. v86's I/O table is a plain object per port with
 * the handler the CPU calls (`entry.write8.call(entry.device, value)`), so
 * the write can be wrapped from outside the engine -- no patched vendor
 * file, no fork, and the original handler still runs, which matters
 * because it is what makes the probe succeed.
 *
 * What the bytes are worth depends on what plays them, and that is the
 * caller's problem: WebMIDI to the listener's own synth, a SoundFont in an
 * AudioWorklet, or a counter in a test rig. This module is the pipe, and
 * it is deliberately free of the DOM so that test/web/test-midistream.mjs
 * and the v86 gate can use the same code the page does.
 */

export const MPU_DATA = 0x330;    /* data: MIDI bytes in and out */
export const MPU_CMD = 0x331;     /* command and status */

/* How many data bytes follow each status byte. Indexed by the high nibble,
   which is all that matters for everything except 0xF. */
const DATA_BYTES = {
    0x8: 2,   /* note off          */
    0x9: 2,   /* note on           */
    0xA: 2,   /* aftertouch        */
    0xB: 2,   /* controller        */
    0xC: 1,   /* program change    */
    0xD: 1,   /* channel pressure  */
    0xE: 2,   /* pitch bend        */
};

/* System common messages, which do NOT set running status. */
const SYSTEM_DATA_BYTES = {
    0xF1: 1,  /* MTC quarter frame */
    0xF2: 2,  /* song position     */
    0xF3: 1,  /* song select       */
    0xF6: 0,  /* tune request      */
};

/**
 * A MIDI byte stream, reassembled into messages.
 *
 * The awkward parts of MIDI are all here, and each of them is something a
 * real DOS game does:
 *
 *   - Running status: after one `0x90 note vel`, a game may send bare
 *     `note vel` pairs for every note that follows. Dropping those loses
 *     most of a piece of music.
 *   - Realtime bytes (0xF8-0xFF) may arrive in the MIDDLE of another
 *     message, and must not disturb it.
 *   - Sysex (0xF0 ... 0xF7) is any length, and is how a game uploads MT-32
 *     patches -- which is to say the most important thing in the stream
 *     for anyone wanting the 1994 sound.
 */
export class MidiStream {
    constructor() {
        this.status = 0;          /* running status, 0 for none */
        this.pending = [];        /* data bytes collected so far */
        this.want = 0;            /* how many this message needs */
        this.sysex = null;        /* bytes of a sysex in progress */
        this.bytes = 0;
        this.messages = 0;
        this.notes = 0;           /* note-ons with velocity, for the UI */
        this.sysexes = 0;
        this.dropped = 0;         /* data bytes with no status to belong to */
    }

    /**
     * Feed one byte. Returns an array of completed messages, each a
     * Uint8Array ready to hand to a synthesiser -- usually empty, because
     * most bytes are the middle of something.
     */
    feed(byte) {
        const b = byte & 0xFF;
        this.bytes++;

        /* Realtime: a complete message in one byte, and it may interrupt
           anything without affecting it. Clock bytes arrive constantly
           from a sequencer, so they are passed on and not counted as
           music. */
        if (b >= 0xF8) {
            this.messages++;
            return [new Uint8Array([b])];
        }

        if (this.sysex) {
            if (b === 0xF7) {                  /* end of sysex */
                this.sysex.push(b);
                const msg = new Uint8Array(this.sysex);
                this.sysex = null;
                this.messages++;
                this.sysexes++;
                return [msg];
            }
            if (b & 0x80) {
                /* A status byte inside sysex: the spec says the sysex is
                   abandoned. Real hardware does this, so we do too, rather
                   than growing a buffer for ever. */
                this.sysex = null;
                this.dropped++;
                return this.feed(b);
            }
            this.sysex.push(b);
            return [];
        }

        if (b === 0xF0) {
            this.sysex = [b];
            this.status = 0;                   /* sysex clears it */
            this.pending = [];
            return [];
        }

        if (b & 0x80) {
            const n = b in SYSTEM_DATA_BYTES
                ? SYSTEM_DATA_BYTES[b]
                : DATA_BYTES[b >> 4];

            if (n === undefined) {
                /* 0xF4, 0xF5, 0xF7 out of place: nothing to do with it. */
                this.dropped++;
                return [];
            }

            /* System common messages cancel running status; channel
               messages become it. */
            if (b >= 0xF0) {
                this.status = 0;
            } else {
                this.status = b;
            }

            this.current = b;
            this.pending = [];
            this.want = n;

            if (n === 0) {
                this.messages++;
                return [new Uint8Array([b])];
            }
            return [];
        }

        /* A data byte. Either it completes the message in progress, or
           running status says which message it starts. */
        if (this.want === 0) {
            if (!this.status) {
                this.dropped++;
                return [];
            }
            this.current = this.status;
            this.want = DATA_BYTES[this.status >> 4];
            this.pending = [];
        }

        this.pending.push(b);
        if (this.pending.length < this.want)
            return [];

        const msg = new Uint8Array([this.current, ...this.pending]);
        this.pending = [];
        this.want = 0;
        this.messages++;

        /* A note-on with zero velocity is a note-off; counting it as a
           note played would overstate what was heard. */
        if ((msg[0] & 0xF0) === 0x90 && msg[2] > 0)
            this.notes++;

        return [msg];
    }

    /** What to show somebody: counts, not bytes. */
    summary() {
        return `${this.bytes} bytes, ${this.messages} messages, ` +
               `${this.notes} notes` +
               (this.sysexes ? `, ${this.sysexes} sysex` : "") +
               (this.dropped ? `, ${this.dropped} stray` : "");
    }
}

/**
 * Wrap the emulated MPU-401's data port and hand every byte written to it
 * to `onByte`. Returns a function that puts v86 back as it was.
 *
 * `emulator` is a running V86. This must be called after the machine
 * exists (its devices register their ports when constructed) -- the
 * "emulator-ready" listener is the place.
 */
export function tapMpu401(emulator, onByte, port = MPU_DATA) {
    const io = emulator && emulator.v86 && emulator.v86.cpu
        && emulator.v86.cpu.io;

    if (!io || !io.ports || !io.ports[port]) {
        throw new Error(
            `v86 has no I/O port ${port.toString(16)}h to tap. With an ` +
            `SB16 configured it registers the MPU-401 there; without one ` +
            `there is nothing for a game to find either.`);
    }

    const entry = io.ports[port];
    const original = entry.write8;

    entry.write8 = function (value) {
        try {
            onByte(value & 0xFF);
        } catch (e) {
            /* A broken sink must not break the emulated machine: the guest
               is in the middle of an OUT instruction. */
            if (typeof console !== "undefined")
                console.error("MIDI sink threw:", e);
        }
        /* The original handler still runs. It discards the byte, but it is
           also what keeps the device's own state consistent. */
        return original.call(this, value);
    };

    return function detach() {
        entry.write8 = original;
    };
}

/**
 * The whole chain in one call: tap the port, reassemble the stream, hand
 * complete messages to `onMessage`. Returns {stream, detach}.
 */
export function listen(emulator, onMessage) {
    const stream = new MidiStream();

    const detach = tapMpu401(emulator, (byte) => {
        for (const msg of stream.feed(byte))
            onMessage(msg, stream);
    });

    return { stream, detach };
}
