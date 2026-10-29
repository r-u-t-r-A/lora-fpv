// SPDX-License-Identifier: GPL-3.0-or-later
// Runs a sim/loopback capture through the real WASM reassembler. Needs
// public/olv.wasm (npm run wasm) and OLV_VRXLOG=<dir> with rx1.vrxlog and
// rx2.vrxlog from `loopback --out <dir>`; skipped otherwise.
import assert from "node:assert/strict";
import { existsSync, readFileSync } from "node:fs";
import { test } from "node:test";
import { linkId, parseOta, OTA_TYPE_VIDEO, RX_TRAILER } from "../src/proto.ts";
import { PUSH_NEW, Reassembler } from "../src/reasm.ts";

const wasmPath = new URL("../public/olv.wasm", import.meta.url);
const dir = process.env["OLV_VRXLOG"];

interface Rec { t: number; ota: Uint8Array }

function readLog(path: string): Rec[] {
  const b = readFileSync(path);
  assert.equal(b.subarray(0, 4).toString(), "OLVL");
  const v = new DataView(b.buffer, b.byteOffset, b.byteLength);
  const out: Rec[] = [];
  for (let p = 8; p + 10 <= b.length; ) {
    const t = v.getUint32(p, true) + v.getUint32(p + 4, true) * 2 ** 32;
    const len = v.getUint16(p + 8, true);
    out.push({ t, ota: new Uint8Array(b.subarray(p + 10, p + 10 + len - RX_TRAILER)) });
    p += 10 + len;
  }
  return out;
}

test("WASM reassembler merges two receivers", { skip: !existsSync(wasmPath) || !dir }, async () => {
  const r = await Reassembler.fromBytes(readFileSync(wasmPath));
  r.init(linkId("open-lr-video"), 2, 0);
  const a = readLog(`${dir}/rx1.vrxlog`), b = readLog(`${dir}/rx2.vrxlog`);
  const all = [...a.map((x) => ({ ...x, rx: 0 })), ...b.map((x) => ({ ...x, rx: 1 }))].sort((x, y) => x.t - y.t);
  const first = [0, 0];
  let frames = 0, complete = 0;
  for (const p of all) {
    const h = parseOta(p.ota);
    if (!h || h.type !== OTA_TYPE_VIDEO) continue;
    if (r.push(p.ota, p.t) === PUSH_NEW) first[p.rx]!++;
    for (const f of r.frames()) {
      frames++;
      if (f.complete) complete++;
      if (!f.lost) assert.ok(f.data.length > 0 || !f.complete);
    }
  }
  assert.ok(frames > 30, `frames ${frames}`);
  assert.ok(complete / frames > 0.85, `complete ${complete}/${frames}`);
  assert.ok(first[0]! > 0 && first[1]! > 0, "both receivers contributed");
  const s = r.stats();
  assert.equal(s.bad, 0);
});
