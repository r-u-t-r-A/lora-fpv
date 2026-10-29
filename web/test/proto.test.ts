// SPDX-License-Identifier: GPL-3.0-or-later
import assert from "node:assert/strict";
import { test } from "node:test";
import { linkId, parseMessage, parseOta } from "../src/proto.ts";
import { codecFromSps, nals, rbsp } from "../src/hevc.ts";

test("link id matches the C implementation", () => {
  assert.equal(linkId("test"), 0x71e5); // FNV-1a("test") = 0xafd071e5
});

test("OTA header layout matches common/tests/test_ota.c", () => {
  const b = new Uint8Array([0x34, 0x12, 0xef, 0xbe, 7, 0x28, 0x02, 0x01, 3, 9, 8, 12]);
  const h = parseOta(b)!;
  assert.deepEqual(h, {
    linkId: 0x1234, seq: 0xbeef, hop: 7, type: 0, idr: false, eof: true, mode: 2,
    frame: 0x0102, block: 3, shard: 9, k: 8, n: 12,
  });
  assert.equal(parseOta(b.subarray(0, 11)), null);
});

test("records: RX_PACKET, STATUS, truncated tail", () => {
  const ota = new Uint8Array(20).fill(7);
  const trailer = new Uint8Array(8);
  const tv = new DataView(trailer.buffer);
  tv.setInt16(0, -97, true);
  tv.setInt8(2, -20);
  trailer[3] = 33;
  tv.setUint32(4, 0xdeadbeef, true);
  const status = new Uint8Array(28);
  status[0] = 3;
  status[2] = 1;
  new DataView(status.buffer).setInt16(20, -32768, true);
  const rec = (type: number, p: Uint8Array) => new Uint8Array([type, p.length & 255, p.length >> 8, ...p]);
  const msg = new Uint8Array([...rec(2, new Uint8Array([...ota, ...trailer])), ...rec(4, status), 2, 50, 0, 1]);
  const out = parseMessage(msg);
  assert.equal(out.length, 2);
  const r = out[0]!;
  assert.equal(r.kind, "rx");
  if (r.kind === "rx") {
    assert.equal(r.pkt.ota.length, 20);
    assert.equal(r.pkt.rssi, -97);
    assert.equal(r.pkt.snr, -5);
    assert.equal(r.pkt.hop, 33);
    assert.equal(r.pkt.tUs, 0xdeadbeef);
  }
  const s = out[1]!;
  assert.equal(s.kind, "status");
  if (s.kind === "status") {
    assert.equal(s.status.vrxId, 3);
    assert.equal(s.status.syncState, 1);
    assert.equal(s.status.noiseDbm, null);
  }
});

// SPS produced by x265 in sim/loopback (640x360, Main, level 2.1).
const SPS = "42010101600000030090000003000003003fa005020171f2e5ba4a4c2f016808000003000800000300f040";
const hex = (s: string) => new Uint8Array(s.match(/../g)!.map((x) => parseInt(x, 16)));

test("codec string from SPS", () => {
  assert.equal(codecFromSps(hex(SPS)), "hev1.1.6.L63.90");
});

test("emulation prevention removed", () => {
  assert.deepEqual(rbsp(new Uint8Array([0, 0, 3, 1, 0, 0, 3, 0])), new Uint8Array([0, 0, 1, 0, 0, 0]));
});

test("NAL iteration over 3- and 4-byte start codes", () => {
  const au = new Uint8Array([0, 0, 0, 1, 0x40, 1, 0xaa, 0, 0, 1, 0x42, 1, 0xbb, 0, 0, 0, 1, 0x02, 1, 0xcc]);
  const types = [...nals(au)].map((n) => n.type);
  assert.deepEqual(types, [32, 33, 1]);
  assert.deepEqual([...nals(au)][1]!.data, new Uint8Array([0x42, 1, 0xbb]));
});
