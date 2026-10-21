/* Unit tests for web/relayurl.mjs.
 *
 *   node test/web/test-relayurl.mjs
 *
 * Small, and worth having: the mistake it guards against (handing v86 a
 * ws:// URL, which selects a network adapter that is not Wisp) produces a
 * guest that boots, loads its packet driver and then cannot reach
 * anything -- with no error anywhere to say why.
 */
import { toWisp, toSocket, blockedAsMixed } from "../../web/relayurl.mjs";

let failed = 0;

const eq = (what, got, want) => {
    if (got === want) {
        console.log(`  ok    ${what}`);
    } else {
        console.log(`  FAIL  ${what}: got ${JSON.stringify(got)}, wanted ${JSON.stringify(want)}`);
        failed++;
    }
};

console.log("what v86 is given:");
eq("ws becomes wisp", toWisp("ws://127.0.0.1:8086/"), "wisp://127.0.0.1:8086/");
eq("wss becomes wisps", toWisp("wss://relay.example/"), "wisps://relay.example/");
eq("wisp is left alone", toWisp("wisp://127.0.0.1:8086/"), "wisp://127.0.0.1:8086/");
eq("wisps is left alone", toWisp("wisps://r.example/x"), "wisps://r.example/x");
eq("a bare host:port gets a scheme", toWisp("127.0.0.1:8086"), "wisp://127.0.0.1:8086");
eq("surrounding space is dropped", toWisp("  ws://a/  "), "wisp://a/");
eq("empty stays empty (no network)", toWisp(""), "");
eq("something else is left for v86 to judge",
   toWisp("http://x/"), "http://x/");

console.log("what the probe dials:");
eq("wisp -> ws", toSocket("wisp://127.0.0.1:8086/"), "ws://127.0.0.1:8086/");
eq("wisps -> wss", toSocket("wisps://r.example/"), "wss://r.example/");
eq("ws -> ws, round trip", toSocket("ws://127.0.0.1:8086/"), "ws://127.0.0.1:8086/");
eq("wss -> wss, round trip", toSocket("wss://r.example/"), "wss://r.example/");

console.log("mixed content, which blocks a boot with no useful error:");
eq("https page, remote plain socket", blockedAsMixed("https:", "ws://r.example/"), true);
eq("https page, loopback is exempt", blockedAsMixed("https:", "ws://127.0.0.1:8086/"), false);
eq("https page, localhost is exempt", blockedAsMixed("https:", "wisp://localhost:8086/"), false);
eq("https page, wss is fine", blockedAsMixed("https:", "wss://r.example/"), false);
eq("http page, anything goes", blockedAsMixed("http:", "ws://r.example/"), false);

console.log();
if (failed) {
    console.log(`${failed} check(s) failed`);
    process.exit(1);
}
console.log("RELAY URL OK: v86 gets a wisp:// URL whatever was typed.");
