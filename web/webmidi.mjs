/* webmidi.mjs - send the game's MIDI to a synthesiser you already own.
 *
 * The other sink for the stream web/midi.mjs takes off the emulated
 * MPU-401. web/synth.mjs renders it here in the page, which costs a 40MB
 * SoundFont download; this forwards it out of the browser instead, to
 * whatever is plugged in or running on the machine -- a Roland module, a
 * keyboard, a software synth, Windows' own GS Wavetable. On a desk with an
 * SC-55 on it, this is the best this demo can possibly sound: a 1994 DOS
 * game driving the exact hardware it was written for, with an emulator and
 * a browser in between.
 *
 * Nothing here touches the DOM, and the Web MIDI entry point is injected
 * rather than reached for, so test/web/test-webmidi.mjs can drive the whole
 * thing with a stub and check what would have been sent. What that cannot
 * cover is a browser's own implementation: Safari has none at the time of
 * writing, Firefox asks permission per site, and Chrome may too.
 *
 * Two awkward details, both of which change what a listener hears:
 *
 *   - Sysex needs its own permission, and a browser may refuse it while
 *     allowing everything else. A game uploading MT-32 patches sends
 *     mostly sysex, so silently dropping it would leave the music playing
 *     with the wrong instruments. Refused sysex is therefore counted and
 *     reported, not swallowed.
 *   - A note is held until something releases it. If this stops mid-tune
 *     -- the visitor switches sink, or closes the tab -- the note stays on
 *     in somebody's hardware until they power-cycle it. So stopping sends
 *     all-notes-off, on every channel, always.
 */

/** Is there a Web MIDI implementation at all? */
export function available(nav = globalThis.navigator) {
    return !!(nav && typeof nav.requestMIDIAccess === "function");
}

export class WebMidiOut {
    /**
     * @param requestAccess  navigator.requestMIDIAccess, or a stub.
     */
    constructor(requestAccess) {
        this.requestAccess = requestAccess;
        this.access = null;
        this.output = null;
        this.sysexAllowed = false;
        this.sent = 0;
        this.sysexDropped = 0;
        this.failed = 0;
        this.lastError = "";
        this.onoutputs = null;      /* called when the device list changes */
    }

    /**
     * Ask for access. Tries with sysex first and falls back without it,
     * because a browser that refuses sysex should still give you notes --
     * and because which of the two happened is worth telling the listener.
     *
     * Returns the list of outputs.
     */
    async open() {
        if (!this.requestAccess)
            throw new Error("this browser has no Web MIDI (Safari does not, "
                            + "and Firefox may need it enabled)");

        try {
            this.access = await this.requestAccess({ sysex: true });
            this.sysexAllowed = true;
        } catch (e) {
            /* Denied, or not supported with sysex. Either way, try again
               for the ordinary case before giving up. */
            this.lastError = (e && e.message) || String(e);
            this.access = await this.requestAccess({ sysex: false });
            this.sysexAllowed = false;
        }

        if (this.access && "onstatechange" in this.access) {
            this.access.onstatechange = () => {
                if (this.onoutputs)
                    this.onoutputs(this.outputs());
                /* A device unplugged mid-tune is not an error, but it is
                   the end of the music, and the page should say so rather
                   than look as though it is still playing. */
                if (this.output && !this.outputs()
                        .some((o) => o.id === this.output.id)) {
                    this.lastError = `${this.output.name} went away`;
                    this.output = null;
                }
            };
        }

        return this.outputs();
    }

    /** Every output the browser will admit to, as {id, name}. */
    outputs() {
        if (!this.access || !this.access.outputs)
            return [];

        const out = [];
        /* A MIDIOutputMap is a Map in every implementation, but it is
           specified as a maplike and some polyfills are iterable only. */
        const it = typeof this.access.outputs.values === "function"
            ? this.access.outputs.values()
            : this.access.outputs;

        for (const o of it)
            out.push(o);
        return out;
    }

    /** Choose one, by the id the browser gave it. Returns its name. */
    use(id) {
        const found = this.outputs().find((o) => o.id === id);

        if (!found) {
            this.lastError = `no MIDI output with id ${id}`;
            return null;
        }
        if (this.output && this.output !== found)
            this.panic();           /* do not leave notes on the old one */

        this.output = found;
        this.lastError = "";
        return found.name;
    }

    /** One message, exactly as the guest wrote it to the MPU-401. */
    send(message) {
        if (!this.output || !message || !message.length)
            return false;

        if (message[0] === 0xF0 && !this.sysexAllowed) {
            /* Sending it would throw, once per message, for the rest of
               the tune. Counted instead, so the page can say that an
               MT-32 game's patches are not reaching the synth. */
            this.sysexDropped++;
            return false;
        }

        try {
            this.output.send(message);
            this.sent++;
            return true;
        } catch (e) {
            this.failed++;
            this.lastError = (e && e.message) || String(e);
            return false;
        }
    }

    /** All notes off, on every channel. Cheap insurance against a drone. */
    panic() {
        if (!this.output)
            return;

        for (let ch = 0; ch < 16; ch++) {
            try {
                this.output.send(new Uint8Array([0xB0 | ch, 123, 0]));
                this.output.send(new Uint8Array([0xB0 | ch, 120, 0]));
            } catch { /* going away is the usual reason, and it is fine */ }
        }
    }

    close() {
        this.panic();
        this.output = null;
    }

    summary() {
        if (!this.output)
            return "no output selected";
        return `${this.output.name}: ${this.sent} messages`
            + (this.sysexDropped
                ? `, ${this.sysexDropped} sysex dropped (not permitted)`
                : "")
            + (this.failed ? `, ${this.failed} refused` : "");
    }
}
