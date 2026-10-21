/* Unit tests for web/fitscreen.mjs.
 *
 *   node test/web/test-fitscreen.mjs
 *
 * The emulated screen changes size whenever the guest changes video mode,
 * and the page has to put whatever arrives inside a fixed bezel. Doom's
 * 320x200 was drawn at its own size in a bezel 1750 wide and clipped;
 * these are the numbers that stop that happening again.
 */
import { fitScale, fitBox, MAX_SCALE } from "../../web/fitscreen.mjs";

let failed = 0;
const near = (a, b) => Math.abs(a - b) < 0.001;
const ok = (what, cond) => {
    console.log(`  ${cond ? "ok  " : "FAIL"}  ${what}`);
    if (!cond) failed++;
};

console.log("filling the width when there is height to spare:");
ok("text mode 720x400 in an 880 bezel",
   near(fitScale(720, 400, 880, 900), 880 / 720));
ok("Doom 320x200 in an 880 bezel",
   near(fitScale(320, 200, 880, 900), 880 / 320));

console.log("height is the limit for a tall window-less page:");
ok("320x200 with only 300px of height",
   near(fitScale(320, 200, 1750, 300), 300 / 200));
ok("and the result fits", fitBox(320, 200, fitScale(320, 200, 1750, 300))
   .height <= 300);

console.log("a very wide bezel does not blow the pixels up for ever:");
ok("capped at MAX_SCALE", fitScale(320, 200, 4000, 4000) === MAX_SCALE);

console.log("shrinking when the bezel is small:");
ok("720x400 in a 360 bezel halves it",
   near(fitScale(720, 400, 360, 900), 0.5));

console.log("nothing to measure yet:");
for (const args of [[0, 0, 880, 600], [320, 200, 0, 600],
                    [320, 200, 880, 0]])
    ok(`${JSON.stringify(args)} -> 1`, fitScale(...args) === 1);

console.log("the box the page reserves:");
{
    const s = fitScale(320, 200, 880, 900);
    const b = fitBox(320, 200, s);
    ok("as wide as the bezel", b.width === 880);
    ok("in proportion", b.height === 550);
}

console.log();
if (failed) {
    console.log(`${failed} check(s) failed`);
    process.exit(1);
}
console.log("FITSCREEN OK: whatever the guest picks, it fits the bezel.");
