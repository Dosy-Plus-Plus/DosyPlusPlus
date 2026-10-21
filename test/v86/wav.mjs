/* wav.mjs - write rendered audio where test/dos/midirms.py can read it.
 *
 * Shared by the two browser MIDI rigs so that both are judged by the same
 * listener as the DOSBox-X one: 16-bit stereo PCM, the format that script
 * expects and the only one it will accept.
 */
import { mkdirSync, writeFileSync } from "node:fs";
import { dirname } from "node:path";

export function writeWav(path, { left, right, sampleRate }) {
    const frames = left.length;
    const data = Buffer.alloc(frames * 4);

    for (let i = 0; i < frames; i++) {
        /* Clamped, not scaled: a synthesiser that overshoots should be
           heard as clipping, which is a real fault, rather than quietly
           normalised into sounding fine. */
        const l = Math.max(-1, Math.min(1, left[i]));
        const r = Math.max(-1, Math.min(1, right[i]));
        data.writeInt16LE((l * 32767) | 0, i * 4);
        data.writeInt16LE((r * 32767) | 0, i * 4 + 2);
    }

    const head = Buffer.alloc(44);
    head.write("RIFF", 0);
    head.writeUInt32LE(36 + data.length, 4);
    head.write("WAVEfmt ", 8);
    head.writeUInt32LE(16, 16);
    head.writeUInt16LE(1, 20);              /* PCM */
    head.writeUInt16LE(2, 22);              /* stereo */
    head.writeUInt32LE(sampleRate, 24);
    head.writeUInt32LE(sampleRate * 4, 28);
    head.writeUInt16LE(4, 32);
    head.writeUInt16LE(16, 34);
    head.write("data", 36);
    head.writeUInt32LE(data.length, 40);

    mkdirSync(dirname(path), { recursive: true });
    writeFileSync(path, Buffer.concat([head, data]));
    return path;
}
