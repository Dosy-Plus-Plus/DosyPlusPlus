/* relayurl.mjs - what to call the relay, and why it matters.
 *
 * v86 chooses its network adapter from the SCHEME of relay_url, and it is
 * not a guess it makes well: only `wisp://` and `wisps://` select the Wisp
 * adapter. A `ws://` URL silently selects the old plain adapter, which
 * speaks raw Ethernet frames over the socket instead of Wisp streams -- so
 * the guest boots, loads its packet driver, sends ARP into a void and looks
 * like a network problem. Nothing logs a complaint.
 *
 * Public Wisp relays, meanwhile, are published as `wss://` URLs, which is
 * what somebody will paste. So the page normalises instead of insisting:
 *
 *   ws://host/   -> wisp://host/      (what v86 needs)
 *   wss://host/  -> wisps://host/
 *   wisp://host/ -> unchanged
 *
 * and turns it back the other way for the reachability probe, which uses a
 * plain WebSocket.
 *
 * Tested by test/web/test-relayurl.mjs, because this is a one-character
 * mistake with a symptom that points somewhere else entirely.
 */

/** The URL to hand v86 as relay_url. "" for an empty input. */
export function toWisp(url) {
    const u = (url || "").trim();
    if (!u) return "";
    if (u.startsWith("wisps://") || u.startsWith("wisp://")) return u;
    if (u.startsWith("wss://")) return "wisps://" + u.slice(6);
    if (u.startsWith("ws://")) return "wisp://" + u.slice(5);
    /* No scheme at all: assume the plain case rather than refusing. A
       host:port is what people type. */
    if (!/^[a-z][a-z0-9+.-]*:\/\//i.test(u)) return "wisp://" + u;
    return u;                       /* something else; let v86 judge it */
}

/** The same endpoint as a WebSocket URL, for a reachability probe. */
export function toSocket(url) {
    const u = toWisp(url);
    if (u.startsWith("wisps://")) return "wss://" + u.slice(8);
    if (u.startsWith("wisp://")) return "ws://" + u.slice(7);
    return u;
}

/** True when this page cannot open that socket for mixed-content reasons.
 *  Loopback is exempt from mixed-content blocking, so it is not counted --
 *  though a browser may still ask about local-network access. */
export function blockedAsMixed(pageProtocol, url) {
    if (pageProtocol !== "https:") return false;
    const s = toSocket(url);
    if (!s.startsWith("ws://")) return false;
    return !/^ws:\/\/(127\.0\.0\.1|localhost|\[::1\])(:|\/|$)/.test(s);
}
