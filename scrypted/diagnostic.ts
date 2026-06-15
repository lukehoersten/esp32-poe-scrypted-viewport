// Paste this into a NEW Scrypted Script (Scripts UI → New). Save + Run.
// The console output tells us exactly which names the Scrypted-eval
// sandbox is exposing on this Scrypted/core version, which guides what
// the real `scrypted-viewport.ts` should reference.
//
// `declare const ...: any` lines below exist only to keep the editor's
// TypeScript happy; they fully erase at runtime, and the `typeof` checks
// look up whatever the runtime injects (or undefined if it doesn't).

declare const sdk:                 any;
declare const ScryptedDeviceBase:  any;
declare const ScryptedDeviceType:  any;
declare const ScryptedInterface:   any;
declare const systemManager:       any;
declare const deviceManager:       any;
declare const mediaManager:        any;
declare const endpointManager:     any;
declare const log:                 any;
declare const device:              any;
declare const require:             any;
declare const exports:             any;

console.log("------ Scrypted scriptedEval scope probe ------");
console.log("sdk                 ->", typeof sdk);
console.log("ScryptedDeviceBase  ->", typeof ScryptedDeviceBase);
console.log("ScryptedDeviceType  ->", typeof ScryptedDeviceType);
console.log("ScryptedInterface   ->", typeof ScryptedInterface);
console.log("systemManager       ->", typeof systemManager);
console.log("deviceManager       ->", typeof deviceManager);
console.log("mediaManager        ->", typeof mediaManager);
console.log("endpointManager     ->", typeof endpointManager);
console.log("log                 ->", typeof log);
console.log("device              ->", typeof device);
console.log("require             ->", typeof require);
console.log("exports             ->", typeof exports);

if (typeof sdk !== "undefined" && sdk) {
    const keys = Object.keys(sdk);
    console.log("sdk has", keys.length, "keys; first 25:", keys.slice(0, 25).join(","));
    console.log("sdk.ScryptedDeviceBase ->", typeof sdk.ScryptedDeviceBase);
    console.log("sdk.systemManager      ->", typeof sdk.systemManager);
}

// Try the "require Node builtin" path too — older Scripts plugin builds
// gate `require` differently than the SDK names.
try {
    const dns = require("dns");
    console.log("require('dns')      -> ok, has", typeof dns.promises === "object" ? "promises" : "(no .promises)");
} catch (e: any) {
    console.log("require('dns')      -> ERR:", e && e.message);
}
