/* boot.mjs - boot the v86 browser host headlessly, in Node.
 *
 * The browser build was written against v86's source but could not be
 * verified without a browser -- and "written but never run" is not a status
 * worth shipping. Node has a global WebSocket now, which is all v86's Wisp
 * adapter needs, so the whole path can be exercised from the command line:
 *
 *   PCIPKT -> Watt-32 -> DOSy++ -> v86 ne2k -> Wisp -> relay -> ADC hub
 *
 * DOS output is captured over COM1 rather than by scraping the text screen,
 * which is why the data image carries AUTOTEST.BAT.
 *
 * Usage: node test/v86/boot.mjs <relay-url> <boot.img> <hda.img>
 *                                [timeout-s] [success-marker] [done-marker]
 *
 * The markers default to the login test's. The transfer gate passes its
 * own: there the guest's job is to serve a file, so "it logged in" is not
 * the verdict -- the CSND line in its peer trace is, and the host side
 * checks the bytes it received independently.
 */
import { readFileSync, statSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { installWorkerShim, partsConfig } from "./workershim.mjs";
import { fileURLToPath } from "node:url";
import { V86 } from "../../web/vendor/libv86.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(here, "../..");

const relayUrl = process.argv[2] || "wisp://localhost:8086/";
const bootImg = process.argv[3] || resolve(root, "web/images/boot.img");
const hdaImg = process.argv[4] || resolve(root, "web/images/dosypp.img");
const timeoutS = Number(process.argv[5] || 180);
const successMark = process.argv[6] || "PHASE 2 OK";
const doneMark = process.argv[7] || "AUTOTEST-DONE";

const vendor = resolve(root, "web/vendor");

/* A split image, if one was named instead of a plain one.
 *
 * Pass the manifest (dosypp.img.json) as the hda argument and this boots
 * from the zstd-compressed pieces that get published -- the same bytes a
 * browser would fetch, decompressed by v86's own worker through the shim
 * in workershim.mjs. Without this the rigs would only ever boot the
 * uncompressed image, and the published form would be untested. */
const partsManifest = hdaImg.endsWith(".json")
    ? JSON.parse(readFileSync(hdaImg, "utf8"))
    : null;

if (partsManifest)
    installWorkerShim();

/* Stream the hard disk instead of handing v86 the whole 64MB.
 *
 * This is how the published page loads it: a visitor should not wait for
 * 64MB before seeing a boot message, and v86 can read the image in blocks
 * instead -- over HTTP Range requests in a browser, and here, where v86
 * treats a URL as a path, over ranged reads of the file. Different
 * transport, same code path through v86's asynchronous block layer, which
 * is the part that could plausibly break. Off by default: the other gates
 * want the deterministic one. */
const asyncHda = process.env.V86_ASYNC_HDA === "1";

/* v86 wants ArrayBuffers for locally supplied images. */
const buffer = (p) => {
    const b = readFileSync(p);
    return b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength);
};

console.log(`[harness] relay   : ${relayUrl}`);
console.log(`[harness] boot.img: ${bootImg}`);
console.log(`[harness] hda     : ${hdaImg}`
    + (partsManifest
        ? ` (${partsManifest.parts} zstd pieces of `
          + `${partsManifest.chunk / 1024}KB, ${partsManifest.size} bytes `
          + "of disk)"
        : asyncHda ? " (streamed)" : ""));
console.log(`[harness] timeout : ${timeoutS}s`);

const emulator = new V86({
    wasm_path: resolve(vendor, "v86.wasm"),
    bios: { buffer: buffer(resolve(vendor, "seabios.bin")) },
    vga_bios: { buffer: buffer(resolve(vendor, "vgabios.bin")) },
    fda: { buffer: buffer(bootImg) },
    hda: partsManifest
        ? partsConfig(partsManifest, dirname(hdaImg))
        : asyncHda
            ? { url: hdaImg, async: true, size: statSync(hdaImg).size }
            : { buffer: buffer(hdaImg) },
    boot_order: 0x321,            /* low nibble first: 1 = floppy */
    memory_size: 16 * 1024 * 1024,
    vga_memory_size: 2 * 1024 * 1024,
    uart1: true,                  /* COM1, where AUTOTEST.BAT sends output */
    net_device: {
        type: "ne2k",
        relay_url: relayUrl,
        router_ip: "192.168.86.1",
        vm_ip: "192.168.86.100",
    },
    autostart: true,
    /* The SB16 device runs either way -- its DMA does not care whether
       anybody is listening -- and Node has no audio sink, so the speaker
       stays off. The sound gate reads the DMA counters, not a waveform. */
    disable_speaker: true,
});

let serial = "";
let settled = false;

const finish = (ok, why) => {
    if (settled) return;
    settled = true;

    console.log("\n--- COM1 output from DOS ---");
    console.log(serial.replace(/\r/g, "").trimEnd() || "(nothing)");
    console.log("----------------------------");
    console.log(`[harness] ${why}`);

    try {
        emulator.destroy();
    } catch { /* already gone */ }

    process.exit(ok ? 0 : 1);
};

emulator.add_listener("serial0-output-byte", (byte) => {
    serial += String.fromCharCode(byte);

    /* The success condition is DOSy++'s own verdict, not merely "it booted". */
    if (serial.includes(successMark))
        finish(true, `PASS: saw "${successMark}" on COM1`);

    /* Fail fast on the errors we know how to produce, rather than waiting
       out the timeout and reporting nothing useful. */
    if (/^failed: /m.test(serial))
        finish(false, "FAIL: DOSy++ ran but could not log in");
    if (serial.includes("TCP/IP init failed"))
        finish(false, "FAIL: Watt-32 could not start (packet driver?)");
    if (serial.includes("No IP address"))
        finish(false, "FAIL: no DHCP reply from v86's virtual router");
    if (serial.includes(doneMark) && !serial.includes(successMark))
        finish(false, `FAIL: the guest finished without "${successMark}"`);
});

emulator.add_listener("emulator-ready", () =>
    console.log("[harness] emulator ready, booting FreeDOS..."));

emulator.add_listener("download-error", (e) =>
    console.log(`[harness] download error: ${e.file_name}`));

/* Progress, so a hang is distinguishable from slow. */
let ticks = 0;
const ticker = setInterval(() => {
    ticks += 5;
    if (!settled)
        console.log(`[harness] ${ticks}s, ${serial.length} bytes from COM1`);
}, 5000);

setTimeout(() => {
    clearInterval(ticker);
    finish(false, `TIMEOUT after ${timeoutS}s`);
}, timeoutS * 1000);
