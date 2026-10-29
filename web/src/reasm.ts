// SPDX-License-Identifier: GPL-3.0-or-later
// Wraps web/wasm/olv_wasm.c: the same reassembler + Reed-Solomon code that
// the simulator tests, compiled to WebAssembly.

export const PUSH_NEW = 0;
export const PUSH_DUP = 1;
export const PUSH_LATE = 2;
export const PUSH_BAD = 3;
export const PUSH_IGNORED = 4;

export interface Frame {
  frame: number;
  data: Uint8Array; // Annex B access unit (copy), empty if lost
  lost: boolean;
  complete: boolean;
  hasIdr: boolean;
  blocks: number;
  blocksOk: number;
}

export interface ReasmStats {
  pkts: number;
  newShards: number;
  dups: number;
  late: number;
  bad: number;
  blocksOk: number;
  blocksLost: number;
  framesComplete: number;
  framesPartial: number;
  framesLost: number;
  restarts: number;
  queueDropped: number;
}

interface Exports {
  memory: WebAssembly.Memory;
  olv_rx_init(linkId: number, maxWait: number, timeoutUs: number): number;
  olv_in_buf(): number;
  olv_rx_push(len: number, nowUs: number): number;
  olv_rx_tick(nowUs: number): void;
  olv_rx_flush(): void;
  olv_frame_count(): number;
  olv_frame_data(): number;
  olv_frame_len(): number;
  olv_frame_info(): number;
  olv_frame_blocks(): number;
  olv_frame_pop(): void;
  olv_stats(): number;
  _initialize?: () => void;
}

// Standalone Emscripten output may import a few WASI/env functions we never
// call on this path; give it harmless stubs.
function stubImports(mod: WebAssembly.Module): WebAssembly.Imports {
  const imports: Record<string, Record<string, WebAssembly.ImportValue>> = {};
  for (const imp of WebAssembly.Module.imports(mod)) {
    const ns = (imports[imp.module] ??= {});
    if (imp.kind === "function") ns[imp.name] = () => 0;
  }
  return imports;
}

export class Reassembler {
  private constructor(private readonly x: Exports) {}

  static async load(url: string): Promise<Reassembler> {
    const resp = await fetch(url);
    if (!resp.ok) throw new Error(`${url}: HTTP ${resp.status}`);
    return Reassembler.fromBytes(await resp.arrayBuffer());
  }

  static async fromBytes(bytes: BufferSource): Promise<Reassembler> {
    const mod = await WebAssembly.compile(bytes);
    const inst = await WebAssembly.instantiate(mod, stubImports(mod));
    const x = inst.exports as unknown as Exports;
    x._initialize?.();
    return new Reassembler(x);
  }

  init(linkId: number, maxWaitFrames: number, timeoutUs: number): void {
    if (this.x.olv_rx_init(linkId, maxWaitFrames, timeoutUs) !== 0)
      throw new Error("reassembler init failed");
  }

  push(ota: Uint8Array, nowUs: number): number {
    new Uint8Array(this.x.memory.buffer, this.x.olv_in_buf(), 512).set(ota);
    return this.x.olv_rx_push(ota.length, nowUs >>> 0);
  }

  tick(nowUs: number): void {
    this.x.olv_rx_tick(nowUs >>> 0);
  }

  *frames(): Generator<Frame> {
    while (this.x.olv_frame_count() > 0) {
      const info = this.x.olv_frame_info();
      const len = this.x.olv_frame_len();
      const ptr = this.x.olv_frame_data();
      const data = len ? new Uint8Array(this.x.memory.buffer, ptr, len).slice() : new Uint8Array();
      const flags = (info >>> 16) & 7;
      const f: Frame = {
        frame: info & 0xffff,
        data,
        lost: (flags & 1) !== 0,
        complete: (flags & 2) !== 0,
        hasIdr: (flags & 4) !== 0,
        blocks: this.x.olv_frame_blocks(),
        blocksOk: info >>> 24,
      };
      this.x.olv_frame_pop();
      yield f;
    }
  }

  stats(): ReasmStats {
    const s = new Uint32Array(this.x.memory.buffer, this.x.olv_stats(), 12);
    return {
      pkts: s[0]!,
      newShards: s[1]!,
      dups: s[2]!,
      late: s[3]!,
      bad: s[4]!,
      blocksOk: s[5]!,
      blocksLost: s[6]!,
      framesComplete: s[7]!,
      framesPartial: s[8]!,
      framesLost: s[9]!,
      restarts: s[10]!,
      queueDropped: s[11]!,
    };
  }
}
