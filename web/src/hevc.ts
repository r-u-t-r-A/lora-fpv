// SPDX-License-Identifier: GPL-3.0-or-later
// Just enough H.265 parsing for WebCodecs: find NAL types and build the codec
// string ("hev1.<profile>.<compat>.<tier><level>.<constraints>") from the SPS.

export const NAL_VPS = 32;
export const NAL_SPS = 33;
export const NAL_PPS = 34;

export interface Nal {
  type: number;
  data: Uint8Array; // NAL header + payload, no start code
}

export function* nals(au: Uint8Array): Generator<Nal> {
  let i = 0;
  let start = -1;
  const n = au.length;
  while (i + 2 < n) {
    if (au[i] === 0 && au[i + 1] === 0 && au[i + 2] === 1) {
      if (start >= 0) yield mk(au.subarray(start, trimZeros(au, start, i)));
      i += 3;
      start = i;
    } else {
      i++;
    }
  }
  if (start >= 0 && start < n) yield mk(au.subarray(start, n));
}

function trimZeros(b: Uint8Array, from: number, to: number): number {
  while (to > from && b[to - 1] === 0) to--;
  return to;
}

function mk(data: Uint8Array): Nal {
  return { type: data.length ? (data[0]! >> 1) & 0x3f : -1, data };
}

export function isIrap(type: number): boolean {
  return type >= 16 && type <= 23;
}

// Removes emulation prevention bytes (00 00 03 -> 00 00).
export function rbsp(nal: Uint8Array): Uint8Array {
  const out = new Uint8Array(nal.length);
  let o = 0;
  let zeros = 0;
  for (const b of nal) {
    if (zeros >= 2 && b === 3) {
      zeros = 0;
      continue;
    }
    out[o++] = b;
    zeros = b === 0 ? zeros + 1 : 0;
  }
  return out.subarray(0, o);
}

function reverseBits32(x: number): number {
  let r = 0;
  for (let i = 0; i < 32; i++) {
    r = (r << 1) | (x & 1);
    x >>>= 1;
  }
  return r >>> 0;
}

// Codec string per ISO/IEC 14496-15 Annex E, from an SPS NAL (with header).
export function codecFromSps(sps: Uint8Array): string | null {
  const r = rbsp(sps);
  // 2 header bytes, then vps_id(4) max_sub_layers(3) nesting(1), then the
  // general profile_tier_level: 1 + 4 + 6 + 1 bytes.
  if (r.length < 2 + 1 + 12) return null;
  const p = r.subarray(3);
  const space = p[0]! >> 6;
  const tier = (p[0]! >> 5) & 1;
  const profile = p[0]! & 0x1f;
  const compat = ((p[1]! << 24) | (p[2]! << 16) | (p[3]! << 8) | p[4]!) >>> 0;
  const cons = Array.from(p.subarray(5, 11));
  while (cons.length && cons[cons.length - 1] === 0) cons.pop();
  const level = p[11]!;
  const spaceStr = ["", "A", "B", "C"][space] ?? "";
  const parts = [
    "hev1",
    `${spaceStr}${profile}`,
    reverseBits32(compat).toString(16).toUpperCase(),
    `${tier ? "H" : "L"}${level}`,
    ...cons.map((c) => c.toString(16).toUpperCase()),
  ];
  return parts.join(".");
}
