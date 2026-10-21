/* midiworklet.mjs - the synthesiser, on the audio thread.
 *
 * Game MIDI arrives on the main thread (web/midi.mjs taps the emulated
 * MPU-401 as the guest writes to it) and has to be turned into sound
 * without ever making the audio callback wait. So the synthesiser lives
 * here, in an AudioWorklet, and the main thread only posts messages at it.
 *
 * Why not render on the main thread: that thread is also emulating a 486.
 * A glitch in the emulator would become a glitch in the music, and the
 * music is the thing being demonstrated.
 *
 * Messages in (port.postMessage):
 *   {type: "soundfont", buffer}  an .sf2/.sf3's bytes; transferred, not copied
 *   {type: "midi", data}         one MIDI message, exactly as the guest wrote it
 *   {type: "panic"}              all notes off
 *
 * Messages out:
 *   {type: "ready", presets}     a SoundFont is loaded and it will make sound
 *   {type: "error", message}     said out loud: silence needs an explanation
 */
import { Synth } from "./synth.mjs";

class MidiWorklet extends AudioWorkletProcessor {
    constructor() {
        super();

        this.synth = new Synth(sampleRate);
        this.loaded = false;

        /* Messages that arrive before the SoundFont does are kept: a game
           sets up its patches and controllers in the first moments, and
           dropping those makes everything after them sound wrong. */
        this.queued = [];

        this.port.onmessage = (e) => this.onmessage(e.data);
    }

    async onmessage(msg) {
        try {
            if (msg.type === "soundfont") {
                const presets = await this.synth.loadSoundFont(msg.buffer);
                this.loaded = true;

                for (const m of this.queued)
                    this.synth.send(m);
                const queued = this.queued.length;
                this.queued = [];

                this.port.postMessage({ type: "ready", presets, queued });
                return;
            }

            if (msg.type === "midi") {
                if (this.loaded)
                    this.synth.send(msg.data);
                else if (this.queued.length < 4096)
                    this.queued.push(msg.data);
                return;
            }

            if (msg.type === "panic") {
                this.synth.allOff();
                return;
            }
        } catch (e) {
            this.port.postMessage({
                type: "error",
                message: (e && e.message) || String(e),
            });
        }
    }

    process(inputs, outputs) {
        const out = outputs[0];

        if (!this.loaded || !out || out.length < 2)
            return true;            /* silence, but keep the node alive */

        /* The block is 128 frames and the synth renders in blocks of that
           size anyway, so this is one call. */
        this.synth.render(out[0], out[1], 0, out[0].length);
        return true;
    }
}

registerProcessor("midi-worklet", MidiWorklet);
