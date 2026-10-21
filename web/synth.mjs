/* synth.mjs - turn MIDI messages into sound, the same way in both places.
 *
 * The page needs this inside an AudioWorklet, where there is a sample clock
 * and a speaker. A test rig needs it with neither: no WebAudio, no device,
 * no real time -- just "here are the messages a DOS game sent, render what
 * a listener would have heard so it can be measured".
 *
 * So the synthesis lives here, knowing nothing about either. It renders
 * into Float32Arrays on demand, which is what spessasynth_core offers and
 * the reason it was chosen: the audio the browser demo plays and the audio
 * make test-v86-midi measures come from the same code, so the measurement
 * is about the demo and not about a copy of it.
 *
 * What is NOT here: anything that could make this sound different from the
 * MPU-401 a DOS game thinks it is writing to. The stream goes in
 * unaltered, including sysex -- which is how a game uploads MT-32 patches,
 * and dropping it would quietly change the music.
 */
import {
    SpessaSynthProcessor,
    SoundBankLoader,
    SpessaLog,
} from "./vendor/synth/spessasynth_core.js";

/* The library is chatty on load (every preset, every bank). Useful once. */
try {
    SpessaLog.enabled = false;
} catch { /* a future version may not expose it */ }

export class Synth {
    /**
     * @param sampleRate what the output device runs at.
     */
    constructor(sampleRate) {
        this.sampleRate = sampleRate;
        this.processor = new SpessaSynthProcessor(sampleRate);
        this.ready = this.processor.processorInitialized;
        this.bank = null;
        this.presets = 0;
        this.messages = 0;
    }

    /**
     * Load a SoundFont (.sf2 or .sf3) from its bytes. Until one is loaded
     * the synth runs and renders silence -- which is the single most
     * confusing state this can be in, so callers should check `presets`
     * and say so rather than let it be heard as a broken MIDI path.
     */
    async loadSoundFont(arrayBuffer) {
        await this.ready;
        this.bank = SoundBankLoader.fromArrayBuffer(arrayBuffer);
        this.processor.soundBankManager.addSoundBank(this.bank, "main");
        this.presets = this.bank.presets ? this.bank.presets.length : 0;
        return this.presets;
    }

    /** One MIDI message, exactly as it came off the MPU-401. */
    send(message) {
        if (!message || !message.length)
            return;
        /* Realtime bytes (clock, active sensing) are not for a
           synthesiser and some versions object to them. */
        if (message[0] >= 0xF8)
            return;
        this.processor.processMessage(message);
        this.messages++;
    }

    /**
     * Render `count` frames into the two channels, starting at `offset`.
     * Rendered in blocks because that is how the engine is meant to be
     * driven, and because a single enormous call makes a note's envelope
     * jump rather than move.
     */
    render(left, right, offset = 0, count = null, block = 128) {
        const n = count === null ? left.length - offset : count;

        for (let i = 0; i < n; i += block)
            this.processor.process(left, right, offset + i,
                                   Math.min(block, n - i));
    }

    /** Silence everything, on every channel. */
    allOff() {
        for (let ch = 0; ch < 16; ch++)
            this.send(new Uint8Array([0xB0 | ch, 120, 0]));
    }
}

/**
 * Render a timed stream of MIDI messages to stereo Float32Arrays.
 *
 * `events` is [{at: milliseconds, data: Uint8Array}, ...] -- the timestamps
 * a tap recorded while a guest played. This is what makes a measurement of
 * the result meaningful: the gaps between the notes are the gaps the game
 * left, so the envelope can be checked for the shape the music had rather
 * than merely for energy.
 */
export async function renderEvents(events, {
    sampleRate = 48000,
    soundFont = null,
    tailMs = 1500,
} = {}) {
    const synth = new Synth(sampleRate);
    await synth.ready;

    if (soundFont)
        await synth.loadSoundFont(soundFont);

    const lastAt = events.length ? events[events.length - 1].at : 0;
    const totalMs = lastAt + tailMs;
    const frames = Math.ceil((totalMs * sampleRate) / 1000);
    const left = new Float32Array(frames);
    const right = new Float32Array(frames);

    let cursor = 0;                 /* frames rendered so far */

    for (const ev of events) {
        const upto = Math.min(frames,
                              Math.floor((ev.at * sampleRate) / 1000));
        if (upto > cursor) {
            synth.render(left, right, cursor, upto - cursor);
            cursor = upto;
        }
        synth.send(ev.data);
    }

    if (cursor < frames)
        synth.render(left, right, cursor, frames - cursor);

    return { left, right, sampleRate, presets: synth.presets,
             messages: synth.messages };
}
