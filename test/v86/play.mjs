/* play.mjs - does a downloaded game actually start?
 *
 *   node test/v86/play.mjs <boot.img> <hda.img> [seconds]
 *
 * make test-v86-dl proves a game arrives and unpacks. That is not the
 * same as playing: BLKDROP unpacked perfectly and then died with
 *
 *     unable to find the mouse. it is really necessary.
 *     Fatal Error #243: mouse driver failure
 *
 * because DOS has no int 33h unless something provides one, and nothing
 * did. The demo's disk carries CTMOUSE now, GO.BAT loads it, and this is
 * the check that says so -- by driving the guest's keyboard the way a
 * visitor would and then watching what the screen does.
 *
 * A game in 320x200 draws no text, so the verdict is the video mode plus
 * the absence of the complaints a failure prints. There is no frame
 * buffer to look at in Node and nothing to compare it with if there were.
 */
import { readFileSync } from "node:fs";
import { dirname, resolve, join } from "node:path";
import { fileURLToPath } from "node:url";
import { V86 } from "../../web/vendor/libv86.mjs";
import { WispServer, installInPageSocket } from "../../web/wisp.mjs";
import { AdcHub, parseShare } from "../../web/adchub.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(here, "../..");
const vendor = resolve(root, "web/vendor");

const bootImg = process.argv[2] || resolve(root, "web/images/boot.img");
const hdaImg = process.argv[3] || resolve(root, "web/images/dosypp.img");
const seconds = Number(process.argv[4] || 150);
const shareDir = process.env.WAREZ_DIR || resolve(root, "build/warez");

const buffer = (p) => {
    const b = readFileSync(p);
    return b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength);
};

/* The hub in the page, so the guest boots the way a visitor's would --
   it is what GO.BAT connects to, and a client that cannot log in never
   reaches the DOS prompt this rig needs. */
const { files } = parseShare(readFileSync(join(shareDir, "SHARE.TXT"),
                                          "utf8"));
const hub = new AdcHub({
    share: files,
    fetchFile: async (f, start, count) =>
        readFileSync(join(shareDir, f.name)).subarray(start, start + count),
});
installInPageSocket(new WispServer(hub.listeners()));

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
    net_device: {
        type: "ne2k",
        relay_url: "wisp://in-page/",
        router_ip: "192.168.86.1",
        vm_ip: "192.168.86.100",
    },
});

const rows = [];
emulator.add_listener("screen-put-char", ([r, c, ch]) => {
    rows[r] = rows[r] || [];
    rows[r][c] = String.fromCharCode(ch || 32);
});
const screenText = () => rows
    .map((r) => (r || []).join("").replace(/\s+$/, ""))
    .filter(Boolean).join("\n");

let mode = "80x25 (text)";
let graphics = false;
emulator.add_listener("screen-set-size", (a) => {
    mode = `${a[0]}x${a[1]}`;
    /* A text screen is 80x25 or 80x50; anything else here is a game
       taking the display. */
    if (a[0] > 100)
        graphics = true;
});

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const type = async (text, wait) => {
    emulator.keyboard_send_text(text, 80);
    await sleep(wait);
};

console.log("[harness] booting, then playing a game the way a visitor would");

await sleep(32000);
await type("\n", 25000);            /* keep the default handle         */
await type("/quit\n", 12000);       /* out to a DOS prompt             */
rows.length = 0;                    /* forget the client's screen      */
await type("CD \\DL\\WAREZ\\GAMES\n", 5000);
await type("PLAY BLKDROP\n", seconds * 1000 - 74000);

const text = screenText();
console.log("\n--- the screen ---");
console.log(text || "(blank: a graphics mode has it)");
console.log(`--- video mode: ${mode} ---`);

try { emulator.destroy(); } catch { /* already gone */ }

let bad = 0;
const fail = (why) => { console.log(`FAIL: ${why}`); bad++; };

if (/Unpacking BLKDROP/.test(text) === false)
    fail("PLAY never got as far as unpacking the game");
if (/mouse/i.test(text))
    fail("the game complained about the mouse: CTMOUSE did not load");
if (/Fatal Error/i.test(text))
    fail("the game stopped with a fatal error");
if (/TZ environment/i.test(text))
    fail("UNZIP is still warning about TZ on every file");
if (!graphics)
    fail(`the display never left text mode (${mode}): the game did not `
         + "start");

if (bad === 0) {
    console.log("\nV86 PLAY OK: the game unpacked, found a mouse and took "
                + "the display.");
    console.log("What this cannot see is whether it is fun.");
    process.exit(0);
}
console.log("\nV86 PLAY FAILED");
process.exit(1);
