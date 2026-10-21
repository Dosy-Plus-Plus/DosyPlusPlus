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

setTimeout(async () => {
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
        fail(`${s ? s.notes : 0} notes in ${seconds}s. Doom's title music `
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
}, seconds * 1000);
