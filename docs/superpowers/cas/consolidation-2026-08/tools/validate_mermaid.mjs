// validate_mermaid.mjs — parse stdin as one mermaid diagram; exit 1 on parse error.
import { JSDOM } from "jsdom";
const dom = new JSDOM("<!DOCTYPE html><body></body>");
globalThis.window = dom.window; globalThis.document = dom.window.document;
// Node >= 21 defines globalThis.navigator as a read-only getter; direct assignment
// throws, so redefine the property instead of assigning it.
Object.defineProperty(globalThis, "navigator", { value: dom.window.navigator, configurable: true });
globalThis.DOMPurify = { sanitize: s => s, addHook: () => {} };
const mermaid = (await import("mermaid")).default;
mermaid.initialize({ startOnLoad: false, securityLevel: "loose" });
let src = ""; for await (const c of process.stdin) src += c;
try { await mermaid.parse(src); process.exit(0); }
catch (e) { console.error(String(e).split("\n")[0]); process.exit(1); }
