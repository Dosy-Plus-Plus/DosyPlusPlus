/* splitimg.mjs - publish a disk image as compressed pieces.
 *
 *   node tools/splitimg.mjs <image> <outdir> [chunk-bytes]
 *
 * A 128MB FAT16 image with 6MB of files in it is 122MB of zeros, and a
 * static host has to store and serve every one of them. v86 can read an
 * image as separate per-chunk files instead -- that is its `use_parts`
 * mode -- and when the extension ends in .zst it decompresses each piece
 * with the zstd it already carries in v86.wasm.
 *
 * So the published form is a few hundred small files, of which the empty
 * ones cost about thirty bytes each. Measured on this project's image:
 * 128MB becomes about 9MB. It also means the browser fetches whole small
 * files rather than byte ranges, so the demo no longer depends on the host
 * honouring Range requests.
 *
 * The naming is v86's, not ours (libv86.mjs):
 *
 *     basename + "-" + start + "-" + end + extension
 *
 * where the extension is everything from the last dot before any `.zst`,
 * so `dosypp.img.zst` yields `dosypp-0-1048576.img.zst`. Get it wrong and
 * every fetch 404s with no explanation, which is why this writes a
 * manifest and the rig checks the first and last piece by name.
 */
import { createReadStream } from "node:fs";
import { mkdir, writeFile, stat } from "node:fs/promises";
import { basename, join } from "node:path";
import { constants, zstdCompressSync } from "node:zlib";

const [, , imagePath, outDir, chunkArg] = process.argv;

if (!imagePath || !outDir) {
    console.error("usage: splitimg.mjs <image> <outdir> [chunk-bytes]");
    process.exit(2);
}

/* 1MB: small enough that booting DOS touches a handful of pieces, big
   enough that a quiet disk does not become ten thousand files. */
const CHUNK = Number(chunkArg || 1024 * 1024);

const name = basename(imagePath);                    /* dosypp.img      */
const stem = name.replace(/\.[^.]+$/, "");           /* dosypp          */
const ext = name.slice(stem.length) + ".zst";        /* .img.zst        */

const { size } = await stat(imagePath);
await mkdir(outDir, { recursive: true });

let written = 0;
let bytesOut = 0;
let empty = 0;

const stream = createReadStream(imagePath, { highWaterMark: CHUNK });
let carry = Buffer.alloc(0);
let offset = 0;

async function emit(buf) {
    /* Every piece is exactly CHUNK long except, possibly, the last: v86
       reads a whole chunk at a time and writes it into a buffer of that
       size. A short piece in the middle would silently shift the image. */
    const start = offset;
    const end = start + buf.length;
    const out = join(outDir, `${stem}-${start}-${start + CHUNK}${ext}`);

    const packed = zstdCompressSync(buf, {
        params: { [constants.ZSTD_c_compressionLevel]: 19 },
    });
    await writeFile(out, packed);

    written++;
    bytesOut += packed.length;
    if (packed.length < 200)
        empty++;
    offset = end;
}

for await (const piece of stream) {
    carry = carry.length ? Buffer.concat([carry, piece]) : piece;

    while (carry.length >= CHUNK) {
        await emit(carry.subarray(0, CHUNK));
        carry = carry.subarray(CHUNK);
    }
}
if (carry.length)
    await emit(carry);

/* What the page needs to know before it can ask for any of this: how big
   the disk is and how the pieces are cut. Without it the page would have
   to guess, and a wrong size is a corrupt filesystem. */
const manifest = {
    image: `${stem}${ext}`,
    size,
    chunk: CHUNK,
    parts: written,
    bytes: bytesOut,
    created: new Date().toISOString().replace(/\.\d+Z$/, "Z"),
};
await writeFile(join(outDir, `${stem}.img.json`),
                JSON.stringify(manifest, null, 2) + "\n");

const pct = ((bytesOut / size) * 100).toFixed(1);
console.log(`  ${name}: ${(size / 1048576).toFixed(0)}MB -> ${written} `
            + `pieces, ${(bytesOut / 1048576).toFixed(1)}MB (${pct}%), `
            + `${empty} of them empty`);
console.log(`  manifest: ${stem}.img.json`);
