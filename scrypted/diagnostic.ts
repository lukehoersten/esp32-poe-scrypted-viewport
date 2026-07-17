// Paste into the diagnostic / probe Scrypted script. Save + Run.
// Tells us which Node/mDNS modules the scriptedEval sandbox lets us
// require() — drives whether the auto-discovery feature can use a
// proper library or has to fall back to raw multicast UDP.

declare const require: any;

console.log("------ scriptedEval require() probe ------");
for (const mod of [
    "dns",              // built-in; we already use this for dns.lookup
    "dgram",            // built-in; needed for raw mDNS multicast UDP
    "multicast-dns",
    "bonjour",
    "bonjour-service",
    "mdns",
]) {
    try {
        const m = require(mod);
        const top = Object.keys(m || {}).slice(0, 8).join(",");
        console.log(`  ${mod.padEnd(16)} -> ok; top keys: ${top}`);
    } catch (e: any) {
        console.log(`  ${mod.padEnd(16)} -> ${e?.code || "ERR"}: ${e?.message || e}`);
    }
}
