/* doom.mjs - a real game's music, out of the browser demo.
 *
 *   node test/v86/doom.mjs <boot.img> <doom.img> [seconds] [wav-out]
 *
 * test/v86/midi.mjs proves the MPU-401 tap recovers a known byte sequence.
 * This asks the question that actually matters: does a game anybody would
 * download produce music through it?
 *
 * The game is BOOM 2.02 -- the GPL Doom engine, with the free WAD the
 * FreeDOS package ships, so nothing here is anybody's property but its
 * authors'. It plays its title music on its own, with no input, which is
 * what makes it testable at all.
 *
 * The one setting that matters is music_card = 6 in BOOM.CFG, which is
 * MIDI_MPU in Allegro 3's driver numbering. With the default of -1,
 * autodetect, Boom runs happily and sends not one byte: it finds v86's
 * SB16 and chooses an FM device that v86 does not implement. That is the
 * difference between a demo with a soundtrack and a demo that looks
 * identical and is silent, so the rig that builds this image sets it and
 * mkv86img.sh installs the same file for PLAY BOOM.
 */
import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { V86 } from "../../web/vendor/libv86.mjs";
import { listen } from "../../web/midi.mjs";
import { renderEvents } from "../../web/synth.mjs";
import { writeWav } from "./wav.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(here, "../..");
const vendor = resolve(root, "web/vendor");

const bootImg = process.argv[2];
const hdaImg = process.argv[3];
const seconds = Number(process.argv[4] || 75);
const wavOut = process.argv[5] || resolve(root, "build/v86doom/doom.wav");
const sf = process.env.DOSYPP_SF2
    || resolve(root, "dosbox-x/sf2/MuseScore_General.sf3");

const buffer = (p) => {
    const b = readFileSync(p);
    return b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength);
};

console.log(`[harness] ${hdaImg}, ${seconds}s, 32MB of RAM`);

const emulator = new V86({
    wasm_path: resolve(vendor, "v86.wasm"),
    bios: { buffer: buffer(resolve(vendor, "seabios.bin")) },
    vga_bios: { buffer: buffer(resolve(vendor, "vgabios.bin")) },
    fda: { buffer: buffer(bootImg) },
    hda: { buffer: buffer(hdaImg) },
    boot_order: 0x321,
    /* The page gives the machine 16MB, which is the period-correct
       figure the client is held to; DOOM_MB exists to find out whether
       the games need more. Measured: Doom plays its music in 16. */
    memory_size: Number(process.env.DOOM_MB || 16) * 1024 * 1024,
    vga_memory_size: 2 * 1024 * 1024,
    uart1: true,
    autostart: true,
    disable_speaker: true,
});

const events = [];
const started = Date.now();
let serial = "";
let tap = null;

emulator.add_listener("emulator-ready", () => {
    tap = listen(emulator, (msg) =>
        events.push({ at: Date.now() - started, data: msg }));
    console.log("[harness] tapped 330h before DOS has even booted");
});

emulator.add_listener("serial0-output-byte", (byte) => {
    serial += String.fromCharCode(byte);
});

const ticker = setInterval(() => {
    const s = tap && tap.stream;
    console.log(`[harness] ${((Date.now() - started) / 1000) | 0}s  `
                + (s ? s.summary() : "no tap"));
}, 10000);

/* How much music to collect once it starts, and how long to wait for it
   to start at all.
 *
 * This used to be a single deadline `seconds` after boot, which assumed
 * the game reaches its title screen in a predictable time. On a loaded
 * machine it does not: a run that measured fine here produced its first
 * note at 69 seconds of a 75-second window, and the rig reported "7000ms
 * of audible music, wanted 10000" -- a complaint about Doom's soundtrack
 * that was really a complaint about the boot.
 *
 * So the window starts when the music does. `seconds` is now the budget
 * for getting there; MUSIC_SECS is how much is then recorded. */
const MUSIC_SECS = Number(process.env.DOOM_MUSIC_SECS || 30);

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const collect = async () => {
    const startBudget = Date.now() + seconds * 1000;
    let musicAt = 0;

    while (Date.now() < startBudget) {
        const s = tap && tap.stream;
        if (s && s.notes > 0) {
            musicAt = Date.now();
            console.log(`[harness] first note at `
                        + `${((musicAt - started) / 1000).toFixed(1)}s; `
                        + `recording ${MUSIC_SECS}s of it`);
            break;
        }
        await sleep(250);
    }

    if (!musicAt) {
        console.log(`[harness] no MIDI at all within ${seconds}s -- `
                    + "measuring what there is, which is nothing");
        return;
    }
    while (Date.now() - musicAt < MUSIC_SECS * 1000)
        await sleep(250);
};

const judge = async () => {
    clearInterval(ticker);

    const s = tap && tap.stream;
    console.log("\n--- what the guest said ---");
    console.log(serial.replace(/\r/g, "").trim().split("\n").slice(-12)
                .join("\n") || "(nothing)");

    try { emulator.destroy(); } catch { /* already gone */ }

    let bad = 0;
    const fail = (why) => { console.log(`FAIL: ${why}`); bad++; };

    console.log("\n--- the MIDI it sent ---");
    console.log(`  ${s ? s.summary() : "no tap"}`);

    if (!/Startup v2\.02/.test(serial))
        fail("Boom did not start -- this is not a test of its music");
    if (!s || s.notes < 200)
        fail(`${s ? s.notes : 0} notes in ${MUSIC_SECS}s of music. Doom's `
             + `title music `
             + "is over a thousand; this is silence or a trickle. Check "
             + "music_card = 6 in BOOM.CFG.");
    if (s && s.dropped)
        fail(`${s.dropped} stray byte(s): the stream was misread`);

    console.log("\n--- rendered through the page's synthesiser ---");
    const bytes = readFileSync(sf);
    const out = await renderEvents(events, {
        soundFont: bytes.buffer.slice(bytes.byteOffset,
                                     bytes.byteOffset + bytes.byteLength),
    });
    console.log(`  ${out.presets} presets, ${out.messages} messages, `
                + `${(out.left.length / out.sampleRate).toFixed(1)}s`);
    writeWav(wavOut, out);
    console.log(`  wrote ${wavOut}`);

    process.exit(bad ? 1 : 0);
};

await collect();
await judge();
