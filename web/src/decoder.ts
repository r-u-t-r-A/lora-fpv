// SPDX-License-Identifier: GPL-3.0-or-later
// H.265 decode with WebCodecs (Chrome on Linux uses VA-API). Frames come in
// already reassembled; a lost or partial frame is passed on (the decoder
// conceals what it can) unless the decoder needs a key frame.
import { codecFromSps, isIrap, nals, NAL_SPS } from "./hevc.ts";
import type { Frame } from "./reasm.ts";

export interface DecoderStats {
  codec: string | null;
  chunks: number;
  decoded: number;
  errors: number;
  waitingForKey: boolean;
  lastError: string | null;
}

export class HevcDecoder {
  private dec: VideoDecoder | null = null;
  private needKey = true;
  private unwrap = { last: -1, base: 0 };
  readonly stats: DecoderStats = { codec: null, chunks: 0, decoded: 0, errors: 0, waitingForKey: true, lastError: null };

  constructor(
    private readonly fps: () => number,
    private readonly onFrame: (f: VideoFrame) => void,
  ) {}

  static supported(): boolean {
    return typeof VideoDecoder !== "undefined";
  }

  private async configure(codec: string): Promise<void> {
    const cfg: VideoDecoderConfig = { codec, optimizeForLatency: true, hardwareAcceleration: "no-preference" };
    const sup = await VideoDecoder.isConfigSupported(cfg);
    if (!sup.supported) {
      this.stats.lastError = `${codec} not supported by this browser (enable VA-API HEVC, or wait for the libde265 fallback)`;
      return;
    }
    this.dec?.close();
    this.dec = new VideoDecoder({
      output: (f) => {
        this.stats.decoded++;
        this.onFrame(f);
      },
      error: (e) => {
        this.stats.errors++;
        this.stats.lastError = e.message;
        this.dec = null; // a closed decoder must be rebuilt
        this.needKey = true;
      },
    });
    this.dec.configure(cfg);
    this.stats.codec = codec;
  }

  private timestampUs(frame: number): number {
    // 16-bit frame counter -> monotonic microseconds.
    const u = this.unwrap;
    if (u.last >= 0 && frame < u.last && u.last - frame > 32768) u.base += 65536;
    u.last = frame;
    return Math.round(((u.base + frame) * 1e6) / this.fps());
  }

  async feed(f: Frame): Promise<void> {
    if (f.lost || f.data.length === 0) return;
    let sps: Uint8Array | null = null;
    let key = false;
    for (const n of nals(f.data)) {
      if (n.type === NAL_SPS) sps = n.data;
      if (isIrap(n.type)) key = true;
    }
    if (sps && (!this.dec || this.needKey)) {
      const codec = codecFromSps(sps);
      if (codec && (codec !== this.stats.codec || !this.dec)) await this.configure(codec);
    }
    if (!this.dec) return;
    // WebCodecs needs a key frame after configure(). With column intra
    // refresh there are none after the first; a frame carrying parameter sets
    // is the best restart point we have (see docs/protocol.md, open issues).
    const restart = key || (sps !== null && f.complete);
    if (this.needKey && !restart) {
      this.stats.waitingForKey = true;
      return;
    }
    this.needKey = false;
    this.stats.waitingForKey = false;
    this.stats.chunks++;
    try {
      this.dec.decode(
        new EncodedVideoChunk({ type: this.stats.chunks === 1 || restart ? "key" : "delta", timestamp: this.timestampUs(f.frame), data: f.data }),
      );
    } catch (e) {
      this.stats.errors++;
      this.stats.lastError = e instanceof Error ? e.message : String(e);
      this.needKey = true;
    }
  }
}
