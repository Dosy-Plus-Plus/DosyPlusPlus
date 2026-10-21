/* midi.mjs - does the MIDI stream get out of the emulated MPU-401?
 *
 *   node test/v86/midi.mjs <boot.img> <hda.img> [timeout-s]
 *
 * Boots the browser host's own images with AUTOMIDI.BAT, attaches the
 * page's own tap (web/midi.mjs -- the same module, not a copy of it) and
 * checks the bytes that arrive are exactly the MIDI that
 * test/dos/miditest.c plays.
 *
 * That is the half of browser MIDI a rig can settle. v86 answers the
 * guest's probe at 0x330 and discards every data byte, so the question
 * worth asking is whether the data can be recovered at all, byte for
 * byte, from outside the engine. What a synthesiser then makes of it
 * needs ears, and the page says so.
 *
 * No network here, deliberately: this needs no hub and no relay, which
 * makes a failure unambiguously about the MPU-401.
 */
import { existsSync, readFileSync, statSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { V86 } from "../../web/vendor/libv86.mjs";
import { listen } from "../../web/midi.mjs";
import { writeWav } from "./wav.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(here, "../..");
const vendor = resolve(root, "web/vendor");

const bootImg = process.argv[2] || resolve(root, "web/images/boot.img");
const hdaImg = process.argv[3] || resolve(root, "web/images/dosypp.img");
const timeoutS = Number(process.argv[4] || 180);

const buffer = (p) => {
    const b = readFileSync(p);
    return b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength);
};

console.log(`[harness] boot.img: ${bootImg}`);
console.log(`[harness] hda     : ${hdaImg} (${statSync(hdaImg).size} bytes)`);

const emulator = new V86({
    wasm_path: resolve(vendor, "v86.wasm"),
    bios: { buffer: buffer(resolve(vendor, "seabios.bin")) },
    vga_bios: { buffer: buffer(resolve(vendor, "vgabios.bin")) },
    fda: { buffer: buffer(bootImg) },
    hda: { buffer: buffer(hdaImg) },
    boot_order: 0x321,
    memory_size: 16 * 1024 * 1024,
    vga_memory_size: 2 * 1024 * 1024,
    uart1: true,
    autostart: true,
    disable_speaker: true,
});

/* Messages with the time they arrived, so what gets rendered afterwards
   has the gaps the guest left between its chords. An envelope with the
   right shape is a much stronger claim than "energy was produced". */
const messages = [];
const events = [];
const started = Date.now();
let serial = "";
let settled = false;
let tap = null;

emulator.add_listener("emulator-ready", () => {
    /* The devices register their I/O ports when the machine is built, so
       this is the earliest the port exists to be wrapped -- and it is
       before DOS has booted, let alone run anything. */
    tap = listen(emulator, (msg) => {
        messages.push(msg);
        events.push({ at: Date.now() - started, data: msg });
    });
    console.log("[harness] tapped the MPU-401 data port at 330h");
});

emulator.add_listener("serial0-output-byte", (byte) => {
    serial += String.fromCharCode(byte);
    if (serial.includes("AUTOMIDI-DONE"))
        finish();
});

const hex = (a) => [...a].map((b) => b.toString(16).padStart(2, "0")).join(" ");

/* What test/dos/miditest.c plays, in order. Written out rather than
   computed from the program, so that a change to either has to be a
   deliberate change to both. */
const EXPECTED = [
    "c0 13",                       /* program change: church organ      */
    "b0 07 7f",                    /* channel volume, full              */
    "90 3c 64", "90 40 64", "90 43 64",     /* C major, velocity 100    */
    "80 3c 40", "80 40 40", "80 43 40",     /* released                 */
    "90 3c 64", "90 40 64", "90 43 64",     /* and again                */
    "80 3c 40", "80 40 40", "80 43 40",
];

async function finish() {
    if (settled) return;
    settled = true;

    if (tap) tap.detach();

    console.log("\n--- COM1 ---");
    console.log(serial.replace(/\r/g, "").trimEnd() || "(nothing)");

    const got = messages.map(hex);
    console.log("\n--- MIDI recovered from port 330h ---");
    for (const m of got) console.log(`  ${m}`);
    console.log(`  (${tap ? tap.stream.summary() : "no tap"})`);

    let bad = 0;
    const fail = (why) => { console.log(`FAIL: ${why}`); bad++; };

    if (!/MIDI: reset acknowledged/.test(serial))
        fail("the guest found no MPU-401 at 330h -- v86 should answer a "
             + "reset even though it discards the data");

    /* Subsequence rather than equality: the all-notes-off sweep at the end
       of the program is deliberately not listed, and a future version of
       the program may add setup messages. Order is what matters. */
    let i = 0;
    for (const want of EXPECTED) {
        const at = got.indexOf(want, i);
        if (at < 0) {
            fail(`never saw "${want}" after the messages before it`);
            break;
        }
        i = at + 1;
    }

    const notes = tap ? tap.stream.notes : 0;
    if (notes !== 6)
        fail(`${notes} notes played, expected 6 (two chords of three)`);

    const strays = tap ? tap.stream.dropped : 0;
    if (strays)
        fail(`${strays} byte(s) could not be assigned to a message -- the `
             + "stream was misread, or bytes were lost");

    /* The all-notes-off sweep: 16 controller messages, which is how the
       program leaves the synth quiet. Their absence would mean a note
       ringing for ever in the page. */
    const allOff = got.filter((m) => /^b[0-9a-f] 7b 00$/.test(m)).length;
    if (allOff !== 16)
        fail(`${allOff} all-notes-off messages, expected 16 (one per channel)`);

    try { emulator.destroy(); } catch { /* already gone */ }

    /* And what a listener would have heard. The page plays this stream
       through the same synthesiser (web/synth.mjs), so rendering it here
       and measuring the result is a measurement of the demo rather than of
       a stand-in for it. Skipped, loudly, when the synth or the SoundFont
       has not been fetched -- the bytes above are still checked. */
    const rendered = await renderIfPossible(events);
    if (rendered)
        console.log(`[harness] wrote ${rendered} for measurement`);
    else
        console.log("[harness] no audio rendered (see above); the MIDI "
                    + "stream itself was still checked");

    if (bad === 0) {
        console.log("\nV86 MIDI OK: the guest wrote MIDI to the emulated "
                    + "MPU-401 and every byte was");
        console.log("recovered outside the engine, in order, with nothing "
                    + "stray.");
        process.exit(0);
    }
    console.log("\nV86 MIDI FAILED");
    process.exit(1);
}

/* ------------------------------------------------------------- the audio */

const SF_CANDIDATES = [
    process.env.DOSYPP_SF2,
    resolve(root, "dosbox-x/sf2/MuseScore_General.sf3"),
].filter(Boolean);

const WAV_OUT = process.env.MIDI_WAV
    || resolve(root, "build/v86midi/rendered.wav");

async function renderIfPossible(events) {
    const synthPath = resolve(root, "web/vendor/synth/spessasynth_core.js");

    if (!existsSync(synthPath)) {
        console.log("[harness] no synthesiser at web/vendor/synth -- run: "
                    + "tools/fetchsynth.sh");
        return null;
    }

    const sf = SF_CANDIDATES.find((p) => existsSync(p));
    if (!sf) {
        console.log("[harness] no SoundFont -- run: make sf2 (or set "
                    + "DOSYPP_SF2)");
        return null;
    }
    if (!events.length) {
        console.log("[harness] nothing to render: no MIDI arrived");
        return null;
    }

    const { renderEvents } = await import("../../web/synth.mjs");
    const bytes = readFileSync(sf);
    const buf = bytes.buffer.slice(bytes.byteOffset,
                                   bytes.byteOffset + bytes.byteLength);

    console.log(`[harness] rendering ${events.length} messages through `
                + `${sf.split("/").pop()}`);
    const out = await renderEvents(events, { soundFont: buf });
    console.log(`[harness] ${out.presets} presets, ${out.messages} messages `
                + `accepted, ${(out.left.length / out.sampleRate).toFixed(1)}s`);

    writeWav(WAV_OUT, out);
    return WAV_OUT;
}

let ticks = 0;
setInterval(() => {
    ticks += 5;
    if (!settled)
        console.log(`[harness] ${ticks}s, ${messages.length} MIDI messages, `
                    + `${serial.length} bytes from COM1`);
}, 5000).unref?.();

setTimeout(() => {
    console.log(`[harness] TIMEOUT after ${timeoutS}s`);
    finish();
}, timeoutS * 1000);
