// SPDX-License-Identifier: GPL-3.0-or-later
// VRX -> browser records and the OTA header. Mirrors common/vrx_proto.h and
// common/ota.h; see docs/protocol.md.

export const OTA_HDR_LEN = 12;
export const RX_TRAILER = 8;

export const REC_RX_PACKET = 2;
export const REC_STATUS = 4;
export const REC_LOG = 5;

export const OTA_TYPE_VIDEO = 0;
export const OTA_TYPE_TELEM = 1;
export const OTA_TYPE_BEACON = 2;

export interface OtaHdr {
  linkId: number;
  seq: number;
  hop: number;
  type: number;
  idr: boolean;
  eof: boolean;
  mode: number;
  frame: number;
  block: number;
  shard: number;
  k: number;
  n: number;
}

export interface RxPacket {
  ota: Uint8Array;
  rssi: number; // dBm
  snr: number; // dB
  hop: number;
  tUs: number;
}

export interface VrxStatus {
  vrxId: number;
  band: number;
  syncState: number; // 0 parked, 1 locked, 2 coasting
  hop: number;
  cycle: number;
  pktsOk: number;
  pktsCrc: number;
  pktsDrop: number;
  noiseDbm: number | null;
  locks: number;
  uptimeMs: number;
}

export type VrxRecord =
  | { kind: "rx"; pkt: RxPacket }
  | { kind: "status"; status: VrxStatus }
  | { kind: "log"; text: string };

export function parseOta(b: Uint8Array): OtaHdr | null {
  if (b.length < OTA_HDR_LEN) return null;
  const v = new DataView(b.buffer, b.byteOffset, b.byteLength);
  const flags = b[5]!;
  return {
    linkId: v.getUint16(0, true),
    seq: v.getUint16(2, true),
    hop: b[4]!,
    type: flags & 3,
    idr: (flags & 4) !== 0,
    eof: (flags & 8) !== 0,
    mode: (flags >> 4) & 3,
    frame: v.getUint16(6, true),
    block: b[8]!,
    shard: b[9]!,
    k: b[10]!,
    n: b[11]!,
  };
}

function parseRx(p: Uint8Array): RxPacket | null {
  if (p.length < RX_TRAILER || p.length - RX_TRAILER > 255) return null;
  const t = p.length - RX_TRAILER;
  const v = new DataView(p.buffer, p.byteOffset, p.byteLength);
  return {
    ota: p.subarray(0, t),
    rssi: v.getInt16(t, true),
    snr: v.getInt8(t + 2) / 4,
    hop: p[t + 3]!,
    tUs: v.getUint32(t + 4, true),
  };
}

function parseStatus(p: Uint8Array): VrxStatus | null {
  if (p.length !== 28) return null;
  const v = new DataView(p.buffer, p.byteOffset, p.byteLength);
  const noise = v.getInt16(20, true);
  return {
    vrxId: p[0]!,
    band: p[1]!,
    syncState: p[2]!,
    hop: p[3]!,
    cycle: v.getUint32(4, true),
    pktsOk: v.getUint32(8, true),
    pktsCrc: v.getUint32(12, true),
    pktsDrop: v.getUint32(16, true),
    noiseDbm: noise === -32768 ? null : noise,
    locks: v.getUint16(22, true),
    uptimeMs: v.getUint32(24, true),
  };
}

const utf8 = new TextDecoder();

// Parses one WebSocket message (one or more records). Unknown or malformed
// records are skipped.
export function parseMessage(msg: Uint8Array): VrxRecord[] {
  const out: VrxRecord[] = [];
  let pos = 0;
  while (pos + 3 <= msg.length) {
    const type = msg[pos]!;
    const len = msg[pos + 1]! | (msg[pos + 2]! << 8);
    if (pos + 3 + len > msg.length) break;
    const p = msg.subarray(pos + 3, pos + 3 + len);
    pos += 3 + len;
    if (type === REC_RX_PACKET) {
      const pkt = parseRx(p);
      if (pkt) out.push({ kind: "rx", pkt });
    } else if (type === REC_STATUS) {
      const status = parseStatus(p);
      if (status) out.push({ kind: "status", status });
    } else if (type === REC_LOG) {
      out.push({ kind: "log", text: utf8.decode(p) });
    }
  }
  return out;
}

// FNV-1a 32 of the bind phrase; the link id is the low 16 bits.
export function linkId(bind: string): number {
  let h = 2166136261;
  for (const c of new TextEncoder().encode(bind)) {
    h ^= c;
    h = Math.imul(h, 16777619) >>> 0;
  }
  return h & 0xffff;
}
