/* Unit tests for web/disk.mjs.
 *
 *   node test/web/test-disk.mjs
 *
 * This exists because of a specific failure: the published bundle has no
 * single-file disk image, and the page asked for one anyway -- so the demo
 * could not boot at all, and the only symptom was "Download failed:
 * images/dosypp.img". The logic is four lines; the consequence of getting
 * it wrong is everything.
 */
import { chooseDisk } from "../../web/disk.mjs";

let failed = 0;
const eq = (what, got, want) => {
    if (got === want) {
        console.log(`  ok    ${what}`);
    } else {
        console.log(`  FAIL  ${what}: got ${JSON.stringify(got)}, wanted `
                    + JSON.stringify(want));
        failed++;
    }
};

const MANIFEST = {
    image: "dosypp.img.zst",
    size: 134217728,
    chunk: 1048576,
    parts: 128,
    bytes: 8857995,
};

console.log("with the pieces published, they are the disk:");
for (const streamed of [true, false]) {
    const d = chooseDisk(MANIFEST, streamed);
    eq(`use_parts (stream box ${streamed})`, d.use_parts, true);
    eq(`the url is the manifest's image`, d.url, "images/dosypp.img.zst");
    eq(`async, because pieces are fetched as needed`, d.async, true);
    eq(`the size v86 needs`, d.size, 134217728);
    eq(`the chunk size v86 needs`, d.fixed_chunk_size, 1048576);
}

console.log("and specifically NOT the single file, which is not published:");
{
    /* The bug: the stream box is off by default on localhost, and that
       used to decide whether the manifest was consulted at all. */
    const d = chooseDisk(MANIFEST, false);
    eq("never asks for dosypp.img", d.url.endsWith("dosypp.img"), false);
}

console.log("with no manifest, the visitor's preference decides:");
{
    const streamed = chooseDisk(null, true);
    eq("streamed: one file, async", streamed.url, "images/dosypp.img");
    eq("streamed: async", streamed.async, true);
    eq("streamed: not parts", !!streamed.use_parts, false);

    const whole = chooseDisk(null, false);
    eq("whole: one file", whole.url, "images/dosypp.img");
    eq("whole: not async", whole.async, false);
}

console.log("a manifest that does not describe a disk is not a manifest:");
for (const bad of [{}, { image: "x" }, { image: "x", size: 1 },
                   { size: 1, chunk: 2 }]) {
    const d = chooseDisk(bad, false);
    eq(`${JSON.stringify(bad)} falls back`, d.form, "whole");
}

console.log("the page says which form it got:");
eq("pieces are described", chooseDisk(MANIFEST, false).describe,
   "128 compressed pieces, 8.4MB for a 128MB drive");
eq("so is a single file", chooseDisk(null, true).form, "streamed");

console.log();
if (failed) {
    console.log(`${failed} check(s) failed`);
    process.exit(1);
}
console.log("DISK OK: the pieces win whenever they exist.");
